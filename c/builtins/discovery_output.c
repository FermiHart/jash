// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "discovery_output.h"
#include <core/data_emit.h>
#include <core/data_json.h>
#include <core/data_wire.h>
#include <core/argv.h>
#include <core/output_budget.h>
#include <core/structured_output.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define DISCOVERY_ARENA_BYTES (4u * 1024u * 1024u)
#define DISCOVERY_WIRE_BYTES (8u * 1024u * 1024u)

struct jash_discovery_effects {
    jash_structured_output_t output;
    jdv_wire_push_t* stream;
    jdv_schema_t schema;
    void* workspace;
    size_t emitted;
    uint64_t errors;
    int mode;
};

static void effects_release(jash_discovery_rows_t* rows) {
    struct jash_discovery_effects* effects = rows->effects;
    if (!effects) return;
    jdv_wire_push_v2_close(effects->stream);
    if (effects->output.restore_sigpipe) (void)jash_structured_finish(&effects->output, 125);
    free(effects->workspace); free(effects); rows->effects = NULL;
}

static long effects_sink(void* context, const void* bytes, size_t length) {
    struct jash_discovery_effects* effects = context;
    jash_json_writer_t* out = &effects->output.json;
    if (out->error) { errno = out->error; return -1; }
    long n = out->sink(out->context, bytes, length);
    /* The wire writer owns the retry loop; EINTR must not poison its sink. */
    if (n < 0 && errno == EINTR) return n;
    if (n <= 0 && length) { out->error = errno ? errno : EIO; errno = out->error; return -1; }
    return n;
}

int jash_discovery_effects_begin(jash_discovery_rows_t* rows, jash_writer_t writer, void* context,
                                  const char* command, int mode, const char* schema_id) {
    if (mode == JASH_DISCOVERY_TEXT) return 0;
    if (rows->effects) return 125;
    if (rows->error) return jash_discovery_error(writer, context, command, mode, "collection_limit_exceeded", 75);
    struct jash_discovery_effects* effects = calloc(1, sizeof *effects);
    if (!effects) return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
    effects->mode = mode;
    if (mode == JASH_DISCOVERY_DATA) {
        effects->workspace = malloc(DISCOVERY_WIRE_BYTES);
        if (!effects->workspace) { free(effects); return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125); }
    }
    rows->effects = effects;
    if (jash_structured_begin(&effects->output, writer, context)) { effects_release(rows); return 125; }
    if (mode == JASH_DISCOVERY_DATA) {
        effects->schema = (jdv_schema_t){.kind = JDV_SCHEMA_RECORD, .nullable = 0,
            .as.record = {.fields = NULL, .len = 0, .open = 1}};
        jdv_stream_config_t config;
        jdv_stream_config_init(&config);
        config.boundedness = JDV_STREAM_FINITE;
        config.declared_schema = config.guaranteed_schema = &effects->schema;
        config.stream_limits.max_items = JASH_DISCOVERY_ROWS_MAX;
        config.stream_limits.max_logical_bytes = DISCOVERY_ARENA_BYTES;
        config.stream_limits.max_materialized_bytes = DISCOVERY_ARENA_BYTES;
        config.stream_limits.max_physical_bytes = DISCOVERY_WIRE_BYTES;
        if (jdv_wire_push_v2_open(&effects->stream, &config, effects_sink, effects,
                                  effects->workspace, DISCOVERY_WIRE_BYTES)) {
            effects->output.json.error = errno ? errno : EIO;
            effects_release(rows); return 125;
        }
    } else {
        jash_json_writer_t* out = &effects->output.json;
        jash_jsonw_raw(out, "{\"schema\":"); jash_jsonw_string(out, schema_id);
        jash_jsonw_raw(out, ",\"command\":"); jash_jsonw_string(out, command);
        jash_jsonw_raw(out, ",\"items\":[");
        if (jash_jsonw_finish(out)) { effects_release(rows); return 125; }
    }
    return 0;
}

