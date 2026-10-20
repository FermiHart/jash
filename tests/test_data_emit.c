// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "core/data_emit.h"
#include "core/data_wire.h"
#include "core/data_wire_import.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "data emit check failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

typedef struct {
    unsigned char bytes[32768];
    size_t used;
    int interrupt;
    size_t fail_after;
} output_t;

static long deliver(void* context, const void* bytes, size_t size) {
    output_t* out = context;
    if (!out->interrupt++) { errno = EINTR; return -1; }
    if (out->fail_after && out->used >= out->fail_after) { errno = EPIPE; return -1; }
    if (size > 7) size = 7; /* Every emission also exercises short writes. */
    if (size > sizeof(out->bytes) - out->used) { errno = ENOSPC; return -1; }
    memcpy(out->bytes + out->used, bytes, size);
    out->used += size;
    return (long)size;
}

static unsigned char workspace[16384];
static unsigned char arena_bytes[65536];
static unsigned char schema_bytes[16384];
static unsigned char item_bytes[65536];
static jdv_arena_t arena;
static jdv_schema_t any;
static jdv_stream_config_t config;
static jdv_stream_end_t outcome;
static jdv_value_t values[2];
static const uint64_t credits[] = {7, 11};

static int write_values(output_t* output, jdv_stream_end_t* final) {
    return jdv_emit_sequence(values, 2, credits, &config, &outcome,
                              deliver, output, workspace, sizeof(workspace), final);
}
static int check_wire(const output_t* output, const jdv_stream_end_t* expected) {
    jdv_wire_limits_t bounds; jdv_wire_limits_init(&bounds);
    bounds.max_wire_bytes = sizeof(output->bytes);
    bounds.max_frame_bytes = sizeof(workspace);
    jdv_arena_t scratch; jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate_v2(output->bytes, output->used, &scratch, &bounds, &info) == 0);
    CHECK(info.state == expected->state && info.completeness == expected->completeness);
    CHECK(info.items_seen == 2 && info.bytes_seen == 18 && info.logical_verified);
    CHECK(info.errors_seen == expected->errors_seen && info.truncated == expected->truncated);
    CHECK(info.final_hash_present && !memcmp(info.final_hash, expected->final_hash, JDV_HASH_SIZE));
    jdv_wire_import_t imported = {0};
    CHECK(jdv_wire_import_v2_open(&imported, output->bytes, output->used,
            schema_bytes, sizeof(schema_bytes), item_bytes, sizeof(item_bytes), &bounds) == 0);
    for (size_t i = 0; i < 2; i++) {
        jdv_stream_record_t record;
        CHECK(jdv_stream_pull(&imported.cursor, &record) == JDV_PULL_ITEM);
        CHECK(record.logical_bytes == credits[i]);
        uint8_t actual[JDV_HASH_SIZE], original[JDV_HASH_SIZE];
        CHECK(jdv_hash(record.item, actual) == 0 && jdv_hash(&values[i], original) == 0);
        CHECK(!memcmp(actual, original, sizeof(actual)));
    }
    jdv_stream_record_t terminal;
    CHECK(jdv_stream_pull(&imported.cursor, &terminal) ==
            (expected->state == JDV_STREAM_COMPLETE ? JDV_PULL_END : JDV_PULL_ERROR));
    CHECK(terminal.terminal && terminal.terminal->state == expected->state);
    CHECK(terminal.terminal->producer_status == expected->producer_status);
    CHECK(terminal.terminal->transform_status == expected->transform_status);
    CHECK(terminal.terminal->transport_status == expected->transport_status);
    CHECK(terminal.terminal->reason.len == expected->reason.len);
    CHECK(!expected->reason.len || !memcmp(terminal.terminal->reason.data, expected->reason.data, expected->reason.len));
    CHECK(jdv_wire_import_close(&imported) == 0);
    return 0;
}

