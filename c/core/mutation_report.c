// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native per-item results. No inference from stderr, filesystem rescans or help.
#include "mutation_report.h"
#include "output_budget.h"
#include "data_wire.h"
#include <parser/builtin_redir.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static jash_mutation_report_t* active_report;
static volatile sig_atomic_t cancellation_signal;
static int (*control_poll)(void*);
static void* control_context;

void jash_mutation_cancel_control(int (*poll)(void*), void* context) {
    control_poll = poll; control_context = context;
}

int jash_mutation_cancelable(const char* operation, int argc, char* const* argv) {
    if (!operation || argc < 1 || !argv ||
        (strcmp(argv[0], "--json") && strcmp(argv[0], "--data") && strcmp(argv[0], "--data-v2"))) return 0;
    static const char* const operations[] = {"cp", "mv", "rm", "touch", "mkdir", "rmdir", "ln", "chmod", "chown", "sed"};
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); i++)
        if (!strcmp(operation, operations[i])) return 1;
    return 0;
}
static void report_cancel(int signal_number) {
    if (!cancellation_signal) cancellation_signal = signal_number;
}

int jash_mutation_cancel_status(void) {
    if (active_report && !cancellation_signal && control_poll && control_poll(control_context))
        report_cancel(SIGINT);
    return active_report && cancellation_signal ? 128 + cancellation_signal : 0;
}

int jash_mutation_report_stop(const jash_mutation_report_t* report) {
    if (!report) return 0;
    if (report == active_report && !report->writer.error) (void)jash_mutation_cancel_status();
    int cancelled = report == active_report ? cancellation_signal : report->cancelled;
    return report->writer.error ? 125 : cancelled ? 128 + cancelled : 0;
}

static int takes_value(const char* arg, const char* const* options) {
    if (options) for (size_t i = 0; options[i]; i++) {
        if (!strcmp(arg, options[i])) return 1;
        /* A short option with a value consumes the rest of its cluster, or
         * the next operand when last (mv -vt DIR, mv -vtDIR). */
        if (arg[0] == '-' && arg[1] != '-' && options[i][0] == '-' &&
            options[i][1] && !options[i][2]) {
            const char* p = strchr(arg + 1, options[i][1]);
            if (p) return !p[1];
        }
    }
    return 0;
}

