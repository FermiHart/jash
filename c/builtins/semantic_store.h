// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Bounded reads of documented local stores. This is a storage boundary, never
 * a parser for a builtin's human stdout. JSON quoted YAML scalars reuse JDJ. */
#ifndef JASH_BUILTINS_SEMANTIC_STORE_H
#define JASH_BUILTINS_SEMANTIC_STORE_H
#include "semantic_builtin.h"
#include <stdio.h>
#include <sys/syscalls.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

#define SEMANTIC_STORE_BYTES (512u * 1024u)
#define SEMANTIC_STORE_ROWS 512u

static inline int semantic_home_path(char* path, size_t capacity, const char* suffix) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = jash_tmpdir();
    int n = snprintf(path, capacity, "%s/.jash/%s", home, suffix);
    if (n < 0 || (size_t)n >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static inline int semantic_store_directory(const char* suffix) {
    char path[4096]; struct stat st;
    if (semantic_home_path(path, sizeof(path), "")) return -1;
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (suffix && *suffix) {
        if (semantic_home_path(path, sizeof(path), suffix)) return -1;
        if (mkdir(path, 0700) && errno != EEXIST) return -1;
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    }
    return 0;
}

static inline int semantic_read_file(const char* path, size_t limit, char** data, size_t* size, int* exists) {
    *data = NULL; *size = 0; *exists = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    *exists = 1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    if (st.st_size < 0 || (uint64_t)st.st_size > limit) { close(fd); errno = E2BIG; return -1; }
    char* bytes = malloc(limit + 1);
    if (!bytes) { close(fd); return -1; }
    size_t used = 0; int failed = 0;
    for (;;) {
        ssize_t n = read(fd, bytes + used, limit + 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { failed = errno; break; }
        if (!n) break;
        used += (size_t)n;
        if (used > limit) { failed = E2BIG; break; }
    }
    if (close(fd) && !failed) failed = errno;
    if (failed) { free(bytes); errno = failed; return -1; }
    bytes[used] = 0; *data = bytes; *size = used; return 0;
}

static inline int semantic_write_file(const char* path, const void* data, size_t length, int append) {
    if (length > SEMANTIC_STORE_BYTES) { errno = E2BIG; return -1; }
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (append ? O_APPEND : 0), 0600);
    if (fd < 0) return -1;
    struct stat before;
    if (fstat(fd, &before)) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    if (!S_ISREG(before.st_mode)) { close(fd); errno = EINVAL; return -1; }
    if (append && (before.st_size < 0 || (uint64_t)before.st_size > SEMANTIC_STORE_BYTES - length)) {
        close(fd); errno = E2BIG; return -1;
    }
    if (!append && ftruncate(fd, 0)) { int saved = errno; close(fd); errno = saved; return -1; }
    FILE* file = fdopen(fd, append ? "ab" : "wb");
    if (!file) { int saved = errno; close(fd); errno = saved; return -1; }
    int failed = length && fwrite(data, 1, length, file) != length;
    int saved = failed ? errno : 0;
    if (fclose(file)) { failed = 1; if (!saved) saved = errno; }
    if (failed) { errno = saved ? saved : EIO; return -1; }
    return 0;
}

static inline int semantic_store_line_count(const char* path, const char* marker, uint64_t* count, int* exists) {
    char* data = NULL; size_t bytes = 0; *count = 0;
    if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, exists)) return -1;
    size_t offset = 0, marker_length = marker ? strlen(marker) : 0;
    while (offset < bytes) {
        size_t start = offset; while (offset < bytes && data[offset] != '\n') offset++;
        size_t length = offset - start; if (offset < bytes) offset++;
        if (length && data[start + length - 1] == '\r') length--;
        if (!marker || (length == marker_length && !memcmp(data + start, marker, length))) (*count)++;
    }
    free(data); return 0;
}

static inline int semantic_timestamp(char timestamp[64], char date[64]) {
    time_t now = time(NULL); struct tm local;
    if (now == (time_t)-1 || !localtime_r(&now, &local)) return -1;
    if (!strftime(timestamp, 64, "%Y-%m-%d %H:%M:%S", &local) ||
        !strftime(date, 64, "%Y-%m-%d", &local)) return -1;
    return 0;
}

static inline char* semantic_quoted(const char* text) {
    size_t size = strlen(text);
    if (size > 16384u || !jash_json_utf8_valid_n(text, size)) { errno = EILSEQ; return NULL; }
    size_t capacity = size * 6u + 3u;
    char* result = malloc(capacity); if (!result) return NULL;
    jash_json_buffer_t buffer = {result, capacity, 0}; jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer); jash_jsonw_string_n(&writer, text, size);
    if (jash_jsonw_finish(&writer)) { free(result); return NULL; }
    return result;
}

