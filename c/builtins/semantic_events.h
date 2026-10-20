// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Bounded semantic receipt streams. JSON is JSONL; JTV2 reuses the checked
 * incremental core encoder. A receipt is flushed before the next effect. */
#ifndef JASH_BUILTINS_SEMANTIC_EVENTS_H
#define JASH_BUILTINS_SEMANTIC_EVENTS_H
#include "semantic_builtin.h"

/* Callers provide every possible row, using worst-case outcome fields, before
 * beginning effects. The terminal reserve covers cancellation/failure detail.
 * This checks aggregate output, rather than accepting a per-row budget anew. */
static inline int semantic_events_preflight(semantic_builtin_t* call,
        const jdv_value_t* rows, size_t count) {
    if (call->values.error || !count || count > 65536u) return 125;
    size_t limit = SEMANTIC_OUTPUT_BYTES - 512u;
    value_output_measure_t measured = {0, limit};
    if (call->mode == 2) {
        jdv_schema_t record = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        return value_output_data(semantic_measure_output, &measured, rows, count, NULL,
            &record, &call->values.limits, limit, 0, "operation_failed");
    }
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, semantic_measure_output, &measured);
    for (size_t i = 0; i < count; i++) {
        if (jdv_json_write(&writer, &rows[i], &call->values.limits)) return 125;
        jash_jsonw_raw(&writer, "\n");
    }
    if (jash_jsonw_finish(&writer)) return 125;
    long budget = jash_budget_limit();
    /* Presentation refusal is status75, distinct from invalid values or a
     * broken sink (125). JTV2 returns above and never consults this budget. */
    return budget > 0 && (uint64_t)budget < measured.bytes + 512u ? 75 : 0;
}

static inline int semantic_events_emit(semantic_builtin_t* call, jdv_value_t row) {
    if (!call->admitted || call->values.error) return 125;
    if (call->mode == 2)
        return !call->push || jdv_wire_push_v2_item(call->push, &row, 0) ? 125 : 0;
    if (jdv_json_write(&call->output.json, &row, &call->values.limits)) return 125;
    jash_jsonw_raw(&call->output.json, "\n");
    return jash_jsonw_finish(&call->output.json) ? 125 : 0;
}

static inline int semantic_events_end(semantic_builtin_t* call, int status) {
    if (!call->admitted) return 125;
    if (call->mode == 2) {
        jdv_stream_end_t outcome, final;
        if (!call->push || jdv_stream_end_init(&outcome)) return 125;
        outcome.state = call->cancellation_status ? JDV_STREAM_CANCELLED :
            status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
        outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
        outcome.producer_status = status;
        outcome.errors_seen = status ? 1 : 0;
        if (call->cancellation_status) outcome.reason = (jdv_slice_t){(const uint8_t*)"cancelled", 9};
        if (jdv_wire_push_v2_end(call->push, &outcome, &final)) return 125;
    }
    return jash_structured_finish(&call->output, status);
}
#endif
