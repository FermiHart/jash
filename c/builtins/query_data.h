// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Shared builtin glue for already observed native query records. No stdout
 * parsing, inferred byte tags, or implicit promotion of a POSIX pipe. */
#ifndef JASH_BUILTINS_QUERY_DATA_H
#define JASH_BUILTINS_QUERY_DATA_H

#include <core/data_emit.h>
#include <core/query_cancel.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <theme/io.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QUERY_DATA_MAX_ITEMS 4096u
#define QUERY_DATA_MAX_BYTES (32u * 1024u * 1024u)
#define QUERY_DATA_ITEM_WORKSPACE (128u * 1024u)
#define QUERY_DATA_FIELD(name, schema) \
    { {(const uint8_t*)(name), sizeof(name) - 1u}, (schema), 0 }
#define QUERY_DATA_SCALAR(name, type, null_allowed) \
    static const jdv_schema_t name = { \
        .kind = JDV_SCHEMA_SCALAR, .nullable = (null_allowed), .as.scalar = (type) }

static inline jdv_value_t query_data_slice(jdv_kind_t kind,
                                           const void* data, size_t length) {
    jdv_value_t result = {0};
    result.kind = kind;
    result.as.slice.data = data;
    result.as.slice.len = length;
    return result;
}

static inline jdv_value_t query_data_text(jdv_kind_t kind, const char* text) {
    return query_data_slice(kind, text, strlen(text));
}

/* Field/schema arrays are in canonical byte order and share literal keys.
 * Every value pointee remains caller-owned until synchronous emission ends. */
static inline jdv_value_t query_data_record(jdv_record_field_t* fields,
                                             const jdv_schema_t* schema) {
    for (size_t i = 0; i < schema->as.record.len; i++) {
        fields[i].key = schema->as.record.fields[i].name;
        fields[i].value = jdv_value_nothing();
    }
    jdv_value_t result = {0};
    result.kind = JDV_RECORD;
    result.as.record.fields = fields;
    result.as.record.len = schema->as.record.len;
    return result;
}

/* JDV_TIMESTAMP is signed Unix nanoseconds. Keep exact seconds/nanoseconds in
 * the record as well; callers can expose an explicit overflow flag for dates
 * outside the timestamp range rather than wrapping or losing precision. */
static inline jdv_value_t query_data_timestamp(int64_t seconds, long nanoseconds) {
    const int64_t billion = INT64_C(1000000000);
    const int64_t minimum_second = INT64_MIN / billion - 1;
    const int64_t minimum_fraction = billion + INT64_MIN % billion;
    if (nanoseconds < 0 || nanoseconds >= billion ||
        seconds < minimum_second || seconds > INT64_MAX / billion)
        return jdv_value_nothing();
    if (seconds == minimum_second) {
        if (nanoseconds < minimum_fraction) return jdv_value_nothing();
        return jdv_value_timestamp(INT64_MIN + (nanoseconds - minimum_fraction));
    }
    int64_t base = seconds * billion;
    if (base > INT64_MAX - nanoseconds) return jdv_value_nothing();
    return jdv_value_timestamp(base + nanoseconds);
}

typedef struct { jash_writer_t writer; void* context; } query_data_sink_t;

static inline long query_data_write(void* context, const void* bytes, size_t size) {
    query_data_sink_t* sink = context;
    return sink->writer(sink->context, bytes, size);
}

static inline long query_data_measure(void* context, const void* bytes, size_t size) {
    (void)bytes;
    uint64_t* count = context;
    if (size > LONG_MAX || *count > UINT64_MAX - size) { errno = EOVERFLOW; return -1; }
    *count += size;
    return (long)size;
}

