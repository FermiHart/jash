// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "semantic_builtin.h"
#include <core/query_cancel.h>

typedef struct {
    const char* command; const char* effect; const char* how;
    char target[512]; int recoverable, writes;
} explain_native_step_t;
typedef struct {
    explain_native_step_t steps[32]; int count;
    int mutations, deletes, writes, network, opaque;
    int risk_mode, score; const char* reason;
} explain_native_result_t;

/* Preserve the established JSON key order while encoding the same collected
 * semantic fields as JTV2. This is JSON projection, never prose capture. */
static int explain_native_json(semantic_builtin_t* call, const explain_native_result_t* result, int status) {
    jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, SEMANTIC_OUTPUT_BYTES);
    jash_json_writer_t json; jash_jsonw_init(&json, jash_structured_buffer_sink, &buffer);
    jash_jsonw_raw(&json, "{\"steps\":[");
    for (int i = 0; i < result->count; i++) {
        const explain_native_step_t* step = &result->steps[i];
        if (i) jash_jsonw_raw(&json, ",");
        jash_jsonw_raw(&json, "{\"cmd\":"); jash_jsonw_string_or_bytes(&json, step->command);
        jash_jsonw_raw(&json, ",\"effect\":"); jash_jsonw_string(&json, step->effect);
        jash_jsonw_raw(&json, ",\"recoverable\":"); jash_jsonw_raw(&json, step->recoverable ? "true" : "false");
        jash_jsonw_raw(&json, ",\"how\":"); jash_jsonw_string(&json, step->how);
        jash_jsonw_raw(&json, ",\"writes\":"); jash_jsonw_raw(&json, step->writes ? "true" : "false");
        jash_jsonw_raw(&json, ",\"target\":"); jash_jsonw_string_or_bytes(&json, step->target);
        jash_jsonw_raw(&json, "}");
    }
    jash_jsonw_raw(&json, "],\"blast\":{\"mutations\":"); jash_jsonw_u64(&json, (uint64_t)result->mutations);
    jash_jsonw_raw(&json, ",\"delete\":"); jash_jsonw_u64(&json, (uint64_t)result->deletes);
    jash_jsonw_raw(&json, ",\"write\":"); jash_jsonw_u64(&json, (uint64_t)result->writes);
    jash_jsonw_raw(&json, ",\"network\":"); jash_jsonw_u64(&json, (uint64_t)result->network);
    jash_jsonw_raw(&json, ",\"opaque\":"); jash_jsonw_u64(&json, (uint64_t)result->opaque);
    jash_jsonw_raw(&json, "}");
    if (result->risk_mode) {
        jash_jsonw_raw(&json, ",\"risk\":{\"score\":"); jash_jsonw_u64(&json, (uint64_t)result->score);
        jash_jsonw_raw(&json, ",\"band\":"); jash_jsonw_string(&json, risk_band(result->score));
        jash_jsonw_raw(&json, ",\"reason\":"); jash_jsonw_string(&json, result->reason);
        jash_jsonw_raw(&json, "}");
    }
    jash_jsonw_raw(&json, "}\n");
    if (jash_jsonw_finish(&json)) { jash_structured_buffer_free(&buffer); return 125; }
    long budget = jash_budget_limit();
    int rc = jash_structured_document(call->writer, call->context, call->command, buffer.data,
        buffer.used, buffer.required, budget > 0 ? budget : SEMANTIC_OUTPUT_BYTES, status);
    jash_structured_buffer_free(&buffer); return rc;
}

