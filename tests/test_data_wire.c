// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "core/data_wire.h"
#include "core/data_wire_import.h"
#include "module/blake3.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "data wire check failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

typedef struct {
    jdv_value_t item;
    jdv_schema_t schema;
    jdv_stream_end_t end;
    int index;
    int releases;
} producer_t;

static jdv_stream_pull_result_t pull(void* context, jdv_stream_record_t* record) {
    producer_t* producer = context;
    if (!producer->index++) {
        record->item = &producer->item;
        record->logical_bytes = 1;
        return JDV_PULL_ITEM;
    }
    record->terminal = &producer->end;
    return JDV_PULL_END;
}

static jdv_stream_pull_result_t cancelled_pull(void* context,
                                               jdv_stream_record_t* record) {
    record->terminal = &((producer_t*)context)->end;
    return JDV_PULL_ERROR;
}

static int cancel(void* context, jdv_slice_t reason) {
    (void)context; (void)reason;
    return 0;
}

static void release(void* context) { ((producer_t*)context)->releases++; }

typedef struct {
    uint8_t bytes[2048];
    size_t used;
    int interrupted;
    int fail;
    size_t fail_after;
} sink_t;

static long sink(void* context, const void* data, size_t size) {
    sink_t* output = context;
    if (!output->interrupted++) { errno = EINTR; return -1; }
    if (output->fail ||
        (output->fail_after && output->used >= output->fail_after)) {
        errno = EPIPE;
        return -1;
    }
    if (size > 3) size = 3;
    if (size > sizeof(output->bytes) - output->used) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(output->bytes + output->used, data, size);
    output->used += size;
    return (long)size;
}

static uint64_t read_u64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | bytes[i];
    return value;
}

static void put_u64(uint8_t bytes[8], uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        bytes[7u - i] = (uint8_t)(value >> (i * 8u));
}

static int observe_item(void* context, const jdv_value_t* value) {
    if (value->kind != JDV_U64 || value->as.u64 != 17) {
        errno = EPROTO;
        return -1;
    }
    (*(int*)context)++;
    return 0;
}

