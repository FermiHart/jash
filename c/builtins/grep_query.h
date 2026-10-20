// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_GREP_QUERY_H
#define JASH_BUILTINS_GREP_QUERY_H

#include "query_data.h"
#include <core/structured_output.h>
#include <fcntl.h>

typedef struct {
    char* path;
    unsigned char* bytes;
    size_t length;
    uint64_t line, start, end;
    const char* error;
} grep_query_row_t;
typedef struct {
    jash_state_t* state;
    grep_query_row_t* rows;
    size_t count, capacity, owned_bytes;
    uint64_t visits, scanned_bytes, offset, limit, json_records, json_bytes;
    int native, capped, cancelled, started, match_limited, semantic_required;
    int max_zero;
    const char* preflight_error;
    uint64_t since_sequence, since_bytes;
    int since_given;
    char since_path[PATH_MAX];
    const char* pattern_sources[64];
    size_t pattern_count, matcher_count;
    void* matchers;
} grep_query_t;
static grep_query_t* g_grep_query;

static int grep_query_stop(void) {
    if (!g_grep_query) return 0;
    if (!g_grep_query->cancelled)
        g_grep_query->cancelled = jash_query_cancel_poll(g_grep_query->state);
    return g_grep_query->cancelled || g_grep_query->capped;
}

static int grep_query_add(const char* path, uint64_t line, uint64_t start, uint64_t end,
                           const char* bytes, size_t length, const char* error) {
    grep_query_t* query = g_grep_query;
    if (!query || !query->native) return -1;
    if (query->capped || query->cancelled) return 0;
    size_t path_size = strlen(path) + 1u;
    if (query->count >= QUERY_DATA_MAX_ITEMS || path_size > QUERY_DATA_MAX_BYTES / 2u - query->owned_bytes ||
        length > QUERY_DATA_MAX_BYTES / 2u - query->owned_bytes - path_size) {
        query->capped = 1; return 0;
    }
    if (query->count == query->capacity) {
        size_t capacity = query->capacity ? query->capacity * 2u : 64u;
        if (capacity > QUERY_DATA_MAX_ITEMS) capacity = QUERY_DATA_MAX_ITEMS;
        grep_query_row_t* rows = realloc(query->rows, capacity * sizeof(*rows));
        if (!rows) { query->capped = 1; return 0; }
        query->rows = rows; query->capacity = capacity;
    }
    char* owned_path = strdup(path);
    unsigned char* owned = length ? malloc(length) : NULL;
    if (!owned_path || (length && !owned)) { free(owned_path); free(owned); query->capped = 1; return 0; }
    if (length) memcpy(owned, bytes, length);
    query->rows[query->count++] = (grep_query_row_t){owned_path, owned, length, line, start, end, error};
    query->owned_bytes += path_size + length;
    return 0;
}