static jash_mutation_format_t format_option(const char* arg) {
    if (!strcmp(arg, "--json")) return JASH_MUTATION_JSON;
    if (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2")) return JASH_MUTATION_DATA;
    return JASH_MUTATION_TEXT;
}
int jash_mutation_args_mode(int argc, char** argv, const char* const* options,
                            int* out_argc, char*** out_argv, jash_mutation_format_t* format) {
    if (argc < 0 || (!argv && argc) || !out_argc || !out_argv || !format) { errno = EINVAL; return -1; }
    *out_argc = argc; *out_argv = NULL;
    *format = JASH_MUTATION_TEXT;
    int end = 0;
    while (end < argc && argv[end][0] == '-' && argv[end][1]) {
        if (!strcmp(argv[end], "--")) break;
        jash_mutation_format_t selected = format_option(argv[end]);
        if (selected) {
            if (*format && selected != *format) { errno = EINVAL; return -1; }
            *format = selected;
        }
        int value = takes_value(argv[end], options);
        end++;
        if (value && end < argc) end++;
    }
    if (!*format) return 0;
    char** result = calloc((size_t)argc + 1, sizeof(*result));
    if (!result) return -1;
    int used = 0;
    for (int i = 0; i < argc; i++) {
        if (i < end && format_option(argv[i])) continue;
        int value = i < end && takes_value(argv[i], options);
        result[used++] = argv[i];
        if (value && i + 1 < argc) result[used++] = argv[++i];
    }
    *out_argc = used; *out_argv = result;
    return 1;
}

int jash_mutation_args(int argc, char** argv, const char* const* options,
                       int* out_argc, char*** out_argv) {
    jash_mutation_format_t format;
    int rc = jash_mutation_args_mode(argc, argv, options, out_argc, out_argv, &format);
    return rc > 0 ? (int)format : rc;
}

#define MUTATION_DATA_ARENA (2u * 1024u * 1024u)
#define MUTATION_DATA_FRAME (256u * 1024u)
struct jash_mutation_data {
    jdv_wire_push_t* stream;
    jdv_arena_t arena;
    void* storage;
    void* workspace;
    size_t mark;
    jdv_limits_t limits;
    jdv_schema_t symbol, any, envelope;
    const char* operation;
};
static jdv_value_t native_text(jdv_kind_t kind, const char* text) {
    if (!text) return jdv_value_nothing();
    return (jdv_value_t){.kind = kind, .as.slice = {(const uint8_t*)text, strlen(text)}};
}
static jdv_record_input_t native_field(const char* key, jdv_value_t value) {
    return (jdv_record_input_t){{(const uint8_t*)key, strlen(key)}, value};
}
static long native_sink(void* context, const void* bytes, size_t size) {
    jash_mutation_report_t* report = context;
    return report->writer.sink(report->writer.context, bytes, size);
}
static int native_error(jash_mutation_report_t* report) {
    if (!report->writer.error) report->writer.error = errno ? errno : EIO;
    errno = report->writer.error;
    return -1;
}
static void native_close(jash_mutation_report_t* report) {
    if (!report->native) return;
    jdv_wire_push_v2_close(report->native->stream);
    free(report->native->storage); free(report->native->workspace);
    free(report->native); report->native = NULL;
}
static int native_record(jash_mutation_report_t* report, const char* kind,
                          const jdv_record_input_t* fields, size_t count,
                          uint64_t logical_credit) {
    struct jash_mutation_data* data = report->native;
    jdv_value_t payload, envelope;
    int rc = jdv_value_record_limited(&data->arena, fields, count, &payload, &data->limits);
    if (!rc) {
        jdv_record_input_t outer[] = {
            native_field("schema", native_text(JDV_SYMBOL, "jash.mutation.data.v1")),
            native_field("operation", native_text(JDV_SYMBOL, data->operation)),
            native_field("kind", native_text(JDV_SYMBOL, kind)),
            native_field("data", payload)
        };
        rc = jdv_value_record_limited(&data->arena, outer, 4, &envelope, &data->limits);
        if (!rc) rc = jdv_wire_push_v2_item(data->stream, &envelope, logical_credit);
    }
    data->arena.used = data->mark;
    return rc ? native_error(report) : 0;
}
static int native_begin(jash_mutation_report_t* report, const char* operation,
                         const char* granularity) {
    struct jash_mutation_data* data = calloc(1, sizeof(*data));
    if (!data) return native_error(report);
    report->native = data;
    data->storage = malloc(MUTATION_DATA_ARENA); data->workspace = malloc(MUTATION_DATA_FRAME);
    if (!data->storage || !data->workspace) return native_error(report);
    jdv_arena_init(&data->arena, data->storage, MUTATION_DATA_ARENA);
    data->limits = JDV_DEFAULT_LIMITS;
    data->limits.max_depth = 16; data->limits.max_nodes = 4096;
    data->limits.max_aggregate_bytes = 128u * 1024u;
    data->limits.max_record_fields = 128; data->limits.max_schema_fields = 128;
    data->limits.max_list_items = 1024; data->limits.max_encoded_bytes = MUTATION_DATA_FRAME - 8;
    data->operation = operation;
    if (jdv_schema_scalar(JDV_SYMBOL, 0, &data->symbol) || jdv_schema_any(&data->any)) return native_error(report);
    jdv_schema_field_input_t fields[] = {
        {JDV_LITERAL("schema"), &data->symbol, 0},
        {JDV_LITERAL("operation"), &data->symbol, 0},
        {JDV_LITERAL("kind"), &data->symbol, 0},
        {JDV_LITERAL("data"), &data->any, 0}
    };
    if (jdv_schema_record_limited(&data->arena, fields, 4, 0, 0, &data->envelope, &data->limits)) return native_error(report);
    data->mark = data->arena.used;
    jdv_stream_config_t config; jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &data->envelope;
    config.value_limits = data->limits;
    /* This is bounded per record and streams an existing finite operation;
     * it does not collect the action history in memory or cap a recursive
     * operation at an arbitrary number of already-applied actions. */
    config.stream_limits.max_items = UINT64_MAX;
    config.stream_limits.max_logical_bytes = UINT64_MAX;
    config.stream_limits.max_physical_bytes = UINT64_MAX;
    config.stream_limits.max_materialized_bytes = UINT64_MAX;
    if (jdv_wire_push_v2_open(&data->stream, &config, native_sink, report,
                              data->workspace, MUTATION_DATA_FRAME)) return native_error(report);
    jdv_record_input_t header[] = {
        native_field("granularity", native_text(JDV_SYMBOL, granularity)),
        native_field("undo_refs", native_text(JDV_SYMBOL, "validated_local"))
    };
    return native_record(report, "header", header, 2, 0);
}
static int native_item(jash_mutation_report_t* report, const jash_mutation_item_t* item,
                        jash_mutation_status_t status, const char* status_name,
                        const char* effect_name) {
    jdv_value_t undo = jdv_value_nothing();
    if (item->undo.token[0] && status == JASH_MUT_COMPLETED) {
        jdv_record_input_t reference[] = {
            native_field("ref", native_text(JDV_STRING, item->undo.token)),
            native_field("scope", native_text(JDV_SYMBOL, "local-user-store")),
            native_field("entry_id", jdv_value_u64(item->undo.entry_id)),
            native_field("coverage", native_text(JDV_SYMBOL, item->undo.coverage))
        };
        if (jdv_value_record_limited(&report->native->arena, reference, 4, &undo, &report->native->limits)) return native_error(report);
    }
    jdv_record_input_t fields[] = {
        native_field("index", jdv_value_u64(report->items - 1)),
        native_field("source", native_text(JDV_PATH, item->source)),
        native_field("destination", native_text(JDV_PATH, item->destination)),
        native_field("action", native_text(JDV_SYMBOL, item->action)),
        native_field("status", native_text(JDV_SYMBOL, status_name)),
        native_field("effect", native_text(JDV_SYMBOL, effect_name)),
        native_field("code", native_text(JDV_SYMBOL, item->code)),
        native_field("bytes_written", item->bytes_known ? jdv_value_filesize(item->bytes_written) : jdv_value_nothing()),
        native_field("verified", item->verification ? jdv_value_bool(item->verification > 0) : jdv_value_nothing()),
        native_field("undo", undo),
        native_field("recovery_path", native_text(JDV_PATH, item->recovery_path && item->recovery_path[0] ? item->recovery_path : NULL))
    };
    return native_record(report, "action", fields, sizeof(fields) / sizeof(fields[0]),
                           item->bytes_known ? item->bytes_written : 0);
}
static int native_end(jash_mutation_report_t* report, int rc,
                       const char* status, const char* error) {
    jdv_record_input_t counts[] = {
        native_field("completed", jdv_value_u64(report->completed)),
        native_field("skipped", jdv_value_u64(report->skipped)),
        native_field("failed", jdv_value_u64(report->failed)),
        native_field("planned", jdv_value_u64(report->planned)),
        native_field("partial", jdv_value_u64(report->partial))
    };
    jdv_value_t count_value;
    if (jdv_value_record_limited(&report->native->arena, counts, 5, &count_value, &report->native->limits)) return native_error(report);
    jdv_record_input_t summary[] = {
        native_field("status", native_text(JDV_SYMBOL, status)),
        native_field("exit_code", jdv_value_i64(rc)),
        native_field("mode", native_text(JDV_SYMBOL, report->preview ? "preview" : "apply")),
        native_field("error_code", native_text(JDV_SYMBOL, error)),
        native_field("transaction", native_text(JDV_SYMBOL, report->transaction)),
        native_field("counts", count_value)
    };
    if (native_record(report, "summary", summary, 6, 0)) return -1;
    jdv_stream_end_t outcome, final; (void)jdv_stream_end_init(&outcome);
    outcome.state = rc == 130 || rc == 143 ? JDV_STREAM_CANCELLED :
        rc || report->failed || report->partial ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = outcome.state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    outcome.producer_status = rc;
    outcome.errors_seen = report->failed + report->partial;
    if (!outcome.errors_seen && outcome.state == JDV_STREAM_FAILED) outcome.errors_seen = 1;
    if (error) outcome.reason = (jdv_slice_t){(const uint8_t*)error, strlen(error)};
    if (jdv_wire_push_v2_end(report->native->stream, &outcome, &final)) return native_error(report);
    return 0;
}

static int report_close(jash_mutation_report_t* r, int rc) {
    native_close(r);
    if (active_report == r) {
        r->cancelled = cancellation_signal;
        cancellation_signal = r->previous && r->cancelled ? r->cancelled : r->previous_cancelled;
        active_report = r->previous;
    }
    if (r->term_installed) { r->term_installed = 0; if (sigaction(SIGTERM, &r->old_term, NULL)) rc = 125; }
    if (r->int_installed) { r->int_installed = 0; if (sigaction(SIGINT, &r->old_int, NULL)) rc = 125; }
    if (r->pipe_installed) {
        r->pipe_installed = 0;
        if (sigaction(SIGPIPE, &r->old_pipe, NULL) != 0) return 125;
    }
    return rc;
}

int jash_mutation_report_begin(jash_mutation_report_t* r, const char* operation,
                               const char* granularity, jash_writer_t sink, void* context) {
    return jash_mutation_report_begin_mode(r, operation, granularity, JASH_MUTATION_JSON, sink, context);
}
int jash_mutation_report_begin_mode(jash_mutation_report_t* r, const char* operation,
                                    const char* granularity, jash_mutation_format_t format,
                                    jash_writer_t sink, void* context) {
    if (!r || !operation || !granularity || !sink ||
        (format != JASH_MUTATION_JSON && format != JASH_MUTATION_DATA)) { errno = EINVAL; return -1; }
    memset(r, 0, sizeof(*r));
    jash_budget_unwrap(&sink, &context);
    jash_redir_stream_writer(sink, context);
    jash_jsonw_init(&r->writer, sink, context);
    /* These native handlers execute synchronously without spawning children.
     * Turn a broken result pipe into a checked error/cancellation boundary and
     * restore the shell's disposition on every exit, including header failure. */
    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &r->old_pipe) != 0) {
        r->writer.error = errno;
        return -1;
    }
    r->pipe_installed = 1;
    r->previous = active_report; r->previous_cancelled = cancellation_signal;
    cancellation_signal = r->previous ? r->previous_cancelled : 0; active_report = r;
    struct sigaction cancel = {0}; cancel.sa_handler = report_cancel; sigemptyset(&cancel.sa_mask);
    if (sigaction(SIGINT, &cancel, &r->old_int)) return report_close(r, -1);
    r->int_installed = 1;
    if (sigaction(SIGTERM, &cancel, &r->old_term)) return report_close(r, -1);
    r->term_installed = 1;
    if (format == JASH_MUTATION_DATA)
        return native_begin(r, operation, granularity) ? report_close(r, -1) : 0;
    jash_jsonw_raw(&r->writer, "{\"schema\":\"jash.mutation.v1\",\"operation\":");
    jash_jsonw_string(&r->writer, operation);
    jash_jsonw_raw(&r->writer, ",\"granularity\":");
    jash_jsonw_string(&r->writer, granularity);
    jash_jsonw_raw(&r->writer, ",\"undo_refs\":\"validated_local\",\"items\":[");
    /* Detect a broken result sink before any requested mutation. */
    if (jash_jsonw_finish(&r->writer) != 0) return report_close(r, -1);
    return 0;
}