static int explain_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    int risk_mode = !strcmp(call->command, "sense"), first = 0;
    while (first < argc) {
        if (!strcmp(argv[first], "--risk")) { risk_mode = 1; first++; continue; }
        if (!strcmp(argv[first], "--")) { first++; break; }
        if (argv[first][0] == '-') return semantic_error(call, "invalid_arguments", 2);
        break;
    }
    char raw[4096], segmented[4096]; size_t used = 0;
    for (int i = first; i < argc; i++) {
        size_t size = strlen(argv[i]);
        if (size + (i != first) > sizeof(raw) - 1u - used) return semantic_error(call, "input_byte_limit", 75);
        if (i != first) raw[used++] = ' ';
        memcpy(raw + used, argv[i], size); used += size;
    }
    raw[used] = 0; memcpy(segmented, raw, used + 1u);
    char* segments[32]; int segment_count = 0, single = 0, dual = 0; char* begin = segmented;
    for (char* cursor = segmented;;cursor++) {
        char byte = *cursor;
        if (byte == '\'' && !dual) single = !single;
        else if (byte == '"' && !single) dual = !dual;
        int split = 0, skip = 0;
        if (!single && !dual) {
            if (byte == ';') split = 1;
            else if (byte == '|') { split = 1; skip = cursor[1] == '|'; }
            else if (byte == '&' && cursor[1] == '&') { split = 1; skip = 1; }
        }
        if (split || !byte) {
            *cursor = 0;
            while (*begin == ' ' || *begin == '\t') begin++;
            if (*begin) {
                if (segment_count == 32) return semantic_error(call, "segment_limit", 75);
                segments[segment_count++] = begin;
            }
            if (!byte) break;
            if (skip) cursor++;
            begin = cursor + 1;
        }
    }
    int cancelled = jash_query_cancel_poll(state);
    if (cancelled) return semantic_cancelled(call, cancelled);
    explain_native_result_t result = {.risk_mode = risk_mode};
    int protected_rm = jash_trash_protected();
    for (int i = 0; i < segment_count; i++) {
        char verb[64]; seg_verb(segments[i], verb, sizeof(verb));
        if (!*verb) continue;
        if (strlen(verb) == sizeof(verb) - 1u) return semantic_error(call, "verb_byte_limit", 75);
        explain_native_step_t* step = &result.steps[result.count++];
        step->command = segments[i]; step->effect = "read"; step->how = "";
        const char* tag = ""; int effect = classify(verb, &tag); int append = 0;
        step->writes = find_redirect(segments[i], step->target, sizeof(step->target), &append);
        if (strlen(step->target) == sizeof(step->target) - 1u) return semantic_error(call, "target_byte_limit", 75);
        if (effect == EFFECT_DELETE) {
            result.deletes++; result.mutations++; step->effect = "delete"; step->recoverable = protected_rm;
            step->how = protected_rm ? "trash on -> undo/rewind" : "permanent (trash off)";
        } else if (effect == EFFECT_TRASH) { result.mutations++; step->effect = "trash"; step->recoverable = 1; step->how = "recoverable 48h"; }
        else if (effect == EFFECT_MOVE) { result.mutations++; step->effect = "move"; step->recoverable = 1; step->how = "undoable"; }
        else if (effect == EFFECT_COPY) { result.mutations++; step->effect = "copy"; step->how = "may overwrite dest"; }
        else if (effect == EFFECT_CREATE) { result.mutations++; step->effect = "create"; }
        else if (effect == EFFECT_CHMOD) { result.mutations++; step->effect = "chmod"; step->how = "metadata"; }
        else if (effect == EFFECT_NET) { result.network++; step->effect = "network"; step->how = "may exfiltrate/fetch"; }
        else if (effect == EFFECT_EXTERNAL) { result.opaque++; step->effect = "opaque"; step->how = tag; }
        if (step->writes) { result.writes++; result.mutations++; }
    }
    result.reason = "no steps";
    if (segment_count) result.score = jash_explain_risk(raw, &result.reason);
    int status = risk_mode ? result.score >= 80 ? 124 : result.score >= 50 ? 1 : 0 : 0;
    if (call->mode == 1) return explain_native_json(call, &result, status);
    semantic_value_t* sem = &call->values; jdv_value_t items[32];
    for (int i = 0; i < result.count; i++) {
        const explain_native_step_t* step = &result.steps[i];
        jdv_record_input_t fields[] = {
            semantic_field("cmd", semantic_text(sem, step->command)),
            semantic_field("effect", semantic_symbol(sem, step->effect)),
            semantic_field("recoverable", jdv_value_bool(step->recoverable)),
            semantic_field("how", semantic_text(sem, step->how)),
            semantic_field("writes", jdv_value_bool(step->writes)),
            semantic_field("target", semantic_text(sem, step->target))
        };
        items[i] = semantic_record(sem, fields, 6);
    }
    jdv_record_input_t blast[] = {
        semantic_field("mutations", jdv_value_u64((uint64_t)result.mutations)),
        semantic_field("delete", jdv_value_u64((uint64_t)result.deletes)),
        semantic_field("write", jdv_value_u64((uint64_t)result.writes)),
        semantic_field("network", jdv_value_u64((uint64_t)result.network)),
        semantic_field("opaque", jdv_value_u64((uint64_t)result.opaque))
    };
    jdv_record_input_t risk[] = {
        semantic_field("score", jdv_value_u64((uint64_t)result.score)),
        semantic_field("band", semantic_symbol(sem, risk_band(result.score))),
        semantic_field("reason", semantic_text(sem, result.reason))
    };
    jdv_record_input_t fields[] = {
        semantic_field("steps", semantic_list(sem, items, (size_t)result.count)),
        semantic_field("blast", semantic_record(sem, blast, 5)),
        semantic_field("risk", semantic_record(sem, risk, 3))
    };
    jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = status;
    return semantic_finish_outcome(call, semantic_record(sem, fields, risk_mode ? 3 : 2), &outcome);
}

SEMANTIC_ARGV_WRAPPER(explain, "explain", "jash.explain.v1", explain_native_provider)
SEMANTIC_ARGV_WRAPPER(sense, "sense", "jash.explain.v1", explain_native_provider)