QUERY_DATA_SCALAR(GREP_Q_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(GREP_Q_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(GREP_Q_BYTES, JDV_BYTES, 1);
QUERY_DATA_SCALAR(GREP_Q_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(GREP_Q_ERROR, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(GREP_Q_SYMBOL, JDV_SYMBOL, 0);
static const jdv_schema_field_t GREP_QUERY_FIELDS[] = {
    QUERY_DATA_FIELD("col", &GREP_Q_U64),
    QUERY_DATA_FIELD("complete", &GREP_Q_BOOL),
    QUERY_DATA_FIELD("error", &GREP_Q_ERROR),
    QUERY_DATA_FIELD("file", &GREP_Q_PATH),
    QUERY_DATA_FIELD("line", &GREP_Q_U64),
    QUERY_DATA_FIELD("match", &GREP_Q_BYTES),
    QUERY_DATA_FIELD("match_truncated", &GREP_Q_BOOL),
    QUERY_DATA_FIELD("span_end", &GREP_Q_U64),
    QUERY_DATA_FIELD("span_start", &GREP_Q_U64),
    QUERY_DATA_FIELD("unit", &GREP_Q_SYMBOL)
};
static const jdv_schema_t GREP_QUERY_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {GREP_QUERY_FIELDS, 10, 0}
};

static jdv_value_t grep_query_value(jdv_record_field_t fields[10], const grep_query_row_t* row) {
    jdv_value_t value = query_data_record(fields, &GREP_QUERY_SCHEMA);
    fields[1].value = jdv_value_bool(!row->error);
    if (row->error) fields[2].value = query_data_text(JDV_SYMBOL, row->error);
    fields[3].value = query_data_text(JDV_PATH, row->path);
    fields[6].value = jdv_value_bool(0);
    fields[9].value = query_data_text(JDV_SYMBOL, "byte");
    if (row->line) {
        fields[0].value = jdv_value_u64(row->start + 1u);
        fields[4].value = jdv_value_u64(row->line);
        fields[5].value = query_data_slice(JDV_BYTES, row->bytes, row->length);
        fields[7].value = jdv_value_u64(row->end);
        fields[8].value = jdv_value_u64(row->start);
    }
    return value;
}

static int grep_query_emit(grep_query_t* query, jash_writer_t writer, void* context,
                            int producer, uint64_t errors) {
    if (!query->started) {
        if (query->cancelled)
            return query_data_emit(writer, context, &GREP_QUERY_SCHEMA, NULL, 0, 0, query->cancelled,
                JDV_STREAM_CANCELLED, "request_cancelled");
        return query_data_error(writer, context, &GREP_QUERY_SCHEMA,
            query->preflight_error ? query->preflight_error : "invalid_arguments", 2);
    }
    size_t count = query->offset < query->count ? query->count - (size_t)query->offset : 0;
    if ((uint64_t)count > query->limit) count = (size_t)query->limit;
    jdv_record_field_t (*fields)[10] = count ? calloc(count, sizeof(*fields)) : NULL;
    jdv_value_t* items = count ? calloc(count, sizeof(*items)) : NULL;
    if (count && (!fields || !items)) {
        free(fields); free(items);
        return query_data_error(writer, context, &GREP_QUERY_SCHEMA, "allocation_failed", 1);
    }
    for (size_t i = 0; i < count; i++) items[i] = grep_query_value(fields[i], &query->rows[(size_t)query->offset + i]);
    int incomplete = query->capped || query->match_limited ||
        (query->offset < query->count && count < query->count - query->offset);
    int status = query->cancelled ? query->cancelled : query->capped ? 75 : producer;
    int result = query_data_emit(writer, context, &GREP_QUERY_SCHEMA, items, count, errors, status,
        query->cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        query->cancelled ? "request_cancelled" : query->capped ? "query_collection_limit" :
        query->match_limited ? "match_limit" : incomplete ? "page_limit" : NULL);
    free(fields); free(items);
    return result;
}

static int grep_query_number(const char* text, uint64_t* value) {
    if (!text || !*text) return -1;
    *value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || *value > (UINT64_MAX - (unsigned)(*text - '0')) / 10u) return -1;
        *value = *value * 10u + (unsigned)(*text - '0');
    }
    return 0;
}

static int grep_query_value_option(const char* arg) {
    const char* const names[] = {"--regexp", "--include", "--exclude", "--exclude-dir", "--after-context",
        "--before-context", "--context", "--since", "--max-count", "--offset", "--limit"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (!strcmp(arg, names[i])) return 1;
    if (arg[0] == '-' && arg[1] != '-') for (size_t i = 1; arg[i]; i++)
        if (strchr("ABCem", arg[i])) return !arg[i + 1];
    return 0;
}

/* Context's optional count must not consume a following output selector or
 * pattern. -A/-B, patterns, and required numeric options remain mandatory. */
static int grep_query_takes_value(int argc, char** argv, int index) {
    const char* argument = argv[index];
    int optional = !strcmp(argument, "--context");
    if (argument[0] == '-' && argument[1] && argument[1] != '-') {
        for (size_t i = 1; argument[i]; i++) if (strchr("ABCem", argument[i])) {
            optional = argument[i] == 'C' && !argument[i + 1]; break;
        }
    }
    if (!optional) return grep_query_value_option(argument);
    if (index + 1 >= argc) return 0;
    const char* value = argv[index + 1];
    return (*value >= '0' && *value <= '9') ||
        (*value == '-' && value[1] >= '0' && value[1] <= '9');
}

static int grep_query_mode(int argc, char** argv) {
    int mode = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--json") && !mode) mode = 1;
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) mode = 2;
        if (grep_query_takes_value(argc, argv, i)) i++;
    }
    return mode;
}

static long grep_query_discard(void* context, const char* data, size_t length) {
    (void)context; (void)data;
    return length > LONG_MAX ? -1 : (long)length;
}

static long grep_query_checked(void* context, const char* data, size_t length) {
    jash_structured_output_t* output = context;
    if (output->json.error || length > LONG_MAX) return -1;
    long written = output->json.sink(output->json.context, data, length);
    if (written != (long)length) { output->json.error = errno ? errno : EIO; return -1; }
    return written;
}

#endif
