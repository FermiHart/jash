// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native OSIF process observations; the JSONL and JTV2 adapters share values.
#include "builtin.h"
#include "discovery_output.h"
#include "query_data.h"
#include <core/data_json.h>
#include <core/structured_output.h>
#include <osif/osinfo.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define PROCESS_MAX 4096u
#define PROCESS_FIELD(name, value) {JDV_LITERAL(name), (value)}
typedef struct {
    osif_procinfo_t* items;
    size_t count;
    int all, overflow, cancelled;
    const char* pattern;
    int pids[256];
    size_t pid_count;
} process_query_t;

static int process_contains(const char* text, const char* pattern) {
    size_t n = strlen(pattern);
    for (; *text; text++) if (!strncasecmp(text, pattern, n)) return 1;
    return n == 0;
}

static int process_collect(const osif_procinfo_t* process, void* context) {
    process_query_t* query = context;
    if ((query->cancelled = jash_query_cancel_poll(NULL))) return 1;
    if (query->pid_count) {
        size_t i = 0;
        for (; i < query->pid_count && query->pids[i] != process->pid; i++) { }
        if (i == query->pid_count) return 0;
    } else if (!query->all && (process->pid == 2 || process->ppid == 2)) return 0;
    if (query->pattern && !process_contains(process->comm, query->pattern)) return 0;
    if (query->count == PROCESS_MAX) { query->overflow = 1; return 1; }
    query->items[query->count++] = *process;
    return 0;
}

static int process_size(const char* text, size_t* result) {
    if (!text || !*text || *text < '0' || *text > '9') return -1;
    char* end; errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || n > SIZE_MAX) return -1;
    *result = (size_t)n;
    return 0;
}

static int process_pids(process_query_t* query, const char* text) {
    while (*text) {
        if (*text < '0' || *text > '9' || query->pid_count == 256) return -1;
        char* end; errno = 0;
        unsigned long n = strtoul(text, &end, 10);
        if (errno || !n || n > INT_MAX || (*end && *end != ',')) return -1;
        query->pids[query->pid_count++] = (int)n;
        if (!*end) return 0;
        text = end + 1;
        if (!*text) return -1;
    }
    return -1;
}

enum { PROCESS_PID, PROCESS_PPID, PROCESS_CPU, PROCESS_RSS, PROCESS_VSZ,
       PROCESS_COMM, PROCESS_UID, PROCESS_STATE };
static int process_sort_field(const char* name) {
    if (!strcmp(name, "pid")) return PROCESS_PID;
    if (!strcmp(name, "ppid")) return PROCESS_PPID;
    if (!strcmp(name, "cpu") || !strcmp(name, "%cpu")) return PROCESS_CPU;
    if (!strcmp(name, "rss") || !strcmp(name, "mem") || !strcmp(name, "%mem")) return PROCESS_RSS;
    if (!strcmp(name, "vsz")) return PROCESS_VSZ;
    if (!strcmp(name, "comm") || !strcmp(name, "cmd") || !strcmp(name, "command")) return PROCESS_COMM;
    if (!strcmp(name, "uid") || !strcmp(name, "user")) return PROCESS_UID;
    if (!strcmp(name, "state") || !strcmp(name, "stat")) return PROCESS_STATE;
    return -1;
}

static int process_compare(const osif_procinfo_t* a, const osif_procinfo_t* b, int field) {
    uint64_t left = (uint64_t)a->pid, right = (uint64_t)b->pid;
    switch (field) {
        case PROCESS_PPID: left = (uint64_t)a->ppid; right = (uint64_t)b->ppid; break;
        case PROCESS_CPU: left = a->cpu_ticks; right = b->cpu_ticks; break;
        case PROCESS_RSS: left = a->rss_kb; right = b->rss_kb; break;
        case PROCESS_VSZ: left = a->vsz_kb; right = b->vsz_kb; break;
        case PROCESS_COMM: { int c = strcmp(a->comm, b->comm); if (c) return c; break; }
        case PROCESS_UID: left = (uint64_t)a->uid; right = (uint64_t)b->uid; break;
        case PROCESS_STATE: left = (unsigned char)a->state; right = (unsigned char)b->state; break;
        default: break;
    }
    if (left != right) return left < right ? -1 : 1;
    return (a->pid > b->pid) - (a->pid < b->pid);
}

