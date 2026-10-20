// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Bounded semantic provider glue. Providers construct JDV from actual engine
 * state. No human-output capture, JSON reparse, or extra wire codec lives here. */
#ifndef JASH_BUILTINS_SEMANTIC_BUILTIN_H
#define JASH_BUILTINS_SEMANTIC_BUILTIN_H
#include "builtin.h"
#include "semantic_value.h"
#include <core/argv.h>
#include <core/data_wire.h>

#define SEMANTIC_VALUE_BYTES (1024u * 1024u)
#define SEMANTIC_OUTPUT_BYTES (4u * 1024u * 1024u)
typedef struct {
    semantic_value_t values;
    const char* command;
    const char* schema_id;
    int mode;
    jash_writer_t writer;
    void* context;
    jash_structured_output_t output;
    jdv_schema_t schema;
    value_output_sink_t sink;
    jdv_wire_push_t* push;
    void* workspace;
    int admitted;
    int cancellation_status; /* set only after a cooperative poll observes it */
} semantic_builtin_t;

static inline long semantic_measure_output(void* context, const char* data, size_t size) {
    return value_output_measure(context, data, size);
}

/* Validate a candidate receipt and the configured complete-output budget
 * before an effect. Providers may use the longest possible status strings
 * and then replace them with shorter observed outcomes after the operation. */
static inline int semantic_preflight(semantic_builtin_t* call, const jdv_value_t* value) {
    if (call->values.error) return 125;
    value_output_measure_t measured = {0, SEMANTIC_OUTPUT_BYTES};
    if (call->mode == 1) return value_output_json(semantic_measure_output, &measured,
        call->command, value, &call->values.limits, SEMANTIC_OUTPUT_BYTES, 0);
    jdv_schema_t record = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
    return value_output_data(semantic_measure_output, &measured, value, 1, NULL, &record,
        &call->values.limits, SEMANTIC_OUTPUT_BYTES, 0, NULL);
}

static inline int semantic_admit_rows(semantic_builtin_t* call, uint64_t max_items) {
    if (call->admitted) return 0;
    if (!max_items) return 125;
    if (call->values.error) return 125;
    if (call->mode == 2) {
        call->workspace = malloc(SEMANTIC_VALUE_BYTES + 512u);
        if (!call->workspace) return 125;
    }
    if (jash_structured_begin(&call->output, call->writer, call->context)) return 125;
    call->admitted = 1;
    if (call->mode == 2) {
        call->schema = (jdv_schema_t){.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        call->sink = (value_output_sink_t){call->output.json.sink, call->output.json.context};
        jdv_stream_config_t config; jdv_stream_config_init(&config);
        config.boundedness = JDV_STREAM_FINITE;
        config.declared_schema = config.guaranteed_schema = &call->schema;
        config.value_limits = call->values.limits;
        config.stream_limits.max_items = max_items;
        config.stream_limits.max_logical_bytes = UINT64_MAX;
        config.stream_limits.max_materialized_bytes = config.stream_limits.max_physical_bytes = SEMANTIC_OUTPUT_BYTES;
        if (jdv_wire_push_v2_open(&call->push, &config, value_output_sink, &call->sink,
                                call->workspace, SEMANTIC_VALUE_BYTES + 512u)) return 125;
    } else {
        jash_jsonw_raw(&call->output.json, "\n");
        if (jash_jsonw_finish(&call->output.json)) return 125;
    }
    return 0;
}

static inline int semantic_admit(semantic_builtin_t* call) {
    return semantic_admit_rows(call, 1);
}

/* Explicit terminal for complete false predicates, real failures and observed
 * cancellation. producer_status remains the command's shell status. */
static inline int semantic_finish_outcome(semantic_builtin_t* call, jdv_value_t value,
                                           const jdv_stream_end_t* requested) {
    if (!requested) return 125;
    jdv_stream_end_t outcome = *requested;
    int status = outcome.producer_status;
    if (call->values.error) {
        call->values.arena.used = 0; call->values.error = 0;
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
            semantic_field("error", semantic_symbol(&call->values, "semantic_value_limit")),
            semantic_field("command_status", jdv_value_i64(status)),
            semantic_field("effects_may_have_applied", jdv_value_bool(call->admitted))
        };
        value = semantic_record(&call->values, fields, 4); status = 125;
        (void)jdv_stream_end_init(&outcome);
        outcome.state = JDV_STREAM_FAILED; outcome.completeness = JDV_COMPLETENESS_PARTIAL;
        outcome.producer_status = status; outcome.errors_seen = 1;
    }
    if (!call->admitted) {
        if (call->mode == 1) return value_output_json(call->writer, call->context,
            call->command, &value, &call->values.limits, SEMANTIC_OUTPUT_BYTES, status);
        jdv_schema_t record = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        return value_output_data_outcome(call->writer, call->context, &value, 1, NULL, &record,
            &call->values.limits, SEMANTIC_OUTPUT_BYTES, &outcome);
    }
    if (call->mode == 2) {
        if (!call->push || jdv_wire_push_v2_item(call->push, &value, 0)) return 125;
        jdv_stream_end_t final;
        if (jdv_wire_push_v2_end(call->push, &outcome, &final)) return 125;
    } else {
        if (jdv_json_write(&call->output.json, &value, &call->values.limits)) return 125;
        jash_jsonw_raw(&call->output.json, "\n");
    }
    return jash_structured_finish(&call->output, status);
}