int jash_discovery_effects_flush(jash_discovery_rows_t* rows) {
    struct jash_discovery_effects* effects = rows->effects;
    if (!effects) return 0;
    jash_json_writer_t* out = &effects->output.json;
    if (out->error) return 125;
    while (effects->emitted < rows->count) {
        const jdv_value_t* item = &rows->items[effects->emitted];
        const jdv_value_t* ok = jash_discovery_field(item, "ok");
        const jdv_value_t* error = jash_discovery_field(item, "error");
        if ((ok && ok->kind == JDV_BOOL && !ok->as.boolean) ||
            (error && error->kind != JDV_NOTHING)) effects->errors++;
        int rc;
        if (effects->mode == JASH_DISCOVERY_DATA) rc = jdv_wire_push_v2_item(effects->stream, item, 0);
        else {
            if (effects->emitted) jash_jsonw_raw(out, ",");
            rc = jdv_json_write(out, item, &JDV_DEFAULT_LIMITS);
            if (!rc) rc = jash_jsonw_finish(out);
        }
        if (rc) { if (!out->error) out->error = errno ? errno : EIO; return 125; }
        effects->emitted++;
    }
    return 0;
}

int jash_discovery_effects_end_outcome(jash_discovery_rows_t* rows, int status,
                                      jdv_stream_state_t state, int truncated, const char* source_reason) {
    struct jash_discovery_effects* effects = rows->effects;
    if (!effects) return status;
    jash_json_writer_t* out = &effects->output.json;
    if (jash_discovery_effects_flush(rows)) status = 125;
    else {
        if (rows->error) status = 75;
        if (effects->mode == JASH_DISCOVERY_DATA) {
            jdv_stream_end_t outcome, final;
            (void)jdv_stream_end_init(&outcome);
            outcome.state = rows->error ? JDV_STREAM_FAILED : state;
            outcome.completeness = outcome.state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
            outcome.truncated = truncated || rows->error;
            outcome.producer_status = status;
            outcome.errors_seen = effects->errors;
            const char* reason = rows->error ? "collection_limit_exceeded" : source_reason ? source_reason : status ? "operation_failed" : NULL;
            if (status && !outcome.errors_seen && outcome.state != JDV_STREAM_COMPLETE) outcome.errors_seen = 1;
            if (reason) outcome.reason = (jdv_slice_t){(const uint8_t*)reason, strlen(reason)};
            if (jdv_wire_push_v2_end(effects->stream, &outcome, &final)) {
                out->error = errno ? errno : EIO; status = 125;
            }
        } else {
            jash_jsonw_raw(out, "],\"complete\":"); jash_jsonw_raw(out, rows->error ? "false" : "true");
            jash_jsonw_raw(out, ",\"status\":"); jash_jsonw_i64(out, status);
            if (rows->error) jash_jsonw_raw(out, ",\"error\":{\"code\":\"collection_limit_exceeded\",\"effects_may_have_applied\":true}");
            jash_jsonw_raw(out, "}\n");
        }
    }
    status = jash_structured_finish(&effects->output, status);
    effects_release(rows);
    return status;
}

int jash_discovery_effects_end(jash_discovery_rows_t* rows, int status) {
    return jash_discovery_effects_end_outcome(rows, status, status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE, 0, NULL);
}

int jash_discovery_callback_begin(int mode, int* saved_stdout) {
    *saved_stdout = -1;
    if (mode == JASH_DISCOVERY_TEXT) return 0;
    if (fflush(stdout)) return -1;
    int copy = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (copy < 0) return -1;
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) { close(copy); return -1; }
    *saved_stdout = copy;
    return 0;
}

int jash_discovery_callback_end(int saved_stdout) {
    if (saved_stdout < 0) return 0;
    int failed = fflush(stdout) != 0;
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) failed = 1;
    if (close(saved_stdout)) failed = 1;
    clearerr(stdout);
    return failed ? -1 : 0;
}

int jash_discovery_format_flag(const char* argument) {
    if (!strcmp(argument, "--json")) return JASH_DISCOVERY_JSON;
    if (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2")) return JASH_DISCOVERY_DATA;
    return JASH_DISCOVERY_TEXT;
}

int jash_discovery_format(int argc, char* const* argv, int* mode) {
    int count = 0;
    *mode = JASH_DISCOVERY_TEXT;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        int selected = jash_discovery_format_flag(argv[i]);
        if (selected) { if (!count) *mode = selected; count++; }
    }
    return count > 1 ? -1 : count;
}