static int process_emit(jash_writer_t writer, void* context, int mode,
                        jash_discovery_rows_t* rows, int complete, int status) {
    if (mode == JASH_DISCOVERY_DATA) {
        static const jdv_schema_t any = { .kind = JDV_SCHEMA_ANY };
        return query_data_emit(writer, context, &any, rows->items, rows->count, 0,
            status, complete ? JDV_STREAM_COMPLETE : JDV_STREAM_INCOMPLETE,
            complete ? NULL : "page_limit");
    }
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, QUERY_DATA_MAX_BYTES);
    jash_json_writer_t json;
    jash_jsonw_init(&json, jash_structured_buffer_sink, &buffer);
    for (size_t i = 0; i < rows->count && !json.error; i++) {
        if (jdv_json_write(&json, &rows->items[i], &JDV_DEFAULT_LIMITS)) { json.error = 1; break; }
        jash_jsonw_raw(&json, "\n");
    }
    int result = jash_jsonw_finish(&json);
    if (result || buffer.error) result = jash_structured_error(writer, context, "ps", "encoding_failed", 125);
    else result = jash_structured_document(writer, context, "ps", buffer.data ? buffer.data : "",
        buffer.used, buffer.required, jash_budget_limit(), status);
    jash_structured_buffer_free(&buffer);
    return result;
}