static int successful_and_partial(void) {
    output_t output = {0}; jdv_stream_end_t final;
    CHECK(write_values(&output, &final) == 0);
    CHECK(output.used > 4 && !memcmp(output.bytes, "JTV2", 4));
    CHECK(final.state == JDV_STREAM_COMPLETE && final.items_seen == 2 && final.bytes_seen == 18);
    CHECK(final.final_schema == &any);
    CHECK(check_wire(&output, &final) == 0);

    const jdv_stream_state_t states[] = {JDV_STREAM_FAILED, JDV_STREAM_INCOMPLETE, JDV_STREAM_CANCELLED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        CHECK(jdv_stream_end_init(&outcome) == 0);
        outcome.state = states[i]; outcome.completeness = JDV_COMPLETENESS_PARTIAL;
        outcome.errors_seen = 3;
        outcome.producer_status = 7; outcome.transform_status = 9; outcome.transport_status = 11;
        outcome.truncated = states[i] == JDV_STREAM_INCOMPLETE;
        outcome.reason = JDV_LITERAL("provider stopped after an observed prefix");
        output = (output_t){0};
        CHECK(write_values(&output, &final) == 0);
        CHECK(final.state == states[i] && final.final_schema == NULL && final.final_hash_present);
        CHECK(check_wire(&output, &final) == 0);
    }
    CHECK(jdv_stream_end_init(&outcome) == 0);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    return 0;
}

static int no_output_failure(size_t workspace_size) {
    output_t output = {0};
    jdv_stream_end_t sentinel; memset(&sentinel, 0xa5, sizeof(sentinel));
    jdv_stream_end_t final = sentinel;
    CHECK(jdv_emit_sequence(values, 2, credits, &config, &outcome,
            deliver, &output, workspace, workspace_size, &final) == -1);
    CHECK(output.used == 0 && !memcmp(&final, &sentinel, sizeof(final)));
    return 0;
}

static int admission_failures(void) {
    jdv_value_t saved_value = values[1];
    values[1] = jdv_value_bool(1); values[1].as.boolean = 2;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    values[1] = saved_value;
    jdv_schema_t wrong; CHECK(jdv_schema_scalar(JDV_BOOL, 0, &wrong) == 0);
    config.declared_schema = config.guaranteed_schema = &wrong;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    config.declared_schema = config.guaranteed_schema = &any;
    config.stream_limits.max_items = 1;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    config.stream_limits.max_items = 2;
    config.stream_limits.max_logical_bytes = 17;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    config.stream_limits.max_logical_bytes = 18;
    CHECK(no_output_failure(96) == 0); /* A late item's payload does not fit. */
    outcome.completeness = JDV_COMPLETENESS_PARTIAL;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    outcome.reason = JDV_LITERAL("embedded\0nul");
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    outcome.reason = (jdv_slice_t){0};
    outcome.version++;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    outcome.version--;
    outcome.final_schema = &wrong;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    outcome.final_schema = NULL;
    config.value_limits.max_nodes = 1;
    CHECK(no_output_failure(sizeof(workspace)) == 0);
    config.value_limits = JDV_DEFAULT_LIMITS;
    uint64_t overflow[] = {UINT64_MAX, 1}; output_t output = {0}; jdv_stream_end_t final;
    config.stream_limits.max_logical_bytes = UINT64_MAX;
    CHECK(jdv_emit_sequence(values, 2, overflow, &config, &outcome,
            deliver, &output, workspace, sizeof(workspace), &final) == -1);
    CHECK(output.used == 0);
    config.stream_limits.max_logical_bytes = 18;
    return 0;
}

static int delivery_failure(void) {
    output_t output = {.fail_after = 20};
    jdv_stream_end_t sentinel; memset(&sentinel, 0xa5, sizeof(sentinel));
    jdv_stream_end_t final = sentinel;
    CHECK(write_values(&output, &final) == -1 && errno == EPIPE);
    CHECK(output.used >= 20 && output.used < 100);
    CHECK(!memcmp(&final, &sentinel, sizeof(final)));
    jdv_wire_limits_t bounds; jdv_wire_limits_init(&bounds);
    jdv_arena_t scratch; jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &scratch, &bounds, &info) == -1);
    return 0;
}

