// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <osif/osif_stat.h>
/* Structured projections of the owned awk AST. Values come from evaluation,
 * never from parsing text/JSON output. The numeric model is explicitly IEEE
 * binary64; formatting never feeds a rounded accumulator back into evaluation. */
#ifndef JASH_BUILTINS_AWK_QUERY_H
#define JASH_BUILTINS_AWK_QUERY_H

#include "query_data.h"
#include <core/structured_output.h>
#include <fcntl.h>
#include <float.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>

#define AWK_QUERY_FILE_BYTES (64u * 1024u * 1024u)
#define AWK_QUERY_SOURCE_BYTES (256u * 1024u * 1024u)
#define AWK_QUERY_OWNED_BYTES (16u * 1024u * 1024u)
#define AWK_QUERY_PRINTF_BYTES (1024u * 1024u)
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "awk native numeric representation requires IEEE binary64");

typedef struct {
    int numeric;
    double number;
    char* bytes;
    size_t length;
    unsigned char bits[8];
    char decimal[32];
    jdv_record_field_t floating[3];
} awk_query_cell_t;
typedef struct {
    const char *kind, *phase, *error;
    char *file, *text;
    size_t text_length, cell_count;
    uint64_t line, record_index, output_index;
    int aggregate;
    awk_query_cell_t cells[AWK_MAX_EXPRS];
    jdv_value_t values[AWK_MAX_EXPRS];
    jdv_record_field_t fields[12];
} awk_query_row_t;
typedef struct {
    jash_state_t* state;
    awk_query_row_t* rows;
    size_t count, capacity, owned;
    uint64_t source_bytes, errors, offset, limit, line;
    const char *file, *phase, *error, *limited;
    int cancelled, aggregate;
} awk_query_t;