static inline int query_data_emit(jash_writer_t writer, void* context,
                                   const jdv_schema_t* schema,
                                   const jdv_value_t* items, size_t count,
                                   uint64_t errors, int status,
                                   jdv_stream_state_t state, const char* reason) {
    /* A control request may arrive inside the last observation syscall. The
     * runtime scope owns the pinned state; check once more before committing a
     * successful/partial result. An already failed producer keeps its error. */
    int cancelled = (state == JDV_STREAM_COMPLETE || state == JDV_STREAM_INCOMPLETE)
        ? jash_query_cancel_poll(NULL) : 0;
    if (cancelled) { state = JDV_STREAM_CANCELLED; status = cancelled; reason = "request_cancelled"; }
    uint64_t* credits = count && count <= QUERY_DATA_MAX_ITEMS + 2u
        ? calloc(count, sizeof(*credits)) : NULL;
    if (count > QUERY_DATA_MAX_ITEMS + 2u || (count && !credits)) {
        count = 0; errors = 1; status = 1; state = JDV_STREAM_FAILED;
        reason = "query_materialization_limit";
    }
    /* Logical credits explicitly measure canonical JDV1 value bytes, not file
     * sizes, syscall traffic, or total process memory. */
    uint64_t total = 0;
    size_t required_workspace = QUERY_DATA_ITEM_WORKSPACE;
    for (size_t i = 0; i < count; i++) {
        jdv_writer_t measure;
        jdv_writer_init(&measure, query_data_measure, &credits[i]);
        if (jdv_encode(&items[i], &measure) < 0 || credits[i] > QUERY_DATA_MAX_BYTES - 8u ||
            credits[i] > QUERY_DATA_MAX_BYTES - total) {
            count = 0; errors = 1; status = 1; state = JDV_STREAM_FAILED;
            reason = "query_value_limit";
            break;
        }
        total += credits[i];
        if (credits[i] + 8u > required_workspace) required_workspace = (size_t)credits[i] + 8u;
    }
    unsigned char small_workspace[QUERY_DATA_ITEM_WORKSPACE];
    unsigned char* workspace = required_workspace > sizeof(small_workspace) ? malloc(required_workspace) : small_workspace;
    if (!workspace) {
        workspace = small_workspace; required_workspace = sizeof(small_workspace);
        count = 0; errors = 1; status = 1; state = JDV_STREAM_FAILED; reason = "allocation_failed";
    }
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.cardinality = JDV_DATA_SEQUENCE;
    config.boundedness = JDV_STREAM_FINITE;
    config.ordering = JDV_STREAM_ORDERED;
    config.declared_schema = schema;
    config.guaranteed_schema = schema;
    config.stream_limits.max_items = QUERY_DATA_MAX_ITEMS + 2u;
    config.stream_limits.max_logical_bytes = QUERY_DATA_MAX_BYTES;
    config.stream_limits.max_physical_bytes = QUERY_DATA_MAX_BYTES;
    config.stream_limits.max_materialized_bytes = QUERY_DATA_MAX_BYTES;
    jdv_stream_end_t outcome, final;
    (void)jdv_stream_end_init(&outcome);
    outcome.state = state;
    outcome.completeness = state == JDV_STREAM_COMPLETE
        ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    outcome.truncated = state == JDV_STREAM_INCOMPLETE;
    outcome.errors_seen = errors;
    outcome.producer_status = status;
    if (reason) outcome.reason = (jdv_slice_t){(const uint8_t*)reason, strlen(reason)};
    struct sigaction previous, ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &previous)) {
        free(credits); if (workspace != small_workspace) free(workspace); return 125;
    }
    jash_budget_unwrap(&writer, &context);
    jash_redir_stream_writer(writer, context);
    query_data_sink_t sink = {writer, context};
    int result = jdv_emit_sequence(items, count, credits, &config, &outcome,
                                    query_data_write, &sink, workspace,
                                    required_workspace, &final);
    free(credits);
    if (workspace != small_workspace) free(workspace);
    if (sigaction(SIGPIPE, &previous, NULL)) return 125;
    return result < 0 ? 125 : status;
}

static inline int query_data_error(jash_writer_t writer, void* context,
                                    const jdv_schema_t* schema,
                                    const char* reason, int status) {
    return query_data_emit(writer, context, schema, NULL, 0, 1, status,
                             JDV_STREAM_FAILED, reason);
}

#endif