static inline int semantic_finish(semantic_builtin_t* call, jdv_value_t value, int status) {
    jdv_stream_end_t outcome;
    if (jdv_stream_end_init(&outcome)) return 125;
    outcome.state = call->cancellation_status ? JDV_STREAM_CANCELLED : status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = status; outcome.errors_seen = status ? 1u : 0u;
    if (call->cancellation_status) outcome.reason = (jdv_slice_t){(const uint8_t*)"cancelled", 9};
    else if (!call->admitted && status) outcome.reason = (jdv_slice_t){(const uint8_t*)"operation_failed", 16};
    return semantic_finish_outcome(call, value, &outcome);
}

static inline int semantic_error(semantic_builtin_t* call, const char* code, int status) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("ok", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(&call->values, code))
    };
    return semantic_finish(call, semantic_record(&call->values, fields, 3), status);
}

/* A command merely exiting 130 is not proof of cancellation. Only a provider
 * with an actual cooperative boundary calls this helper with the poll result. */
static inline int semantic_cancelled(semantic_builtin_t* call, int status) {
    call->cancellation_status = status >= 128 && status <= 255 ? status : 130;
    if (semantic_admit(call)) return 125;
    return semantic_error(call, "cancelled", call->cancellation_status);
}

typedef int (*semantic_provider_fn)(semantic_builtin_t*, jash_state_t*, int, char**);
static inline int semantic_dispatch_argv(jash_state_t* state, jash_writer_t writer, void* context,
                                         const char* command, const char* schema,
                                         int argc, char** argv, semantic_provider_fn provider) {
    if (argc < 1 || (strcmp(argv[0], "--json") && strcmp(argv[0], "--data") && strcmp(argv[0], "--data-v2"))) return -1;
    semantic_builtin_t call = {.command = command, .schema_id = schema,
        .mode = !strcmp(argv[0], "--json") ? 1 : 2, .writer = writer, .context = context};
    if (semantic_value_init(&call.values, 4u * 1024u * 1024u, SEMANTIC_VALUE_BYTES)) return 125;
    size_t bytes = 0; int invalid = argc > 64;
    for (int i = 0; i < argc && !invalid; i++) {
        size_t size = strlen(argv[i]);
        if (size > 16384u - bytes) invalid = 1;
        else bytes += size;
    }
    int result = invalid ? semantic_error(&call, "invalid_arguments", 2) : provider(&call, state, argc - 1, argv + 1);
    jdv_wire_push_v2_close(call.push);
    if (call.output.restore_sigpipe) (void)jash_structured_finish(&call.output, 125);
    free(call.workspace); semantic_value_free(&call.values);
    return result;
}

static inline int semantic_dispatch(jash_state_t* state, jash_writer_t writer, void* context,
                                     const char* command, const char* schema,
                                     const char* arguments, semantic_provider_fn provider) {
    const char* text = arguments ? arguments : "";
    while (*text == ' ' || *text == '\t') text++;
    int selected = (!strncmp(text, "--json", 6) && (!text[6] || text[6] == ' ' || text[6] == '\t')) ||
        (!strncmp(text, "--data", 6) && (!text[6] || text[6] == ' ' || text[6] == '\t')) ||
        (!strncmp(text, "--data-v2", 9) && (!text[9] || text[9] == ' ' || text[9] == '\t'));
    if (!selected) return -1;
    semantic_builtin_t call = {.command = command, .schema_id = schema,
        .mode = !strncmp(text, "--json", 6) ? 1 : 2, .writer = writer, .context = context};
    if (semantic_value_init(&call.values, 4u * 1024u * 1024u, SEMANTIC_VALUE_BYTES)) return 125;
    jash_argv_t parsed = {0};
    int invalid = strlen(text) > 16384u || jash_argv_build(text, &parsed) < 0 ||
                  !parsed.argc || parsed.argc > 64;
    int result = invalid ? semantic_error(&call, "invalid_arguments", 2) :
                 provider(&call, state, parsed.argc - 1, parsed.argv + 1);
    jdv_wire_push_v2_close(call.push);
    if (call.output.restore_sigpipe) (void)jash_structured_finish(&call.output, 125);
    free(call.workspace); semantic_value_free(&call.values); jash_argv_free(&parsed);
    return result;
}

/* Only the old text handler uses its historical rest-of-line convention.
 * Structured modes consume the original argv before this compatibility join. */
static inline char* semantic_legacy_join(int argc, char** argv) {
    size_t length = 1;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n > SIZE_MAX - length - 1) return NULL;
        length += n + (i ? 1u : 0u);
    }
    char* text = malloc(length); if (!text) return NULL;
    size_t used = 0;
    for (int i = 0; i < argc; i++) {
        if (i) text[used++] = ' ';
        size_t n = strlen(argv[i]); memcpy(text + used, argv[i], n); used += n;
    }
    text[used] = 0; return text;
}

#define SEMANTIC_ARGV_WRAPPER(name, command_name, schema_name, provider_name) \
    int jash_b_##name##_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) { \
        int result = semantic_dispatch_argv(state, writer, context, command_name, schema_name, argc, argv, provider_name); \
        if (result >= 0) return result; \
        char* arguments = semantic_legacy_join(argc, argv); \
        if (!arguments) return 125; \
        result = jash_b_##name(state, writer, context, arguments); \
        free(arguments); return result; \
    }
#endif