int jash_discovery_text(jash_builtin_fn handler, jash_state_t* state,
                        jash_writer_t writer, void* context, int argc, char** argv) {
    /* Only legacy human renderers consume this string. They do not parse
     * shell quoting; introducing quotes here changes even `uname -a`. Native
     * query paths consume argv directly and never pass through this adapter. */
    size_t length = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n > SIZE_MAX - length - 2u) return 125;
        length += n + (i != 0);
    }
    char* args = malloc(length + 1u);
    if (!args) return 125;
    size_t at = 0;
    for (int i = 0; i < argc; i++) {
        if (i) args[at++] = ' ';
        size_t n = strlen(argv[i]);
        memcpy(args + at, argv[i], n); at += n;
    }
    args[at] = '\0';
    int rc = handler(state, writer, context, args);
    free(args);
    return rc;
}

const jdv_value_t* jash_discovery_field(const jdv_value_t* record, const char* name) {
    size_t length = strlen(name);
    if (!record || record->kind != JDV_RECORD) return NULL;
    for (size_t i = 0; i < record->as.record.len; i++) {
        const jdv_record_field_t* field = &record->as.record.fields[i];
        if (field->key.len == length && !memcmp(field->key.data, name, length)) return &field->value;
    }
    return NULL;
}

int jash_discovery_rows_init(jash_discovery_rows_t* rows) {
    memset(rows, 0, sizeof(*rows));
    rows->storage = malloc(DISCOVERY_ARENA_BYTES);
    rows->items = calloc(JASH_DISCOVERY_ROWS_MAX, sizeof(*rows->items));
    if (!rows->storage || !rows->items) {
        free(rows->storage); free(rows->items);
        memset(rows, 0, sizeof(*rows));
        return -1;
    }
    jdv_arena_init(&rows->arena, rows->storage, DISCOVERY_ARENA_BYTES);
    return 0;
}

void jash_discovery_rows_free(jash_discovery_rows_t* rows) {
    effects_release(rows);
    free(rows->items); free(rows->storage);
    memset(rows, 0, sizeof(*rows));
}

jdv_value_t jash_discovery_string(jash_discovery_rows_t* rows, const char* text) {
    jdv_value_t value = jdv_value_nothing();
    if (!text || rows->error) return value;
    int rc = jash_json_utf8_valid(text)
        ? jdv_value_string(&rows->arena, text, strlen(text), &value)
        : jdv_value_bytes(&rows->arena, text, strlen(text), &value);
    if (rc) rows->error = 1;
    return value;
}

jdv_value_t jash_discovery_path(jash_discovery_rows_t* rows, const char* text) {
    jdv_value_t value = jdv_value_nothing();
    if (text && !rows->error && jdv_value_path(&rows->arena, text, strlen(text), &value)) rows->error = 1;
    return value;
}

int jash_discovery_add(jash_discovery_rows_t* rows, const jdv_record_input_t* fields, size_t count) {
    if (rows->error || rows->count == JASH_DISCOVERY_ROWS_MAX ||
        jdv_value_record(&rows->arena, fields, count, &rows->items[rows->count])) {
        rows->error = 1;
        return -1;
    }
    rows->count++;
    return 0;
}

typedef struct { jash_writer_t writer; void* context; } discovery_sink_t;
static long discovery_sink(void* context, const void* bytes, size_t count) {
    discovery_sink_t* sink = context;
    return sink->writer(sink->context, bytes, count);
}