int jash_b_ps_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode = JASH_DISCOVERY_TEXT;
    int selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(jash_b_ps, state, writer, context, argc, argv);
    if (selected < 0) return jash_discovery_error(writer, context, "ps", mode, "conflicting_formats", 2);
    process_query_t query = {0};
    size_t offset = 0, limit = PROCESS_MAX, top = 0;
    int top_selected = 0;
    int sort = PROCESS_PID, reverse = 0, cache = 0;
    const char* error = NULL;
    for (int i = 0; i < argc && !error; i++) {
        const char* arg = argv[i];
        if (jash_discovery_format_flag(arg)) continue;
        if (!strcmp(arg, "--")) { if (i + 1 != argc) error = "unexpected_operand"; break; }
        if (!strcmp(arg, "aux") || !strcmp(arg, "-ef") || !strcmp(arg, "ef") ||
            !strcmp(arg, "-e") || !strcmp(arg, "-A") || !strcmp(arg, "ax") ||
            !strcmp(arg, "-ax") || !strcmp(arg, "axu") || !strcmp(arg, "x") ||
            !strcmp(arg, "-x") || !strcmp(arg, "a") || !strcmp(arg, "-a")) query.all = 1;
        else if (!strcmp(arg, "--tree") || !strcmp(arg, "-t") || !strcmp(arg, "-f")) { /* ppid is the native relation */ }
        else if (!strcmp(arg, "--cache")) cache = 1; /* live revalidation; no stale text cache */
        else if (!strcmp(arg, "-r") || !strcmp(arg, "--reverse")) reverse ^= 1;
        else if (!strcmp(arg, "--pattern") || !strcmp(arg, "-C")) {
            if (++i == argc) error = "missing_pattern";
            else { query.pattern = argv[i]; if (!strcmp(arg, "-C")) query.all = 1; }
        } else if (!strcmp(arg, "--offset") || !strcmp(arg, "--limit") || !strcmp(arg, "--top")) {
            size_t value;
            if (++i == argc || process_size(argv[i], &value)) error = "invalid_page";
            else if (!strcmp(arg, "--offset")) offset = value;
            else if (!strcmp(arg, "--top")) { top = value; top_selected = 1; }
            else limit = value;
        } else if (!strcmp(arg, "--sort")) {
            if (++i == argc) { error = "missing_sort"; continue; }
            const char* name = argv[i];
            if (*name == '+' || *name == '-') { reverse ^= *name == '-'; name++; }
            if ((sort = process_sort_field(name)) < 0) error = "unknown_sort";
        } else if (!strcmp(arg, "-p") || !strcmp(arg, "-q") || !strcmp(arg, "--pid")) {
            if (++i == argc || process_pids(&query, argv[i])) error = "invalid_pid";
        } else if (!strncmp(arg, "-p", 2) && arg[2]) {
            if (process_pids(&query, arg + 2)) error = "invalid_pid";
        } else if (!strcmp(arg, "-o") || (!strncmp(arg, "-o", 2) && arg[2])) {
            /* JSON historically emitted its stable full row for -o. Preserve
             * that rule explicitly; column formatting only affects text. */
            if (!strcmp(arg, "-o") && ++i == argc) error = "missing_columns";
        } else error = "invalid_options";
    }
    if (error) return jash_discovery_error(writer, context, "ps", mode, error, 2);
    if (!(bear_osinfo_caps() & OSIF_CAP_PROC))
        return jash_discovery_error(writer, context, "ps", mode, "process_provider_unavailable", 1);
    query.items = calloc(PROCESS_MAX, sizeof(*query.items));
    if (!query.items) return jash_discovery_error(writer, context, "ps", mode, "allocation_failed", 125);
    int observed = bear_osinfo_proc_iter(process_collect, &query);
    if (query.cancelled || query.overflow || observed) {
        int status = query.cancelled ? query.cancelled : query.overflow ? 75 : 1;
        error = query.cancelled ? "request_cancelled" : query.overflow ? "process_limit" : "process_observation_failed";
        free(query.items);
        return jash_discovery_error(writer, context, "ps", mode, error, status);
    }
    if (top_selected && sort == PROCESS_PID) { sort = PROCESS_CPU; reverse = 1; }
    if (top_selected && top < limit) limit = top;
    for (size_t i = 1; i < query.count; i++) {
        osif_procinfo_t current = query.items[i]; size_t j = i;
        while (j && (reverse ? process_compare(&query.items[j - 1], &current, sort) < 0
                            : process_compare(&query.items[j - 1], &current, sort) > 0)) {
            query.items[j] = query.items[j - 1]; j--;
        }
        query.items[j] = current;
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) { free(query.items); return 125; }
    size_t start = offset < query.count ? offset : query.count;
    size_t count = query.count - start;
    if (count > limit) count = limit;
    int complete = start + count == query.count;
    long hz = sysconf(_SC_CLK_TCK);
    for (size_t i = start; i < start + count && !rows.error; i++) {
        const osif_procinfo_t* process = &query.items[i];
        char state_text[2] = {process->state ? process->state : '?', 0};
#define TEXT(value) jash_discovery_string(&rows, (value))
#define QUANTITY(flag, value) ((process->valid & (flag)) ? jdv_value_u64((uint64_t)(value)) : jdv_value_nothing())
        jdv_record_input_t fields[] = {
            PROCESS_FIELD("schema", TEXT("jash.process.v1")), PROCESS_FIELD("pid", jdv_value_u64((uint64_t)process->pid)),
            PROCESS_FIELD("ppid", jdv_value_u64((uint64_t)process->ppid)), PROCESS_FIELD("pgrp", jdv_value_i64(process->pgrp)),
            PROCESS_FIELD("comm", TEXT(process->comm)), PROCESS_FIELD("state", TEXT(state_text)),
            PROCESS_FIELD("uid", QUANTITY(OSIF_PROC_VALID_UID, process->uid)),
            PROCESS_FIELD("rss_kb", QUANTITY(OSIF_PROC_VALID_RSS, process->rss_kb)),
            PROCESS_FIELD("vsz_kb", QUANTITY(OSIF_PROC_VALID_VSZ, process->vsz_kb)),
            PROCESS_FIELD("rss_bytes", (process->valid & OSIF_PROC_VALID_RSS) && process->rss_kb <= UINT64_MAX / 1024u
                ? jdv_value_filesize((uint64_t)process->rss_kb * 1024u) : jdv_value_nothing()),
            PROCESS_FIELD("vsz_rounded_bytes", (process->valid & OSIF_PROC_VALID_VSZ) && process->vsz_kb <= UINT64_MAX / 1024u
                ? jdv_value_filesize((uint64_t)process->vsz_kb * 1024u) : jdv_value_nothing()),
            PROCESS_FIELD("threads", QUANTITY(OSIF_PROC_VALID_THREADS, process->num_threads)),
            PROCESS_FIELD("cpu_ticks", QUANTITY(OSIF_PROC_VALID_CPU, process->cpu_ticks)),
            PROCESS_FIELD("start_ticks", QUANTITY(OSIF_PROC_VALID_START, process->start_ticks)),
            PROCESS_FIELD("ticks_per_second", hz > 0 ? jdv_value_u64((uint64_t)hz) : jdv_value_nothing()),
            PROCESS_FIELD("tty", TEXT(process->tty[0] ? process->tty : "?")),
            PROCESS_FIELD("surface", jdv_value_i64(process->surface)), PROCESS_FIELD("seat", TEXT(process->seat[0] ? process->seat : "-")),
            PROCESS_FIELD("complete", jdv_value_bool(complete)), PROCESS_FIELD("cache_revalidated", jdv_value_bool(cache)),
            PROCESS_FIELD("scope", TEXT("readable-osif-processes")), PROCESS_FIELD("memory_unit_bytes", jdv_value_u64(1024)), PROCESS_FIELD("observed", jdv_value_u64(query.count)),
            PROCESS_FIELD("index", jdv_value_u64(i))
        };
        jash_discovery_add(&rows, fields, sizeof fields / sizeof *fields);
#undef TEXT
#undef QUANTITY
    }
    free(query.items);
    int cancelled = jash_query_cancel_poll(NULL);
    int result = cancelled ? jash_discovery_error(writer, context, "ps", mode, "request_cancelled", cancelled) : rows.error ? jash_discovery_error(writer, context, "ps", mode, "collection_limit", 75)
        : process_emit(writer, context, mode, &rows, complete, 0);
    jash_discovery_rows_free(&rows);
    return result;
}