int main(void) {
    producer_t producer = {0};
    producer.item = jdv_value_u64(17);
    CHECK(jdv_schema_scalar(JDV_U64, 0, &producer.schema) == 0);
    CHECK(jdv_stream_end_init(&producer.end) == 0);
    producer.end.state = JDV_STREAM_COMPLETE;
    producer.end.items_seen = 1;
    producer.end.bytes_seen = 1;
    producer.end.completeness = JDV_COMPLETENESS_COMPLETE;
    producer.end.final_schema = &producer.schema;
    producer.end.final_hash_present = 1;
    producer.end.final_hash_algorithm = JDV_HASH_BLAKE3;
    uint8_t item_hash[JDV_HASH_SIZE];
    CHECK(jdv_hash(&producer.item, item_hash) == 0);
    struct blake3 digest;
    blake3_init(&digest);
    blake3_update(&digest, "JTV1-ITEMS-v1",
                  sizeof("JTV1-ITEMS-v1") - 1u);
    blake3_update(&digest, item_hash, sizeof(item_hash));
    blake3_final(&digest, producer.end.final_hash, JDV_HASH_SIZE);
    static const jdv_stream_producer_vtable_t vtable = {
        JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
        pull, cancel, release
    };
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.declared_schema = config.guaranteed_schema = &producer.schema;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config, &vtable, &producer) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);

    uint8_t workspace[1024];
    sink_t output = {0};
    jdv_stream_end_t terminal;
    int observed_items = 0;
    CHECK(jdv_stream_write_frames_observed(&cursor, sink, &output, workspace,
                                            sizeof(workspace), observe_item,
                                            &observed_items, &terminal) == 0);
    CHECK(observed_items == 1);
    CHECK(terminal.state == JDV_STREAM_COMPLETE);
    CHECK(output.used > 4 && !memcmp(output.bytes, "JTV1", 4));
    CHECK(memcmp(output.bytes, "JDS1", 4) != 0);
    const uint8_t expected[] = {'D', 'G', 'I', 'F', 'T'};
    size_t offset = 4;
    size_t terminal_payload = 0, item_payload = 0;
    for (size_t i = 0; i < sizeof(expected); i++) {
        CHECK(offset + 9 <= output.used);
        CHECK(output.bytes[offset] == expected[i]);
        uint64_t length = read_u64(output.bytes + offset + 1);
        offset += 9;
        CHECK(length <= output.used - offset);
        if (expected[i] == 'I') item_payload = offset;
        if (expected[i] == 'T') {
            terminal_payload = offset;
            CHECK(length == 96);
            CHECK(output.bytes[offset] == JDV_STREAM_COMPLETE);
            CHECK(read_u64(output.bytes + offset + 8) == 1);
        }
        offset += (size_t)length;
    }
    CHECK(offset == output.used);
    uint8_t complete[sizeof(output.bytes)];
    memcpy(complete, output.bytes, output.used);
    size_t complete_size = output.used;
    uint8_t arena_storage[4096];
    jdv_arena_t arena;
    jdv_arena_init(&arena, arena_storage, sizeof(arena_storage));
    jdv_wire_limits_t limits;
    jdv_wire_limits_init(&limits);
    jdv_wire_info_t verified = {0};
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &verified) == 0);
    CHECK(verified.state == JDV_STREAM_COMPLETE && verified.items_seen == 1);
    CHECK(verified.bytes_seen == 1 && verified.errors_seen == 0);
    CHECK(!memcmp(verified.final_hash, producer.end.final_hash, JDV_HASH_SIZE));
    CHECK(arena.used == 0);

    /* JTV1 v1 has no per-item logical credit: the terminal byte total remains
     * a producer claim. A generic D05 importer must not invent item deltas. */
    put_u64(complete + terminal_payload + 16, 2);
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &verified) == 0);
    CHECK(verified.items_seen == 1 && verified.bytes_seen == 2);
    put_u64(complete + terminal_payload + 16, 1);

    jdv_wire_info_t unchanged = {.items_seen = 123};
    CHECK(jdv_wire_validate(complete, complete_size - 1, &arena,
                            &limits, &unchanged) < 0);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    complete[0] = 'S'; /* JDS1 (job stream) cannot be imported as JTV1. */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    complete[0] = 'J';
    CHECK(terminal_payload && item_payload);
    complete[4] = 'G'; /* required D/G order */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[4] = 'D';
    complete[13] = 'X'; /* non-JSC1 declared schema */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[13] = 'J';
    complete[terminal_payload + 5] = 1; /* reserved terminal byte */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[terminal_payload + 5] = 0;
    CHECK(complete_size < sizeof(complete));
    complete[complete_size] = 0; /* trailing bytes after terminal */
    CHECK(jdv_wire_validate(complete, complete_size + 1, &arena,
                            &limits, &unchanged) < 0);
    complete[terminal_payload + 15] ^= 1; /* reported item count */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[terminal_payload + 15] ^= 1;
    complete[item_payload + 12] ^= 1; /* canonical but not terminal-hashed */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[item_payload + 12] ^= 1;
    complete[item_payload + 4] = JDV_I64; /* matching hash, wrong schema */
    jdv_value_t wrong_type = jdv_value_i64(17);
    CHECK(jdv_hash(&wrong_type, item_hash) == 0);
    blake3_init(&digest);
    blake3_update(&digest, "JTV1-ITEMS-v1",
                  sizeof("JTV1-ITEMS-v1") - 1u);
    blake3_update(&digest, item_hash, sizeof(item_hash));
    blake3_final(&digest, complete + terminal_payload + 56,
                  JDV_HASH_SIZE);
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    complete[item_payload + 4] = JDV_U64;
    memcpy(complete + terminal_payload + 56,
           producer.end.final_hash, JDV_HASH_SIZE);
    complete[5] = 0xff; /* absurd, untrusted frame length */
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    complete[5] = output.bytes[5];
    limits.max_wire_bytes = complete_size - 1;
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0 && errno == E2BIG);
    limits.max_wire_bytes = UINT64_C(64) * 1024u * 1024u;
    limits.max_frame_bytes = 2;
    CHECK(jdv_wire_validate(complete, complete_size, &arena,
                            &limits, &unchanged) < 0 && errno == E2BIG);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    limits.max_frame_bytes = 16u * 1024u * 1024u;
    jdv_arena_t alias;
    jdv_arena_init(&alias, complete, complete_size);
    CHECK(jdv_wire_validate(complete, complete_size, &alias,
                            &limits, &unchanged) < 0 && errno == EINVAL);
    CHECK(unchanged.items_seen == 123 && alias.used == 0);

    uint64_t item_size = read_u64(complete + item_payload - 8);
    CHECK(item_size <= SIZE_MAX && item_size > 0);
    size_t item_frame_size = (size_t)item_size + 9;
    CHECK(item_frame_size <= sizeof(complete) - complete_size);
    uint8_t repeated[sizeof(complete)];
    size_t insert = item_payload + (size_t)item_size;
    memcpy(repeated, complete, complete_size);
    memmove(repeated + insert + item_frame_size, repeated + insert,
            complete_size - insert);
    memcpy(repeated + insert, repeated + item_payload - 9,
           item_frame_size);
    size_t repeated_terminal = terminal_payload + item_frame_size;
    put_u64(repeated + repeated_terminal + 8, 2);
    put_u64(repeated + repeated_terminal + 16, 2);
    CHECK(jdv_hash(&producer.item, item_hash) == 0);
    blake3_init(&digest);
    blake3_update(&digest, "JTV1-ITEMS-v1",
                  sizeof("JTV1-ITEMS-v1") - 1u);
    blake3_update(&digest, item_hash, sizeof(item_hash));
    blake3_update(&digest, item_hash, sizeof(item_hash));
    blake3_final(&digest, repeated + repeated_terminal + 56,
                  JDV_HASH_SIZE);
    limits.max_items = 1;
    CHECK(jdv_wire_validate(repeated, complete_size + item_frame_size,
                            &arena, &limits, &unchanged) < 0 && errno == E2BIG);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    limits.max_items = 2;
    CHECK(jdv_wire_validate(repeated, complete_size + item_frame_size,
                            &arena, &limits, &verified) == 0);
    CHECK(verified.items_seen == 2 && arena.used == 0);
    for (unsigned mutation = 0; mutation < 512; mutation++) {
        uint8_t changed[sizeof(complete)];
        memcpy(changed, complete, complete_size);
        size_t byte = ((size_t)mutation * 131u) % complete_size;
        changed[byte] ^= (uint8_t)(1u << (mutation % 8u));
        unchanged.items_seen = 123;
        int result = jdv_wire_validate(changed, complete_size, &arena,
                                        &limits, &unchanged);
        CHECK(arena.used == 0);
        if (result < 0) CHECK(unchanged.items_seen == 123);
        else CHECK(unchanged.state == JDV_STREAM_COMPLETE &&
                   unchanged.items_seen == 1);
    }
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 1);

    producer.index = 0;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config, &vtable, &producer) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    memset(&output, 0, sizeof(output));
    output.fail = 1;
    CHECK(jdv_stream_write_frames(&cursor, sink, &output, workspace,
                                   sizeof(workspace), &terminal) < 0);
    CHECK(errno == EPIPE);
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 2);

    producer.index = 0;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config, &vtable, &producer) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    memset(&output, 0, sizeof(output));
    output.fail_after = complete_size / 2;
    CHECK(jdv_stream_write_frames(&cursor, sink, &output, workspace,
                                   sizeof(workspace), &terminal) < 0);
    CHECK(errno == EPIPE && output.used >= output.fail_after);
    CHECK(output.used < complete_size);
    CHECK(!memcmp(output.bytes, complete, output.used));
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 3);

    producer.index = 0;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config, &vtable, &producer) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    memset(&output, 0, sizeof(output));
    CHECK(jdv_stream_write_frames_v2(&cursor, sink, &output, workspace,
                                      sizeof(workspace), &terminal) == 0);
    CHECK(output.used > 4 && !memcmp(output.bytes, "JTV2", 4));
    CHECK(terminal.state == JDV_STREAM_COMPLETE);
    CHECK(memcmp(terminal.final_hash, producer.end.final_hash,
                 JDV_HASH_SIZE) != 0);
    jdv_wire_info_t imported;
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &arena,
                                &limits, &imported) == 0);
    CHECK(imported.logical_verified && imported.items_seen == 1 &&
          imported.bytes_seen == 1);
    CHECK(jdv_wire_validate(output.bytes, output.used, &arena,
                            &limits, &unchanged) < 0 && errno == EBADMSG);
    size_t v2_position = 4, credit_position = 0;
    while (v2_position < output.used) {
        CHECK(output.used - v2_position >= 9);
        uint8_t tag = output.bytes[v2_position];
        uint64_t length = read_u64(output.bytes + v2_position + 1);
        v2_position += 9;
        CHECK(length <= output.used - v2_position);
        if (tag == 'I') {
            CHECK(length >= 8 + 5 && !credit_position);
            credit_position = v2_position;
            CHECK(read_u64(output.bytes + credit_position) == 1);
            CHECK(!memcmp(output.bytes + credit_position + 8, "JDV1", 4));
        }
        v2_position += (size_t)length;
    }
    CHECK(credit_position && v2_position == output.used);
    output.bytes[credit_position + 7] = 2;
    unchanged.items_seen = 123;
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &arena,
                                &limits, &unchanged) < 0 && errno == EBADMSG);
    CHECK(unchanged.items_seen == 123 && arena.used == 0);
    output.bytes[credit_position + 7] = 1;
    output.bytes[credit_position + 8 + 12] ^= 1;
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &arena,
                                &limits, &unchanged) < 0 && errno == EBADMSG);
    output.bytes[credit_position + 8 + 12] ^= 1;

    uint8_t schema_storage[4096], item_storage[4096];
    jdv_wire_import_t imported_stream = {0};
    CHECK(jdv_wire_import_v2_open(&imported_stream, output.bytes, output.used,
            schema_storage, sizeof(schema_storage), item_storage,
            sizeof(item_storage), &limits) == 0);
    jdv_stream_record_t imported_record;
    CHECK(jdv_stream_pull(&imported_stream.cursor, &imported_record) == JDV_PULL_ITEM);
    CHECK(imported_record.item && imported_record.item->kind == JDV_U64 &&
          imported_record.item->as.u64 == 17);
    CHECK(jdv_hash(imported_record.item, item_hash) == 0);
    uint8_t original_item_hash[JDV_HASH_SIZE];
    CHECK(jdv_hash(&producer.item, original_item_hash) == 0);
    CHECK(!memcmp(item_hash, original_item_hash, JDV_HASH_SIZE));
    CHECK(imported_record.logical_bytes == 1 &&
          imported_record.physical_bytes ==
              read_u64(output.bytes + credit_position - 8) + 9 &&
          imported_record.materialized_bytes == 0);
    CHECK(jdv_stream_pull(&imported_stream.cursor, &imported_record) == JDV_PULL_END);
    CHECK(imported_record.terminal && imported_record.terminal->state ==
          JDV_STREAM_COMPLETE);
    CHECK(imported_record.terminal->bytes_seen == 1 &&
          !memcmp(imported_record.terminal->final_hash,
                  terminal.final_hash, JDV_HASH_SIZE));
    CHECK(jdv_wire_import_close(&imported_stream) == 0);
    CHECK(jdv_wire_import_close(&imported_stream) == 0);
    CHECK(jdv_wire_import_v2_open(&imported_stream, complete, complete_size,
            schema_storage, sizeof(schema_storage), item_storage,
            sizeof(item_storage), &limits) < 0);
    CHECK(imported_stream.active == 0);
    CHECK(jdv_wire_import_v2_open(&imported_stream, output.bytes, output.used,
            &imported_stream, sizeof(imported_stream), item_storage,
            sizeof(item_storage), &limits) < 0 && errno == EINVAL);
    CHECK(imported_stream.active == 0);

    CHECK(jdv_wire_import_v2_open(&imported_stream, output.bytes, output.used,
            schema_storage, sizeof(schema_storage), item_storage,
            sizeof(item_storage), &limits) == 0);
    CHECK(jdv_stream_pull(&imported_stream.cursor, &imported_record) == JDV_PULL_ITEM);
    CHECK(jdv_stream_cancel(&imported_stream.cursor, JDV_LITERAL("early"), NULL) == 0);
    CHECK(jdv_wire_import_close(&imported_stream) == 0);
    CHECK(jdv_stream_release(&cursor) == 0 && producer.releases == 4);

    producer_t cancelled_source = {0};
    CHECK(jdv_schema_scalar(JDV_U64, 0, &cancelled_source.schema) == 0);
    CHECK(jdv_stream_end_init(&cancelled_source.end) == 0);
    cancelled_source.end.state = JDV_STREAM_CANCELLED;
    cancelled_source.end.completeness = JDV_COMPLETENESS_PARTIAL;
    cancelled_source.end.reason = JDV_LITERAL("upstream stopped");
    static const jdv_stream_producer_vtable_t cancelled_vtable = {
        JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
        cancelled_pull, cancel, release
    };
    jdv_stream_config_init(&config);
    config.declared_schema = config.guaranteed_schema = &cancelled_source.schema;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config,
                                      &cancelled_vtable,
                                      &cancelled_source) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    memset(&output, 0, sizeof(output));
    CHECK(jdv_stream_write_frames_v2(&cursor, sink, &output, workspace,
                                      sizeof(workspace), &terminal) == 0);
    CHECK(terminal.state == JDV_STREAM_CANCELLED);
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &arena,
                                &limits, &imported) == 0);
    CHECK(imported.state == JDV_STREAM_CANCELLED && imported.items_seen == 0);
    CHECK(jdv_wire_import_v2_open(&imported_stream, output.bytes, output.used,
            schema_storage, sizeof(schema_storage), item_storage,
            sizeof(item_storage), &limits) == 0);
    CHECK(jdv_stream_pull(&imported_stream.cursor, &imported_record) ==
          JDV_PULL_ERROR);
    CHECK(imported_record.terminal && imported_record.terminal->state ==
          JDV_STREAM_CANCELLED);
    CHECK(jdv_wire_import_close(&imported_stream) == 0);
    CHECK(jdv_stream_release(&cursor) == 0 && cancelled_source.releases == 1);
    puts("data wire: ok");
    return 0;
}