static int empty_and_zero_credit(void) {
    output_t output = {0}; jdv_stream_end_t final;
    CHECK(jdv_emit_sequence(NULL, 0, NULL, &config, &outcome,
            deliver, &output, workspace, sizeof(workspace), &final) == 0);
    CHECK(final.items_seen == 0 && final.bytes_seen == 0 && final.final_hash_present);
    output = (output_t){0};
    CHECK(jdv_emit_sequence(values, 2, NULL, &config, &outcome,
            deliver, &output, workspace, sizeof(workspace), &final) == 0);
    CHECK(final.items_seen == 2 && final.bytes_seen == 0);
    return 0;
}

static int explicit_legacy_version(void) {
    output_t output = {0}; jdv_stream_end_t final;
    CHECK(jdv_emit_sequence_version(1, values, 2, credits, &config, &outcome,
        deliver, &output, workspace, sizeof(workspace), &final) == 0);
    CHECK(output.used > 4 && !memcmp(output.bytes, "JTV1", 4));
    jdv_wire_limits_t bounds; jdv_wire_limits_init(&bounds);
    jdv_arena_t scratch; jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate(output.bytes, output.used, &scratch, &bounds, &info) == 0);
    CHECK(info.state == JDV_STREAM_COMPLETE && info.items_seen == 2 && info.bytes_seen == 18);
    CHECK(!info.logical_verified && info.final_hash_present);
    CHECK(!memcmp(info.final_hash, final.final_hash, JDV_HASH_SIZE));
    jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    CHECK(jdv_wire_validate_v2(output.bytes, output.used, &scratch, &bounds, &info) == -1);
    output = (output_t){0};
    jdv_stream_end_t sentinel; memset(&sentinel, 0xa5, sizeof(sentinel)); final = sentinel;
    CHECK(jdv_emit_sequence_version(3, values, 2, credits, &config, &outcome,
        deliver, &output, workspace, sizeof(workspace), &final) == -1);
    CHECK(output.used == 0 && !memcmp(&final, &sentinel, sizeof(final)));
    jdv_value_t saved = values[1]; values[1] = jdv_value_bool(1); values[1].as.boolean = 2;
    CHECK(jdv_emit_sequence_version(1, values, 2, credits, &config, &outcome,
        deliver, &output, workspace, sizeof(workspace), &final) == -1);
    CHECK(output.used == 0 && !memcmp(&final, &sentinel, sizeof(final)));
    values[1] = saved;
    return 0;
}

static int incremental_legacy_version(void) {
    output_t output = {0}; jdv_wire_push_t* push = NULL;
    CHECK(jdv_wire_push_open_version(1, &push, &config, deliver, &output,
                                     workspace, sizeof(workspace)) == 0);
    CHECK(output.used > 4 && !memcmp(output.bytes, "JTV1", 4));
    CHECK(jdv_wire_push_item(push, &values[0], 7) == 0);
    CHECK(jdv_wire_push_item(push, &values[1], 11) == 0);
    jdv_stream_end_t final;
    CHECK(jdv_wire_push_end(push, &outcome, &final) == 0);
    jdv_wire_push_close(push);
    jdv_wire_limits_t bounds; jdv_wire_limits_init(&bounds);
    jdv_arena_t scratch; jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate(output.bytes, output.used, &scratch, &bounds, &info) == 0);
    CHECK(info.items_seen == 2 && info.bytes_seen == 18 && !info.logical_verified);
    CHECK(info.final_hash_present && !memcmp(final.final_hash, info.final_hash, JDV_HASH_SIZE));
    jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    CHECK(jdv_wire_validate(output.bytes, output.used - 1u, &scratch, &bounds, &info) == -1);
    output_t collected = {0};
    CHECK(jdv_emit_sequence_version(1, values, 2, credits, &config, &outcome,
        deliver, &collected, workspace, sizeof(workspace), &final) == 0);
    CHECK(collected.used == output.used && !memcmp(collected.bytes, output.bytes, output.used));
    output = (output_t){0}; push = NULL;
    CHECK(jdv_wire_push_open_version(1, &push, &config, deliver, &output,
                                     workspace, sizeof(workspace)) == 0);
    CHECK(jdv_wire_push_item(push, &values[0], 7) == 0);
    output.fail_after = output.used;
    CHECK(jdv_wire_push_item(push, &values[1], 11) == -1 && errno == EPIPE);
    CHECK(jdv_wire_push_end(push, &outcome, &final) == -1 && errno == EPIPE);
    jdv_wire_push_close(push);
    jdv_arena_init(&scratch, arena_bytes, sizeof(arena_bytes));
    CHECK(jdv_wire_validate(output.bytes, output.used, &scratch, &bounds, &info) == -1);
    output = (output_t){0};
    CHECK(jdv_wire_push_open_version(3, &push, &config, deliver, &output,
                                     workspace, sizeof(workspace)) == -1);
    CHECK(output.used == 0);
    return 0;
}