static inline int semantic_filename(const char* name, size_t maximum) {
    if (!name || !*name || strlen(name) > maximum || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-')) return 0;
    return 1;
}

static inline int semantic_slice_contains(jdv_slice_t source, const char* query) {
    size_t length = strlen(query);
    if (!length) return 1;
    for (size_t i = 0; i <= source.len && length <= source.len - i; i++)
        if (!memcmp(source.data + i, query, length)) return 1;
    return 0;
}

typedef struct {
    jdv_value_t category, content, learned, confidence, last_used;
    unsigned seen;
} semantic_memory_row_t;

/* The existing pattern/preference store is a small YAML subset: a list of
 * records with these five known keys. Strict JSON quoted scalars handle all
 * supported escapes; arbitrary YAML tags, aliases and duplicate keys fail. */
static inline int semantic_memory_parse(semantic_value_t* sem, char* data, size_t size,
                                        int preferences, semantic_memory_row_t* rows, size_t* count) {
    *count = 0;
    if (size && memchr(data, 0, size)) { errno = EINVAL; return -1; }
    size_t offset = 0;
    while (offset < size) {
        size_t start = offset; while (offset < size && data[offset] != '\n') offset++;
        size_t length = offset - start; if (offset < size) data[offset++] = 0;
        char* line = data + start;
        if (length && line[length - 1] == '\r') line[--length] = 0;
        if (!length) continue;
        if (length > 32768u) { errno = E2BIG; return -1; }
        int begins = length > 2 && line[0] == '-' && line[1] == ' ';
        if (begins) {
            if (*count == SEMANTIC_STORE_ROWS) { errno = E2BIG; return -1; }
            memset(&rows[(*count)++], 0, sizeof(*rows)); line += 2;
        } else {
            if (length < 3 || line[0] != ' ' || line[1] != ' ' || !*count) { errno = EINVAL; return -1; }
            line += 2;
        }
        char* colon = strchr(line, ':');
        if (!colon || colon[1] != ' ') { errno = EINVAL; return -1; }
        *colon = 0; char* scalar = colon + 2;
        semantic_memory_row_t* row = &rows[*count - 1]; unsigned bit = 0; jdv_value_t* target = NULL;
        if (!strcmp(line, preferences ? "preference" : "pattern")) { bit = 1; target = &row->content; }
        else if (preferences && !strcmp(line, "category")) { bit = 2; target = &row->category; }
        else if (!strcmp(line, "learned")) { bit = 4; target = &row->learned; }
        else if (!strcmp(line, "confidence")) { bit = 8; target = &row->confidence; }
        else if (!strcmp(line, "last_used")) { bit = 16; target = &row->last_used; }
        else { errno = EINVAL; return -1; }
        if (row->seen & bit) { errno = EINVAL; return -1; } row->seen |= bit;
        if (bit == 8) {
            jdj_value_t number;
            if (jdj_parse(&sem->arena, scalar, strlen(scalar), &sem->limits, &number) || number.kind != JDJ_NUMBER) return -1;
            char* end; errno = 0; double confidence = strtod(scalar, &end);
            if (errno || *end || !isfinite(confidence)) { errno = EINVAL; return -1; }
            *target = semantic_binary64(sem, confidence);
        } else if (*scalar == '"') {
            jdj_value_t value;
            if (jdj_parse(&sem->arena, scalar, strlen(scalar), &sem->limits, &value) || value.kind != JDJ_STRING) return -1;
            *target = semantic_text_n(sem, value.scalar.data, value.scalar.len);
        } else {
            if ((bit & 3u) || !*scalar) { errno = EINVAL; return -1; }
            *target = semantic_text(sem, scalar);
        }
        if (sem->error) { errno = E2BIG; return -1; }
    }
    for (size_t i = 0; i < *count; i++)
        if (!(rows[i].seen & 1u) || (preferences && !(rows[i].seen & 2u))) { errno = EINVAL; return -1; }
    return 0;
}

static inline jdv_value_t semantic_memory_value(semantic_value_t* sem, const semantic_memory_row_t* row, int preferences) {
    jdv_record_input_t fields[] = {
        semantic_field(preferences ? "preference" : "pattern", row->content),
        semantic_field("learned_local_time", row->learned),
        semantic_field("confidence_recorded", row->confidence),
        semantic_field("last_used", row->last_used),
        semantic_field("category", row->category)
    };
    return semantic_record(sem, fields, preferences ? 5 : 4);
}

static inline jdv_value_t semantic_store_receipt_value(semantic_builtin_t* call, const char* operation,
                                         const char* path, jdv_value_t details, int status) {
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("path", semantic_path(sem, path)),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("persisted", jdv_value_bool(!status)),
        semantic_field("effects_may_have_applied", jdv_value_bool(call->admitted)),
        semantic_field("durability", semantic_symbol(sem, "checked-close")),
        semantic_field("details", details),
        semantic_field("error", semantic_symbol(sem, status ? "store_operation_failed" : NULL))
    };
    return semantic_record(sem, fields, 9);
}