int jash_mutation_report_item(jash_mutation_report_t* r, const jash_mutation_item_t* item) {
    if (!r) return 0;
    if (r->writer.error) return -1;
    if (!item || (unsigned)item->status > JASH_MUT_PARTIAL ||
        (unsigned)item->effect > JASH_MUT_POSSIBLE) {
        r->writer.error = EINVAL;
        return -1;
    }
    jash_mutation_status_t status = item->status;
    if (status == JASH_MUT_FAILED && item->effect != JASH_MUT_NONE) status = JASH_MUT_PARTIAL;
    static const char* statuses[] = {"failed", "completed", "skipped", "planned", "partial"};
    static const char* effects[] = {"none", "changed", "possible"};
    if (r->items == UINT64_MAX) { errno = EOVERFLOW; return native_error(r); }
    if (r->native) {
        r->items++;
        switch (status) {
            case JASH_MUT_COMPLETED: r->completed++; break;
            case JASH_MUT_SKIPPED: r->skipped++; break;
            case JASH_MUT_PLANNED: r->planned++; break;
            case JASH_MUT_PARTIAL: r->partial++; break;
            default: r->failed++; break;
        }
        return native_item(r, item, status, statuses[status], effects[item->effect]);
    }
    if (r->items++) jash_jsonw_raw(&r->writer, ",");
    jash_jsonw_raw(&r->writer, "{\"source\":"); jash_jsonw_string_or_bytes(&r->writer, item->source);
    jash_jsonw_raw(&r->writer, ",\"destination\":"); jash_jsonw_string_or_bytes(&r->writer, item->destination);
    jash_jsonw_raw(&r->writer, ",\"action\":"); jash_jsonw_string(&r->writer, item->action);
    jash_jsonw_raw(&r->writer, ",\"status\":"); jash_jsonw_string(&r->writer, statuses[status]);
    jash_jsonw_raw(&r->writer, ",\"effect\":"); jash_jsonw_string(&r->writer, effects[item->effect]);
    jash_jsonw_raw(&r->writer, ",\"code\":");
    if (item->code) jash_jsonw_string(&r->writer, item->code); else jash_jsonw_raw(&r->writer, "null");
    jash_jsonw_raw(&r->writer, ",\"bytes_written\":");
    if (item->bytes_known) jash_jsonw_safe_u64(&r->writer, item->bytes_written);
    else jash_jsonw_raw(&r->writer, "null");
    jash_jsonw_raw(&r->writer, ",\"verified\":");
    jash_jsonw_raw(&r->writer, item->verification > 0 ? "true" : item->verification < 0 ? "false" : "null");
    jash_jsonw_raw(&r->writer, ",\"undo\":");
    if (item->undo.token[0] && status == JASH_MUT_COMPLETED) {
        jash_jsonw_raw(&r->writer, "{\"ref\":"); jash_jsonw_string(&r->writer, item->undo.token);
        jash_jsonw_raw(&r->writer, ",\"scope\":\"local-user-store\",\"entry_id\":\"");
        jash_jsonw_u64(&r->writer, item->undo.entry_id);
        jash_jsonw_raw(&r->writer, "\",\"coverage\":"); jash_jsonw_string(&r->writer, item->undo.coverage);
        jash_jsonw_raw(&r->writer, "}");
    } else jash_jsonw_raw(&r->writer, "null");
    if (item->recovery_path && item->recovery_path[0]) {
        jash_jsonw_raw(&r->writer, ",\"recovery_path\":");
        jash_jsonw_string_or_bytes(&r->writer, item->recovery_path);
    }
    jash_jsonw_raw(&r->writer, "}");
    switch (status) {
        case JASH_MUT_COMPLETED: r->completed++; break;
        case JASH_MUT_SKIPPED: r->skipped++; break;
        case JASH_MUT_PLANNED: r->planned++; break;
        case JASH_MUT_PARTIAL: r->partial++; break;
        default: r->failed++; break;
    }
    /* Each item is a cancellation boundary: do not perform another item after
     * discovering that the caller cannot receive its results. */
    return jash_jsonw_finish(&r->writer);
}