typedef struct { output_t output; jdv_wire_push_t* push; int armed; } reentrant_t;
static long reentrant_sink(void* context, const void* bytes, size_t size) {
    reentrant_t* sink = context;
    if (sink->armed) {
        sink->armed = 0;
        (void)jdv_wire_push_v2_item(sink->push, &values[0], 0);
    }
    return deliver(&sink->output, bytes, size);
}
static int incremental_final_is_atomic(void) {
    reentrant_t sink = {0};
    CHECK(jdv_wire_push_v2_open(&sink.push, &config, reentrant_sink, &sink,
                               workspace, sizeof(workspace)) == 0);
    CHECK(jdv_wire_push_v2_item(sink.push, &values[0], 7) == 0);
    jdv_stream_end_t sentinel; memset(&sentinel, 0xa5, sizeof(sentinel));
    jdv_stream_end_t final = sentinel;
    sink.armed = 1;
    CHECK(jdv_wire_push_v2_end(sink.push, &outcome, &final) == -1 && errno == EALREADY);
    CHECK(!memcmp(&final, &sentinel, sizeof(final)));
    CHECK(jdv_wire_push_v2_item(sink.push, &values[1], 11) == -1 && errno == EALREADY);
    jdv_wire_push_v2_close(sink.push);
    return 0;
}

int main(void) {
    /* Native units, byte values, a non-UTF8 path and exact integers survive. */
    unsigned char value_storage[16384]; jdv_arena_init(&arena, value_storage, sizeof(value_storage));
    jdv_record_input_t fields[5] = {
        {JDV_LITERAL("size"), jdv_value_filesize(UINT64_MAX)},
        {JDV_LITERAL("time"), jdv_value_timestamp(INT64_MIN)},
        {JDV_LITERAL("integer"), jdv_value_u64(UINT64_C(9007199254740993))},
        {JDV_LITERAL("bytes"), {0}}, {JDV_LITERAL("path"), {0}}
    };
    const unsigned char bytes[] = {0, 1, 0xff};
    CHECK(jdv_value_bytes(&arena, bytes, sizeof(bytes), &fields[3].value) == 0);
    CHECK(jdv_value_path(&arena, "path\xff", 5, &fields[4].value) == 0);
    CHECK(jdv_value_record(&arena, fields, 5, &values[0]) == 0);
    CHECK(jdv_value_record(&arena, fields, 5, &values[1]) == 0);
    CHECK(jdv_schema_any(&any) == 0);
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &any;
    config.stream_limits.max_items = 2; config.stream_limits.max_logical_bytes = 18;
    CHECK(jdv_stream_end_init(&outcome) == 0);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    CHECK(successful_and_partial() == 0);
    CHECK(admission_failures() == 0);
    CHECK(delivery_failure() == 0);
    CHECK(empty_and_zero_credit() == 0);
    CHECK(explicit_legacy_version() == 0);
    CHECK(incremental_final_is_atomic() == 0);
    CHECK(incremental_legacy_version() == 0);
    puts("data emit: native identity, JTV2 verified accounting, terminal preservation, preflight rejection and delivery failure passed");
    return 0;
}