QUERY_DATA_SCALAR(AWK_Q_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(AWK_Q_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(AWK_Q_OPTIONAL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(AWK_Q_BYTES, JDV_BYTES, 1);
QUERY_DATA_SCALAR(AWK_Q_PATH, JDV_PATH, 1);
QUERY_DATA_SCALAR(AWK_Q_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(AWK_Q_ERROR, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(AWK_Q_STRING, JDV_STRING, 0);
/* ANY already includes Nothing; its canonical schema must not set nullable. */
static const jdv_schema_t AWK_Q_ANY = {.kind = JDV_SCHEMA_ANY};
static const jdv_schema_t AWK_Q_FIELDS = {
    .kind = JDV_SCHEMA_LIST, .nullable = 1, .as.element = &AWK_Q_ANY
};
static const jdv_schema_field_t AWK_QUERY_FIELDS[] = {
    QUERY_DATA_FIELD("complete", &AWK_Q_BOOL),
    QUERY_DATA_FIELD("error", &AWK_Q_ERROR),
    QUERY_DATA_FIELD("fields", &AWK_Q_FIELDS),
    QUERY_DATA_FIELD("file", &AWK_Q_PATH),
    QUERY_DATA_FIELD("kind", &AWK_Q_SYMBOL),
    QUERY_DATA_FIELD("line", &AWK_Q_OPTIONAL_U64),
    QUERY_DATA_FIELD("numeric_model", &AWK_Q_SYMBOL),
    QUERY_DATA_FIELD("output_index", &AWK_Q_U64),
    QUERY_DATA_FIELD("phase", &AWK_Q_SYMBOL),
    QUERY_DATA_FIELD("record_index", &AWK_Q_U64),
    QUERY_DATA_FIELD("result", &AWK_Q_ANY),
    QUERY_DATA_FIELD("text", &AWK_Q_BYTES)
};
static const jdv_schema_t AWK_QUERY_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {AWK_QUERY_FIELDS, 12, 0}
};
static const jdv_schema_field_t AWK_FLOAT_FIELDS[] = {
    QUERY_DATA_FIELD("bits", &AWK_Q_BYTES),
    QUERY_DATA_FIELD("decimal", &AWK_Q_STRING),
    QUERY_DATA_FIELD("representation", &AWK_Q_SYMBOL)
};
static const jdv_schema_t AWK_FLOAT_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {AWK_FLOAT_FIELDS, 3, 0}
};

static int awk_query_number(const char* text, uint64_t* value) {
    if (!text || !*text) return -1;
    *value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || *value > (UINT64_MAX - (unsigned)(*text - '0')) / 10u) return -1;
        *value = *value * 10u + (unsigned)(*text - '0');
    }
    return 0;
}

/* Options end at the program, not at the last pathname. In particular a file
 * literally called --data or a -v value containing a mode is just data. */
static int awk_query_mode(int argc, char** argv) {
    int mode = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        const char* argument = argv[i];
        if (argument[0] != '-' || !argument[1]) break;
        if (!strcmp(argument, "--json") && !mode) mode = 1;
        if (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2")) mode = 2;
        if (!strcmp(argument, "-F") || !strcmp(argument, "-v") ||
            !strcmp(argument, "--offset") || !strcmp(argument, "--limit")) i++;
    }
    return mode;
}

static int awk_query_stop(awk_query_t* query) {
    if (!query->cancelled) query->cancelled = jash_query_cancel_poll(query->state);
    return query->cancelled || query->error || query->limited;
}

static void awk_query_row_free(awk_query_row_t* row) {
    free(row->file); free(row->text);
    for (size_t i = 0; i < row->cell_count; i++) free(row->cells[i].bytes);
}

static char* awk_query_copy(awk_query_t* query, const char* source, size_t length) {
    if (length >= AWK_QUERY_OWNED_BYTES - query->owned) {
        query->limited = "query_collection_limit"; return NULL;
    }
    char* result = malloc(length + 1u);
    if (!result) { query->error = "allocation_failed"; query->errors++; return NULL; }
    memcpy(result, source, length); result[length] = 0;
    query->owned += length + 1u;
    return result;
}

static awk_query_row_t* awk_query_row(awk_query_t* query, const awk_runtime_t* runtime,
                                      const char* kind, const char* error) {
    if (query->count == QUERY_DATA_MAX_ITEMS) { query->limited = "query_collection_limit"; return NULL; }
    if (query->count == query->capacity) {
        size_t capacity = query->capacity ? query->capacity * 2u : 32u;
        if (capacity > QUERY_DATA_MAX_ITEMS) capacity = QUERY_DATA_MAX_ITEMS;
        awk_query_row_t* rows = realloc(query->rows, capacity * sizeof(*rows));
        if (!rows) { query->error = "allocation_failed"; query->errors++; return NULL; }
        query->rows = rows; query->capacity = capacity;
    }
    awk_query_row_t* row = &query->rows[query->count];
    memset(row, 0, sizeof(*row));
    row->kind = kind; row->error = error; row->phase = query->phase;
    row->line = query->line; row->record_index = (uint64_t)runtime->nr;
    row->output_index = query->count;
    if (query->file) {
        row->file = awk_query_copy(query, query->file, strlen(query->file));
        if (!row->file) return NULL;
    }
    query->count++;
    return row;
}

static void awk_query_fail(awk_query_t* query, const awk_runtime_t* runtime, const char* error) {
    if (!query->error) query->error = error;
    query->errors++;
    (void)awk_query_row(query, runtime, "error", error);
}

static int awk_query_cell(awk_query_t* query, awk_query_cell_t* cell, const awk_val_t* value) {
    cell->numeric = value->is_num;
    cell->number = value->num;
    if (cell->numeric) return isfinite(cell->number) ? 0 : -1;
    cell->length = strlen(value->str);
    cell->bytes = awk_query_copy(query, value->str, cell->length);
    return cell->bytes ? 0 : -1;
}

static int awk_query_printf(awk_query_t* query, awk_runtime_t* runtime,
                             const awk_stmt_t* statement, char** bytes, size_t* length,
                             const char** error) {
    char* result = malloc(AWK_QUERY_PRINTF_BYTES + 1u);
    if (!result) { *error = "allocation_failed"; return -1; }
    size_t used = 0;
    int argument = 1;
    const char* format = statement->args[0].str;
    for (const char* p = format; *p; p++) {
        const char* part = p;
        size_t size = 1;
        char formatted[512];
        awk_val_t value;
        if (*p == '%') {
            p++;
            if (*p == '%') part = p;
            else {
                if (argument < statement->nargs) awk_eval_expr(runtime, &statement->args[argument++], &value);
                else { memset(&value, 0, sizeof(value)); value.is_num = 1; }
                if (value.is_num && !isfinite(value.num)) { *error = "numeric_overflow"; break; }
                if (*p == 's' && !value.is_num) { part = value.str; size = strlen(part); }
                else if (*p == 's') { awk_fmt_num(value.num, formatted, sizeof(formatted)); part = formatted; size = strlen(part); }
                else {
                    double numeric = awk_val_to_num(&value);
                    if (!isfinite(numeric)) { *error = "numeric_overflow"; break; }
                    if (*p == 'd' && (numeric < -0x1p63 || numeric >= 0x1p63)) {
                        *error = "integer_format_overflow"; break;
                    }
                    int n = *p == 'd' ? snprintf(formatted, sizeof(formatted), "%lld", (long long)numeric)
                        : snprintf(formatted, sizeof(formatted), "%f", numeric);
                    if (n < 0 || (size_t)n >= sizeof(formatted)) { *error = "format_limit"; break; }
                    part = formatted; size = (size_t)n;
                }
            }
        }
        if (size > AWK_QUERY_PRINTF_BYTES - used) { *error = "format_limit"; break; }
        memcpy(result + used, part, size); used += size;
    }
    if (*error) { free(result); return -1; }
    result[used] = 0;
    char* owned = awk_query_copy(query, result, used);
    free(result);
    if (!owned) return -1;
    *bytes = owned; *length = used;
    return 0;
}

static void awk_query_statement(awk_query_t* query, awk_runtime_t* runtime, const awk_stmt_t* statement) {
    if (awk_query_stop(query)) return;
    if (statement->kind == STMT_ADD || statement->kind == STMT_SUB) {
        awk_var_t* variable = awk_var_lookup(runtime, statement->var);
        if (!variable) { awk_query_fail(query, runtime, "variable_limit"); return; }
        awk_val_t value; awk_eval_expr(runtime, &statement->rhs, &value);
        double left = awk_var_get_num(variable), right = awk_val_to_num(&value);
        double result = statement->kind == STMT_ADD ? left + right : left - right;
        if (!isfinite(left) || !isfinite(right) || !isfinite(result)) {
            awk_query_fail(query, runtime, "numeric_overflow"); return;
        }
        awk_var_set_num(variable, result);
        return;
    }
    int aggregate = query->aggregate && !strcmp(query->phase, "end") && statement->kind == STMT_PRINT;
    awk_query_row_t* row = awk_query_row(query, runtime,
        statement->kind == STMT_PRINTF ? "printf" : aggregate ? "aggregate" : "print", NULL);
    if (!row) return;
    row->aggregate = aggregate && statement->nargs == 1;
    const char* error = NULL;
    if (statement->kind == STMT_PRINTF) {
        if (!awk_query_printf(query, runtime, statement, &row->text, &row->text_length, &error) && !error) return;
    } else {
        size_t count = statement->nargs ? (size_t)statement->nargs : 1u;
        for (size_t i = 0; i < count; i++) {
            awk_val_t value;
            if (statement->nargs) awk_eval_expr(runtime, &statement->args[i], &value);
            else { memset(&value, 0, sizeof(value)); memcpy(value.str, runtime->field0, strlen(runtime->field0) + 1u); }
            row->cell_count++;
            if (awk_query_cell(query, &row->cells[i], &value) < 0) {
                if (!query->error && !query->limited) error = "numeric_overflow";
                break;
            }
        }
        if (!error && !query->error && !query->limited) return;
    }
    /* An unfinished print/printf is never published as a shorter valid value. */
    awk_query_row_free(row); query->count--;
    if (error) awk_query_fail(query, runtime, error);
}

static void awk_query_block(awk_query_t* query, awk_runtime_t* runtime, const awk_block_t* block) {
    for (int i = 0; i < block->n && !awk_query_stop(query); i++)
        awk_query_statement(query, runtime, &block->stmts[i]);
}

static int awk_query_same_file(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_size == right->st_size && BEAR_ST_MTIM(*left).tv_sec == BEAR_ST_MTIM(*right).tv_sec &&
        BEAR_ST_MTIM(*left).tv_nsec == BEAR_ST_MTIM(*right).tv_nsec && BEAR_ST_CTIM(*left).tv_sec == BEAR_ST_CTIM(*right).tv_sec &&
        BEAR_ST_CTIM(*left).tv_nsec == BEAR_ST_CTIM(*right).tv_nsec;
}

/* Materialize one bounded descriptor-stable regular source before evaluating
 * any of its records. Stdin is an explicit byte stream, not a file snapshot. */
static const char* awk_query_input(awk_query_t* query, const char* path, char** bytes, size_t* size) {
    int standard = !strcmp(path, "-");
    FILE* input = stdin;
    struct stat before = {0}, after, named;
    *bytes = NULL; *size = 0;
    if (awk_query_stop(query)) return NULL;
    if (!standard) {
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return "open_failed";
        const char* error = NULL;
        if (fstat(fd, &before)) error = "stat_failed";
        else if (!S_ISREG(before.st_mode)) error = "not_regular";
        else if (before.st_size < 0) error = "invalid_file_size";
        else if ((uint64_t)before.st_size > AWK_QUERY_FILE_BYTES) error = "source_limit";
        if (error) { (void)close(fd); return error; }
        input = fdopen(fd, "rb");
        if (!input) { (void)close(fd); return "open_failed"; }
    }
    size_t capacity = standard ? 65536u : (size_t)before.st_size + 1u;
    char* data = malloc(capacity + 1u);
    const char* error = data ? NULL : "allocation_failed";
    size_t used = 0;
    while (!error && !awk_query_stop(query)) {
        if (used == capacity) {
            if (!standard) { error = "changed_during_read"; break; }
            if (capacity == AWK_QUERY_FILE_BYTES + 1u) { error = "source_limit"; break; }
            size_t next = capacity * 2u;
            if (next > AWK_QUERY_FILE_BYTES + 1u) next = AWK_QUERY_FILE_BYTES + 1u;
            char* grown = realloc(data, next + 1u);
            if (!grown) { error = "allocation_failed"; break; }
            data = grown; capacity = next;
        }
        size_t request = capacity - used;
        if (request > 65536u) request = 65536u;
        errno = 0;
        size_t got = fread(data + used, 1u, request, input);
        used += got;
        if (got > AWK_QUERY_SOURCE_BYTES - query->source_bytes) { error = "source_limit"; break; }
        query->source_bytes += got;
        if (ferror(input)) { error = "read_failed"; break; }
        if (used > AWK_QUERY_FILE_BYTES) { error = "source_limit"; break; }
        if (!got) break;
    }
    if (!standard) {
        if (!error && (fstat(fileno(input), &after) || stat(path, &named))) error = "stat_failed";
        if (!error && (!awk_query_same_file(&before, &after) || !awk_query_same_file(&before, &named) ||
                       used != (size_t)before.st_size)) error = "changed_during_read";
        if (fclose(input) && !error) error = "close_failed";
    }
    (void)awk_query_stop(query);
    if (query->cancelled || error) { free(data); return error; }
    data[used] = 0;
    if (memchr(data, 0, used)) { free(data); return "binary_input"; }
    size_t line = 0;
    for (size_t i = 0; i < used; i++) {
        if (data[i] == '\n') line = 0;
        else if (++line >= AWK_MAX_LINE) { free(data); return "line_limit"; }
    }
    *bytes = data; *size = used;
    return NULL;
}

static jdv_value_t awk_query_cell_value(awk_query_cell_t* cell) {
    if (!cell->numeric) return query_data_slice(JDV_BYTES, cell->bytes, cell->length);
    double number = cell->number;
    if (!(number == 0 && signbit(number)) && number == trunc(number)) {
        if (number >= -0x1p63 && number < 0x1p63) return jdv_value_i64((int64_t)number);
        if (number >= 0 && number < 0x1p64) return jdv_value_u64((uint64_t)number);
    }
    uint64_t bits; memcpy(&bits, &number, sizeof(bits));
    for (size_t i = 0; i < 8; i++) cell->bits[i] = (unsigned char)(bits >> (56u - (unsigned)i * 8u));
    (void)snprintf(cell->decimal, sizeof(cell->decimal), "%.17g", number);
    jdv_value_t value = query_data_record(cell->floating, &AWK_FLOAT_SCHEMA);
    cell->floating[0].value = query_data_slice(JDV_BYTES, cell->bits, sizeof(cell->bits));
    cell->floating[1].value = query_data_text(JDV_STRING, cell->decimal);
    cell->floating[2].value = query_data_text(JDV_SYMBOL, "ieee754-binary64");
    return value;
}

static jdv_value_t awk_query_value(awk_query_row_t* row, int complete) {
    jdv_value_t value = query_data_record(row->fields, &AWK_QUERY_SCHEMA);
    row->fields[0].value = jdv_value_bool(complete && !row->error);
    if (row->error) row->fields[1].value = query_data_text(JDV_SYMBOL, row->error);
    if (row->cell_count) {
        for (size_t i = 0; i < row->cell_count; i++) row->values[i] = awk_query_cell_value(&row->cells[i]);
        if (row->aggregate) row->fields[10].value = row->values[0];
        else { row->fields[2].value.kind = JDV_LIST; row->fields[2].value.as.list.items = row->values;
               row->fields[2].value.as.list.len = row->cell_count; }
    }
    if (row->file) row->fields[3].value = query_data_text(JDV_PATH, row->file);
    row->fields[4].value = query_data_text(JDV_SYMBOL, row->kind);
    if (row->line) row->fields[5].value = jdv_value_u64(row->line);
    row->fields[6].value = query_data_text(JDV_SYMBOL, "binary64");
    row->fields[7].value = jdv_value_u64(row->output_index);
    row->fields[8].value = query_data_text(JDV_SYMBOL, row->phase);
    row->fields[9].value = jdv_value_u64(row->record_index);
    if (row->text) row->fields[11].value = query_data_slice(JDV_BYTES, row->text, row->text_length);
    return value;
}

static void awk_query_json_cell(jash_json_writer_t* json, const awk_query_cell_t* cell) {
    if (!cell->numeric) { jash_jsonw_text_or_bytes(json, cell->bytes, cell->length); return; }
    double number = cell->number;
    if (number == 0 && signbit(number)) { jash_jsonw_raw(json, "-0.0"); return; }
    if (number == trunc(number)) {
        if (number >= -0x1p63 && number < 0x1p63) { jash_jsonw_safe_i64(json, (int64_t)number); return; }
        if (number >= 0 && number < 0x1p64) { jash_jsonw_safe_u64(json, (uint64_t)number); return; }
    }
    char decimal[32]; (void)snprintf(decimal, sizeof(decimal), "%.17g", number);
    if (number == trunc(number)) jash_jsonw_string(json, decimal);
    else jash_jsonw_raw(json, decimal);
}

static void awk_query_json_row(jash_json_writer_t* json, const awk_query_row_t* row) {
    if (row->error) {
        jash_jsonw_raw(json, "{\"complete\":false,\"error\":"); jash_jsonw_string(json, row->error);
        jash_jsonw_raw(json, ",\"file\":");
        if (row->file) jash_jsonw_string_or_bytes(json, row->file); else jash_jsonw_raw(json, "null");
        jash_jsonw_raw(json, "}"); return;
    }
    if (row->text) {
        jash_jsonw_raw(json, "{\"text\":"); jash_jsonw_text_or_bytes(json, row->text, row->text_length);
        jash_jsonw_raw(json, "}"); return;
    }
    if (row->aggregate) {
        jash_jsonw_raw(json, "{\"result\":"); awk_query_json_cell(json, &row->cells[0]);
        jash_jsonw_raw(json, "}"); return;
    }
    jash_jsonw_raw(json, "{\"fields\":[");
    for (size_t i = 0; i < row->cell_count; i++) {
        if (i) jash_jsonw_raw(json, ",");
        awk_query_json_cell(json, &row->cells[i]);
    }
    jash_jsonw_raw(json, "]}");
}

static int awk_query_emit(awk_query_t* query, jash_writer_t writer, void* context, int native) {
    (void)awk_query_stop(query);
    size_t first = query->offset < query->count ? (size_t)query->offset : query->count;
    size_t count = query->count - first;
    if ((uint64_t)count > query->limit) count = (size_t)query->limit;
    int page = count < query->count - first;
    const char* reason = query->cancelled ? "request_cancelled" : query->error ? query->error :
        query->limited ? query->limited : page ? "page_limit" : NULL;
    int status = query->cancelled ? query->cancelled : query->error ? 2 : query->limited ? 75 : 0;
    jdv_stream_state_t state = query->cancelled ? JDV_STREAM_CANCELLED : query->error ? JDV_STREAM_FAILED :
        query->limited || page ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE;
    if (native) {
        jdv_value_t* values = count ? calloc(count, sizeof(*values)) : NULL;
        if (count && !values) return query_data_error(writer, context, &AWK_QUERY_SCHEMA, "allocation_failed", 2);
        for (size_t i = 0; i < count; i++) values[i] = awk_query_value(&query->rows[first + i], state == JDV_STREAM_COMPLETE);
        int result = query_data_emit(writer, context, &AWK_QUERY_SCHEMA, values, count, query->errors, status, state, reason);
        free(values); return result;
    }
    jash_structured_output_t output;
    if (jash_structured_begin(&output, writer, context) < 0) return 125;
    jash_json_writer_t* json = &output.json;
    int envelope = state != JDV_STREAM_COMPLETE;
    if (envelope) {
        jash_jsonw_raw(json, "{\"schema\":\"jash.awk.v1\",\"complete\":false,\"reason\":");
        jash_jsonw_string(json, reason);
        jash_jsonw_raw(json, ",\"numeric_model\":\"binary64\",\"errors\":"); jash_jsonw_safe_u64(json, query->errors);
        jash_jsonw_raw(json, ",\"results\":");
    }
    int scalar = !envelope && query->aggregate && count == 1u;
    if (!scalar) jash_jsonw_raw(json, "[");
    for (size_t i = 0; i < count; i++) {
        if (i) jash_jsonw_raw(json, ",");
        awk_query_json_row(json, &query->rows[first + i]);
    }
    if (!scalar) jash_jsonw_raw(json, "]");
    if (envelope) jash_jsonw_raw(json, "}");
    jash_jsonw_raw(json, "\n");
    return jash_structured_finish(&output, status);
}

static int awk_query_error(jash_writer_t writer, void* context, int native, const char* error) {
    return native ? query_data_error(writer, context, &AWK_QUERY_SCHEMA, error, 2) :
        jash_structured_error(writer, context, "awk", error, 2);
}

static int awk_query_identifier(const char* value, size_t length) {
    if (!length || !((value[0] >= 'a' && value[0] <= 'z') ||
        (value[0] >= 'A' && value[0] <= 'Z') || value[0] == '_')) return 0;
    for (size_t i = 1; i < length; i++) if (!((value[i] >= 'a' && value[i] <= 'z') ||
        (value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= '0' && value[i] <= '9') || value[i] == '_')) return 0;
    return 1;
}

static int awk_query_run(jash_state_t* state, jash_writer_t writer, void* context,
                          int argc, char** argv, int native) {
    awk_query_t query = {.state = state, .limit = UINT64_MAX, .phase = "begin"};
    const char* error = NULL;
    int explicit_json = 0, explicit_data = 0, fs_whitespace = 1, index = 0;
    char separator = ' ', variable[AWK_STRBUF] = {0};
    const char* initial = NULL;
    size_t argument_bytes = 0;
    if (argc > 512) error = "argument_limit";
    for (int i = 0; i < argc && !error; i++) {
        size_t length = strlen(argv[i]);
        if (length > 256u * 1024u - argument_bytes) error = "argument_limit";
        else argument_bytes += length;
    }
    for (; index < argc && !error; index++) {
        const char* argument = argv[index];
        if (!strcmp(argument, "--")) { index++; break; }
        if (!strcmp(argument, "--json")) { explicit_json = 1; continue; }
        if (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2")) { explicit_data = 1; continue; }
        if (!strcmp(argument, "--offset") || !strcmp(argument, "--limit")) {
            int is_limit = !strcmp(argument, "--limit");
            uint64_t value;
            if (index + 1 >= argc || awk_query_number(argv[++index], &value) || (is_limit && !value)) error = "invalid_page";
            else if (is_limit) query.limit = value; else query.offset = value;
            continue;
        }
        if (argument[0] != '-' || !argument[1]) break;
        if (argument[1] == 'F') {
            const char* value = argument[2] ? argument + 2 : index + 1 < argc ? argv[++index] : NULL;
            if (!value || strlen(value) != 1u) error = "unsupported_separator";
            else { fs_whitespace = *value == ' '; separator = *value; }
        } else if (argument[1] == 'v') {
            const char* value = argument[2] ? argument + 2 : index + 1 < argc ? argv[++index] : NULL;
            const char* equals = value ? strchr(value, '=') : NULL;
            size_t length = equals ? (size_t)(equals - value) : 0;
            if (initial || !equals || length >= sizeof(variable) || !awk_query_identifier(value, length) ||
                strlen(equals + 1) >= AWK_MAX_LINE) error = "invalid_assignment";
            else {
                memcpy(variable, value, length); variable[length] = 0;
                if (awk_reserved_assign_target(variable) || awk_is_reserved_kw(variable)) error = "invalid_assignment";
                else initial = equals + 1;
            }
        } else error = "unsupported_option";
    }
    if (explicit_data && explicit_json) error = "conflicting_output_modes";
    if (index >= argc) error = "missing_program";
    if (error) return awk_query_error(writer, context, native, error);
    const char* program = argv[index++];
    if (strlen(program) > 65535u || argc - index > 256) return awk_query_error(writer, context, native, "argument_limit");

    /* strtod/printf use a private C numeric locale for the whole evaluation,
     * restoring the caller's locale before returning (no process-global setlocale). */
    locale_t numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!numeric_locale) return awk_query_error(writer, context, native, "numeric_locale_unavailable");
    locale_t previous_locale = uselocale(numeric_locale);
    if (!previous_locale) { freelocale(numeric_locale); return awk_query_error(writer, context, native, "numeric_locale_unavailable"); }
    awk_toklist_t tokens;
    awk_program_t parsed;
    int prepared = 0;
    if (awk_tokenize(program, &tokens) || parse_program(&tokens, &parsed)) error = "unsupported_program";
    else if (awk_pattern_prepare(&parsed.pattern)) error = "invalid_regex";
    else prepared = 1;
    awk_runtime_t* runtime = error ? NULL : calloc(1, sizeof(*runtime));
    if (!error && !runtime) error = "allocation_failed";
    if (error) {
        if (prepared) awk_pattern_cleanup(&parsed.pattern);
        (void)uselocale(previous_locale); freelocale(numeric_locale);
        return awk_query_error(writer, context, native, error);
    }
    if (initial) {
        awk_var_t* value = awk_var_lookup(runtime, variable);
        value->external = initial;
    }
    int end_outputs = 0;
    for (int i = 0; i < parsed.end.n; i++)
        if (parsed.end.stmts[i].kind == STMT_PRINT || parsed.end.stmts[i].kind == STMT_PRINTF) end_outputs++;
    query.aggregate = !parsed.main_implicit_print && !awk_block_has_print(&parsed.begin) &&
        !awk_block_has_print(&parsed.main_action) && end_outputs == 1 && awk_block_has_print_stmt(&parsed.end);
    if (parsed.has_begin) awk_query_block(&query, runtime, &parsed.begin);
    awk_stmt_t implicit = {.kind = STMT_PRINT};
    int sources = argc - index;
    int count = sources ? sources : 1;
    for (int source = 0; source < count && !awk_query_stop(&query) && (parsed.has_main || parsed.has_end); source++) {
        query.file = sources ? argv[index + source] : "-";
        query.phase = "input"; query.line = 0;
        char* bytes; size_t size;
        const char* input_error = awk_query_input(&query, query.file, &bytes, &size);
        if (input_error) {
            if (!strcmp(input_error, "source_limit")) query.limited = input_error;
            else awk_query_fail(&query, runtime, input_error);
            break;
        }
        if (awk_query_stop(&query)) { free(bytes); break; }
        query.phase = "record";
        size_t position = 0;
        while (position < size && !awk_query_stop(&query)) {
            char* start = bytes + position;
            char* newline = memchr(start, '\n', size - position);
            size_t length = newline ? (size_t)(newline - start) : size - position;
            position += length + (newline ? 1u : 0u);
            start[length] = 0;
            runtime->nr += 1; query.line++;
            if (awk_split_fields(runtime, start, fs_whitespace, separator)) {
                awk_query_fail(&query, runtime, "field_limit"); break;
            }
            if (parsed.has_main && awk_pattern_match(runtime, &parsed.pattern)) {
                if (parsed.main_implicit_print) awk_query_statement(&query, runtime, &implicit);
                else awk_query_block(&query, runtime, &parsed.main_action);
            }
        }
        free(bytes);
    }
    query.phase = "end";
    if (parsed.has_end && !awk_query_stop(&query)) awk_query_block(&query, runtime, &parsed.end);
    int result = awk_query_emit(&query, writer, context, native);
    for (size_t i = 0; i < query.count; i++) awk_query_row_free(&query.rows[i]);
    free(query.rows); free(runtime); awk_pattern_cleanup(&parsed.pattern);
    (void)uselocale(previous_locale); freelocale(numeric_locale);
    return result;
}

#endif