int jash_mutation_report_heartbeat(jash_mutation_report_t* report) {
    if (!report) return 0;
    if (report->writer.error) return -1;
    if (report->native) {
        jdv_record_input_t progress = native_field("phase", native_text(JDV_SYMBOL, "atomic_pending"));
        return native_record(report, "progress", &progress, 1, 0);
    }
    jash_jsonw_raw(&report->writer, " ");
    return jash_jsonw_finish(&report->writer);
}

int jash_mutation_report_failed(const jash_mutation_report_t* r) {
    return r && r->writer.error;
}

int jash_mutation_report_end(jash_mutation_report_t* r, int rc) {
    if (r->writer.error) return report_close(r, 125);
    if (r == active_report) r->cancelled = cancellation_signal;
    if (r->cancelled) { rc = 128 + r->cancelled; r->error_code = "cancelled"; }
    const char* status = rc == 130 || rc == 143 ? "cancelled" : r->partial ||
        ((rc || r->failed) && (r->completed || r->planned)) ? "partial" :
        rc || r->failed ? "failed" : r->preview ? "planned" : "completed";
    const char* error = r->error_code ? r->error_code : rc && !r->failed && !r->partial ? "command_failed" : NULL;
    if (r->native) return report_close(r, native_end(r, rc, status, error) == 0 ? rc : 125);
    jash_jsonw_raw(&r->writer, "],\"status\":"); jash_jsonw_string(&r->writer, status);
    jash_jsonw_raw(&r->writer, ",\"exit_code\":"); jash_jsonw_i64(&r->writer, rc);
    if (r->transaction) { jash_jsonw_raw(&r->writer, ",\"transaction\":"); jash_jsonw_string(&r->writer, r->transaction); }
    jash_jsonw_raw(&r->writer, ",\"mode\":"); jash_jsonw_string(&r->writer, r->preview ? "preview" : "apply");
    jash_jsonw_raw(&r->writer, ",\"error_code\":");
    if (error) jash_jsonw_string(&r->writer, error); else jash_jsonw_raw(&r->writer, "null");
    jash_jsonw_raw(&r->writer, ",\"counts\":{\"completed\":"); jash_jsonw_safe_u64(&r->writer, r->completed);
    jash_jsonw_raw(&r->writer, ",\"skipped\":"); jash_jsonw_safe_u64(&r->writer, r->skipped);
    jash_jsonw_raw(&r->writer, ",\"failed\":"); jash_jsonw_safe_u64(&r->writer, r->failed);
    jash_jsonw_raw(&r->writer, ",\"planned\":"); jash_jsonw_safe_u64(&r->writer, r->planned);
    jash_jsonw_raw(&r->writer, ",\"partial\":"); jash_jsonw_safe_u64(&r->writer, r->partial);
    jash_jsonw_raw(&r->writer, "}}\n");
    return report_close(r, jash_jsonw_finish(&r->writer) == 0 ? rc : 125);
}
