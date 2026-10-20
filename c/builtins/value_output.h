// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked builtin output for already collected native values. No text capture
 * or inferred type conversion. Borrowed values stay alive through this call. */
#ifndef JASH_BUILTINS_VALUE_OUTPUT_H
#define JASH_BUILTINS_VALUE_OUTPUT_H

#include <core/data_emit.h>
#include <core/data_json.h>
#include <core/output_budget.h>
#include <core/structured_output.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { jash_writer_t writer; void* context; } value_output_sink_t;
static inline long value_output_sink(void* context, const void* data, size_t size) {
    value_output_sink_t* sink = context;
    return sink->writer(sink->context, data, size);
}
typedef struct { size_t bytes, limit; } value_output_measure_t;
static inline long value_output_measure(void* context, const void* data, size_t size) {
    (void)data;
    value_output_measure_t* measured = context;
    if (size > LONG_MAX || size > measured->limit - measured->bytes) { errno = E2BIG; return -1; }
    measured->bytes += size;
    return (long)size;
}

/* A predicate may be fully evaluated with a nonzero shell status. Callers
 * supply the terminal explicitly instead of deriving completeness from rc. */
static inline int value_output_data_outcome(jash_writer_t writer, void* context,
                                     const jdv_value_t* items, size_t count,
                                     const uint64_t* credits, const jdv_schema_t* schema,
                                     const jdv_limits_t* limits, size_t max_bytes,
                                     const jdv_stream_end_t* outcome) {
    if (!outcome) return 125;
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = schema;
    config.value_limits = *limits;
    config.stream_limits.max_items = 65536;
    config.stream_limits.max_logical_bytes = UINT64_MAX;
    config.stream_limits.max_physical_bytes = max_bytes;
    config.stream_limits.max_materialized_bytes = max_bytes;
    jdv_stream_end_t final;
    size_t workspace_size = limits->max_encoded_bytes < max_bytes
        ? (size_t)limits->max_encoded_bytes : max_bytes;
    if (workspace_size > SIZE_MAX - 512u) return 125;
    workspace_size += 512u;
    void* workspace = malloc(workspace_size);
    if (!workspace) return 125;
    value_output_measure_t measured = {0, max_bytes};
    int rc = jdv_emit_sequence(items, count, credits, &config, outcome,
        value_output_measure, &measured, workspace, workspace_size, &final);
    if (rc) { free(workspace); return 125; }
    jash_structured_output_t checked;
    if (jash_structured_begin(&checked, writer, context)) { free(workspace); return 125; }
    value_output_sink_t sink = {checked.json.sink, checked.json.context};
    rc = jdv_emit_sequence(items, count, credits, &config, outcome,
        value_output_sink, &sink, workspace, workspace_size, &final);
    free(workspace);
    return jash_structured_finish(&checked, rc ? 125 : outcome->producer_status);
}

static inline int value_output_data(jash_writer_t writer, void* context,
                                     const jdv_value_t* items, size_t count,
                                     const uint64_t* credits, const jdv_schema_t* schema,
                                     const jdv_limits_t* limits, size_t max_bytes,
                                     int status, const char* reason) {
    jdv_stream_end_t outcome;
    if (jdv_stream_end_init(&outcome)) return 125;
    outcome.state = status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = status; outcome.errors_seen = status ? 1u : 0u;
    if (reason) outcome.reason = (jdv_slice_t){(const uint8_t*)reason, strlen(reason)};
    return value_output_data_outcome(writer, context, items, count, credits, schema,
                                     limits, max_bytes, &outcome);
}

static inline int value_output_json(jash_writer_t writer, void* context,
                                     const char* command, const jdv_value_t* value,
                                     const jdv_limits_t* limits, size_t max_bytes,
                                     int status) {
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, max_bytes);
    jash_json_writer_t out;
    jash_jsonw_init(&out, jash_structured_buffer_sink, &buffer);
    int rc = jdv_json_write(&out, value, limits);
    jash_jsonw_raw(&out, "\n");
    if (rc || jash_jsonw_finish(&out)) {
        jash_structured_buffer_free(&buffer);
        return 125;
    }
    long budget = jash_budget_limit();
    rc = jash_structured_document(writer, context, command, buffer.data,
        buffer.used, buffer.required, budget > 0 ? budget : (long)max_bytes, status);
    jash_structured_buffer_free(&buffer);
    return rc;
}

#endif
