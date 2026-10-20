// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "core/data_wire_import.h"
#include "module/blake3.h"
#include "osif/buffer.h"
#include "osif/ipc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "sealed import failed at %d: %s (errno=%d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

typedef struct {
    jdv_value_t value;
    jdv_stream_end_t end;
    unsigned next;
} fixture_t;

static jdv_stream_pull_result_t pull(void* context, jdv_stream_record_t* record) {
    fixture_t* fixture = context;
    if (!fixture->next++) {
        record->item = &fixture->value;
        record->logical_bytes = 5;
        return JDV_PULL_ITEM;
    }
    record->terminal = &fixture->end;
    return JDV_PULL_END;
}

static int cancel(void* context, jdv_slice_t reason) {
    (void)context; (void)reason;
    return 0;
}

static void release(void* context) { (void)context; }

typedef struct { uint8_t bytes[4096]; size_t used; } output_t;
static long sink(void* context, const void* bytes, size_t count) {
    output_t* out = context;
    if (count > sizeof(out->bytes) - out->used) { errno = ENOSPC; return -1; }
    memcpy(out->bytes + out->used, bytes, count);
    out->used += count;
    return (long)count;
}

int main(void) {
    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_U64, 0, &schema) == 0);
    fixture_t fixture = {.value = jdv_value_u64(91)};
    CHECK(jdv_stream_end_init(&fixture.end) == 0);
    fixture.end.state = JDV_STREAM_COMPLETE;
    fixture.end.completeness = JDV_COMPLETENESS_COMPLETE;
    fixture.end.final_schema = &schema;
    fixture.end.final_hash_present = 1;
    fixture.end.final_hash_algorithm = JDV_HASH_BLAKE3;
    fixture.end.items_seen = 1;
    fixture.end.bytes_seen = 5;
    blake3_hash("upstream", 8, fixture.end.final_hash);
    static const jdv_stream_producer_vtable_t vtable = {
        JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
        pull, cancel, release
    };
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.declared_schema = config.guaranteed_schema = &schema;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    CHECK(jdv_stream_descriptor_init(&descriptor, &config,
                                     &vtable, &fixture) == 0);
    CHECK(jdv_stream_open(&descriptor, &cursor) == 0);
    output_t output = {0};
    uint8_t workspace[4096];
    jdv_stream_end_t terminal;
    CHECK(jdv_stream_write_frames_v2(&cursor, sink, &output,
                                      workspace, sizeof(workspace),
                                      &terminal) == 0);
    CHECK(!memcmp(output.bytes, "JTV2", 4));
    int sealed = jash_buffer_create(output.bytes, output.used, sizeof(output.bytes));
    CHECK(sealed >= 0);
    const void* mapped = NULL;
    CHECK(jash_buffer_map(sealed, output.used,
                           sizeof(output.bytes), &mapped) == 0);
    output.bytes[0] = 'X'; /* the sealed snapshot cannot change with its input */
    CHECK(!memcmp(mapped, "JTV2", 4));
    int reader = jash_buffer_reader_fd(sealed, output.used,
                                       sizeof(output.bytes));
    CHECK(reader >= 0);
    int lane[2];
    CHECK(jash_ipc_data_pair(lane) == 0);
    CHECK(jash_ipc_data_send(lane[0], "typed-v2", output.used, reader) == 0);
    int received = -1;
    uint64_t received_size = 0;
    CHECK(jash_ipc_data_receive(lane[1], "typed-v2",
                                 &received_size, &received) == 0);
    CHECK(received_size == output.used && received >= 0);
    const void* imported_mapping = NULL;
    CHECK(jash_buffer_map(received, received_size,
                           sizeof(output.bytes), &imported_mapping) == 0);
    CHECK(!memcmp(imported_mapping, "JTV2", 4));
    jdv_wire_limits_t limits;
    jdv_wire_limits_init(&limits);
    uint8_t scratch[4096];
    jdv_arena_t verification;
    jdv_arena_init(&verification, scratch, sizeof(scratch));
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate_v2(imported_mapping, output.used, &verification,
                                &limits, &info) == 0);
    CHECK(info.logical_verified && info.bytes_seen == 5);
    uint8_t schemas[4096], items[4096];
    jdv_wire_import_t source = {0};
    CHECK(jdv_wire_import_v2_open(&source, imported_mapping, output.used,
            schemas, sizeof(schemas), items, sizeof(items), &limits) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&source.cursor, &record) == JDV_PULL_ITEM);
    CHECK(record.item && record.item->kind == JDV_U64 &&
          record.item->as.u64 == 91 && record.logical_bytes == 5);
    CHECK(jdv_stream_pull(&source.cursor, &record) == JDV_PULL_END);
    CHECK(record.terminal && record.terminal->state == JDV_STREAM_COMPLETE);
    CHECK(record.terminal->bytes_seen == 5);
    CHECK(jdv_wire_import_close(&source) == 0);
    CHECK(jdv_stream_release(&cursor) == 0);
    jash_buffer_unmap(imported_mapping, output.used);
    jash_buffer_unmap(mapped, output.used);
    CHECK(close(reader) == 0 && close(received) == 0);
    CHECK(close(lane[0]) == 0 && close(lane[1]) == 0);
    CHECK(close(sealed) == 0);
    puts("sealed JTV2 import: ok");
    return 0;
}