static inline int semantic_store_preflight(semantic_builtin_t* call, const char* operation,
        const char* path, jdv_value_t details) {
    size_t mark = call->values.arena.used;
    jdv_value_t candidate = semantic_store_receipt_value(call, operation, path, details, 1);
    int status = semantic_preflight(call, &candidate);
    call->values.arena.used = mark;
    return status;
}

static inline int semantic_store_receipt(semantic_builtin_t* call, const char* operation,
        const char* path, jdv_value_t details, int status) {
    return semantic_finish(call, semantic_store_receipt_value(call, operation, path, details, status), status);
}

static inline int semantic_memory_collect(semantic_value_t* sem, const char* path, int preferences,
                                          const char* query, jdv_value_t* result, int* exists) {
    char* data = NULL; size_t bytes = 0, count = 0;
    if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, exists)) return -1;
    semantic_memory_row_t* rows = calloc(SEMANTIC_STORE_ROWS, sizeof(*rows));
    jdv_value_t* values = calloc(SEMANTIC_STORE_ROWS, sizeof(*values));
    if (!rows || !values) { free(data); free(rows); free(values); return -1; }
    int failed = semantic_memory_parse(sem, data, bytes, preferences, rows, &count);
    size_t used = 0;
    if (!failed) for (size_t i = 0; i < count; i++) {
        if (query && !semantic_slice_contains(rows[i].content.as.slice, query) &&
            (!preferences || !semantic_slice_contains(rows[i].category.as.slice, query))) continue;
        values[used++] = semantic_memory_value(sem, &rows[i], preferences);
    }
    if (!failed) *result = semantic_list(sem, values, used);
    free(data); free(rows); free(values);
    return failed || sem->error ? -1 : 0;
}

static inline int semantic_memory_append(semantic_builtin_t* call, const char* operation,
                                         const char* path, const char* directory,
                                         const char* content, const char* category) {
    if (!*content || strlen(content) > 4095u || (category && strlen(category) > 255u))
        return semantic_error(call, "memory_field_limit", 2);
    char* quoted = semantic_quoted(content);
    char* group = category ? semantic_quoted(category) : NULL;
    if (!quoted || (category && !group)) { free(quoted); free(group); return semantic_error(call, "invalid_store_text", 2); }
    char timestamp[64], date[64];
    if (semantic_timestamp(timestamp, date)) { free(quoted); free(group); return semantic_error(call, "clock_unavailable", 1); }
    size_t capacity = strlen(quoted) + (group ? strlen(group) : 0) + 256u;
    char* stored = malloc(capacity);
    if (!stored) { free(quoted); free(group); return semantic_error(call, "allocation_failed", 125); }
    int length = category ? snprintf(stored, capacity,
        "- category: %s\n  preference: %s\n  learned: %s\n  confidence: 1.0\n  last_used: never\n", group, quoted, timestamp) :
        snprintf(stored, capacity, "- pattern: %s\n  learned: %s\n  confidence: 1.0\n  last_used: never\n", quoted, timestamp);
    free(quoted); free(group);
    if (length < 0 || (size_t)length >= capacity) { free(stored); return semantic_error(call, "store_record_limit", 75); }
    /* Refuse malformed/overfull existing stores before appending an otherwise
     * valid row. A failed query must not turn into a mutation. */
    jdv_value_t previous; int exists = 0;
    if (semantic_memory_collect(&call->values, path, category != NULL, NULL, &previous, &exists)) {
        free(stored); return semantic_error(call, "invalid_or_oversized_memory_store", 1);
    }
    if (previous.as.list.len >= SEMANTIC_STORE_ROWS) { free(stored); return semantic_error(call, "store_row_limit", 75); }
    semantic_value_t* sem = &call->values;
    size_t receipt_mark = sem->arena.used;
    jdv_record_input_t detail[] = {
        semantic_field(category ? "preference" : "pattern", semantic_text(sem, content)),
        semantic_field("category", semantic_text(sem, category)),
        semantic_field("learned_local_time", semantic_text(sem, timestamp)),
        semantic_field("confidence_source", semantic_symbol(sem, "constant-on-insertion")),
        semantic_field("bytes_written", jdv_value_filesize((uint64_t)length))
    };
    jdv_value_t candidate = semantic_record(sem, detail, 5);
    int preflight = semantic_store_preflight(call, operation, path, candidate);
    if (preflight) { free(stored); return semantic_error(call, "receipt_limit_before_store", preflight); }
    sem->arena.used = receipt_mark;
    if (semantic_admit(call)) { free(stored); return 125; }
    int failed = semantic_store_directory(directory) || semantic_write_file(path, stored, (size_t)length, 1);
    free(stored);
    detail[0].value = semantic_text(sem, content);
    detail[1].value = semantic_text(sem, category);
    detail[2].value = semantic_text(sem, timestamp);
    detail[3].value = semantic_symbol(sem, "constant-on-insertion");
    detail[4].value = failed ? jdv_value_nothing() : jdv_value_filesize((uint64_t)length);
    return semantic_store_receipt(call, operation, path, semantic_record(sem, detail, 5), failed ? 1 : 0);
}
#endif