static int emit_data(jash_writer_t writer, void* context, const jdv_value_t* items,
                     size_t count, int status, const char* error, jdv_stream_state_t state) {
    /* A closed schema is deliberately not invented from one row. Payloads
     * retain their native value kinds; this schema guarantees record rows. */
    jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .nullable = 0,
                           .as.record = {.fields = NULL, .len = 0, .open = 1}};
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &schema;
    config.stream_limits.max_items = JASH_DISCOVERY_ROWS_MAX;
    config.stream_limits.max_logical_bytes = DISCOVERY_ARENA_BYTES;
    config.stream_limits.max_materialized_bytes = DISCOVERY_ARENA_BYTES;
    config.stream_limits.max_physical_bytes = DISCOVERY_WIRE_BYTES;
    jdv_stream_end_t outcome, final;
    if (jdv_stream_end_init(&outcome)) return 125;
    outcome.state = state;
    outcome.completeness = state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    outcome.producer_status = status;
    outcome.errors_seen = error ? 1u : 0u;
    if (error) outcome.reason = (jdv_slice_t){(const uint8_t*)error, strlen(error)};
    void* workspace = malloc(DISCOVERY_WIRE_BYTES);
    if (!workspace) return 125;
    jash_structured_output_t checked;
    if (jash_structured_begin(&checked, writer, context)) { free(workspace); return 125; }
    discovery_sink_t sink = {checked.json.sink, checked.json.context};
    int rc = jdv_emit_sequence(items, count, NULL, &config, &outcome,
                               discovery_sink, &sink, workspace, DISCOVERY_WIRE_BYTES, &final);
    free(workspace);
    return jash_structured_finish(&checked, rc ? 125 : status);
}

int jash_discovery_error(jash_writer_t writer, void* context, const char* command,
                         int mode, const char* code, int status) {
    if (mode == JASH_DISCOVERY_TEXT) {
        jash_writef(jash_stderr_writer, NULL, "%s: %s\n", command, code);
        return status;
    }
    if (mode == JASH_DISCOVERY_DATA) return emit_data(writer, context, NULL, 0, status, code, JDV_STREAM_FAILED);
    return jash_structured_error(writer, context, command, code, status);
}

int jash_discovery_emit(jash_writer_t writer, void* context, const char* command,
                        int mode, jash_discovery_rows_t* rows, int status) {
    return jash_discovery_emit_schema(writer, context, command, mode, rows, status, "jash.discovery.v1");
}

int jash_discovery_emit_schema(jash_writer_t writer, void* context, const char* command,
                               int mode, jash_discovery_rows_t* rows, int status, const char* schema_id) {
    if (rows->effects) return jash_discovery_effects_end(rows, status);
    if (rows->error) return jash_discovery_error(writer, context, command, mode, "collection_limit_exceeded", 75);
    if (mode == JASH_DISCOVERY_DATA) return emit_data(writer, context, rows->items, rows->count, status, NULL, JDV_STREAM_COMPLETE);
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, DISCOVERY_WIRE_BYTES);
    jash_json_writer_t json;
    jash_jsonw_init(&json, jash_structured_buffer_sink, &buffer);
    jash_jsonw_raw(&json, "{\"schema\":"); jash_jsonw_string(&json, schema_id);
    jash_jsonw_raw(&json, ",\"command\":");
    jash_jsonw_string(&json, command);
    jash_jsonw_raw(&json, ",\"complete\":true,\"status\":"); jash_jsonw_i64(&json, status);
    jash_jsonw_raw(&json, ",\"items\":[");
    for (size_t i = 0; i < rows->count && !json.error; i++) {
        if (i) jash_jsonw_raw(&json, ",");
        if (jdv_json_write(&json, &rows->items[i], &JDV_DEFAULT_LIMITS)) {
            if (!json.error) json.error = errno ? errno : EIO;
            break;
        }
    }
    jash_jsonw_raw(&json, "]}\n");
    int rc = jash_jsonw_finish(&json);
    if (rc || buffer.error) {
        jash_structured_buffer_free(&buffer);
        return jash_discovery_error(writer, context, command, mode, "encoding_failed", 125);
    }
    long budget = jash_budget_limit();
    rc = jash_structured_document(writer, context, command, buffer.data,
                                  buffer.used, buffer.required,
                                  budget > 0 ? budget : DISCOVERY_WIRE_BYTES, status);
    jash_structured_buffer_free(&buffer);
    return rc;
}

int jash_discovery_emit_data_outcome(jash_writer_t writer, void* context, const char* command,
                                     jash_discovery_rows_t* rows, int status,
                                     jdv_stream_state_t state, const char* reason) {
    if (rows->effects) return jash_discovery_effects_end_outcome(rows, status, state, 0, reason);
    if (rows->error) return jash_discovery_error(writer, context, command, JASH_DISCOVERY_DATA, "collection_limit_exceeded", 75);
    return emit_data(writer, context, rows->items, rows->count, status, reason, state);
}
