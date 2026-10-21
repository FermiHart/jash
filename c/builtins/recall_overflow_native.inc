// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
/* Included by recall_native.inc. Captures remain bytes; line selection never
 * discards empty lines or treats embedded NUL as end of capture. */
typedef struct { char name[256]; struct stat info; } recall_capture_t;

static int recall_capture_compare(const void* left, const void* right) {
    const recall_capture_t *a = left, *b = right;
    if (a->info.st_mtime != b->info.st_mtime) return a->info.st_mtime > b->info.st_mtime ? -1 : 1;
#if defined(__APPLE__)
    long an = a->info.st_mtimespec.tv_nsec, bn = b->info.st_mtimespec.tv_nsec;
#else
    long an = a->info.st_mtim.tv_nsec, bn = b->info.st_mtim.tv_nsec;
#endif
    return an != bn ? (an > bn ? -1 : 1) : strcmp(a->name, b->name);
}

static int recall_capture_scan(memory_store_t* store, recall_capture_t* captures, size_t* count) {
    if (store->directory < 0) return 0;
    struct stat before, after;
    if (fstat(store->directory, &before)) return -1;
    int fd = dup(store->directory); if (fd < 0) return -1;
    DIR* directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    int error = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (entry->d_name[0] == '.') continue;
        if (*count == 4096 || strlen(entry->d_name) >= sizeof(captures[0].name)) { error = E2BIG; break; }
        recall_capture_t* capture = &captures[*count];
        if (jash_copy_cstr(capture->name, sizeof(capture->name), entry->d_name)) { error = errno; break; }
        if (fstatat(store->directory, capture->name, &capture->info, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (!S_ISREG(capture->info.st_mode) || capture->info.st_uid != geteuid() ||
            capture->info.st_nlink != 1 || (capture->info.st_mode & 0022) || capture->info.st_size < 0) { error = EPERM; break; }
        (*count)++;
    }
    if (closedir(directory) && !error) error = errno;
    if (!error && fstat(store->directory, &after)) error = errno;
    if (!error && !memory_store_same(&before, &after)) error = ESTALE;
    if (error) { errno = error; return -1; }
    qsort(captures, *count, sizeof(*captures), recall_capture_compare); return 0;
}

static int recall_capture_contains(const char* line, size_t length, const char* pattern) {
    size_t size = strlen(pattern);
    if (!size) return 1;
    if (size > length) return 0;
    for (size_t i = 0; i <= length - size; i++) if (!memcmp(line + i, pattern, size)) return 1;
    return 0;
}

static int recall_native_overflow(semantic_builtin_t* call, int argc, char** argv) {
    const char* operands[3]; int count = 0, literal = 0; unsigned seen = 0;
    uint64_t offset = 0, limit = 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i], *value = NULL; unsigned bit = 0;
        if (!literal && !strcmp(argument, "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argument, "--offset")) bit = 1;
        else if (!literal && !strcmp(argument, "--limit")) bit = 2;
        else if (!literal && !strncmp(argument, "--offset=", 9)) { bit = 1; value = argument + 9; }
        else if (!literal && !strncmp(argument, "--limit=", 8)) { bit = 2; value = argument + 8; }
        if (bit) {
            if (seen & bit) return semantic_error(call, "invalid_arguments", 2);
            seen |= bit;
            if (!value) { if (++i == argc) return semantic_error(call, "invalid_arguments", 2); value = argv[i]; }
            if (memory_unsigned(value, bit == 1 ? 4u * 1024u * 1024u : 524288, bit == 1 ? &offset : &limit) || (bit == 2 && !limit))
                return semantic_error(call, "invalid_arguments", 2);
        } else {
            if (count == 3 || (!literal && argument[0] == '-')) return semantic_error(call, "invalid_arguments", 2);
            operands[count++] = argument;
        }
    }
    const char *handle = NULL, *selection = "read", *pattern = NULL;
    uint64_t line_limit = 0; int listing = !count || !strcmp(operands[0], "list");
    if (listing) {
        if (count > 1 || offset > 4096 || limit > 500) return semantic_error(call, "invalid_arguments", 2);
        selection = "list"; if (!limit) limit = 64;
    } else {
        int first = 0;
        if (strcmp(operands[0], "head") && strcmp(operands[0], "tail") && strcmp(operands[0], "grep")) handle = operands[first++];
        if (first < count) {
            selection = operands[first++];
            if ((!strcmp(selection, "head") || !strcmp(selection, "tail")) && first < count) {
                if (memory_unsigned(operands[first++], 500, &line_limit)) return semantic_error(call, "invalid_arguments", 2);
            } else if (!strcmp(selection, "grep") && first < count) pattern = operands[first++];
            else return semantic_error(call, "invalid_arguments", 2);
        }
        if (first != count || (strcmp(selection, "read") && limit > 500)) return semantic_error(call, "invalid_arguments", 2);
        if (!limit) limit = !strcmp(selection, "read") ? 65536 : 500;
    }
    char directory_path[PATH_MAX], selected_path[PATH_MAX];
    const char* home = getenv("HOME");
    int n = snprintf(directory_path, sizeof(directory_path), "%s/.jash/overflow", home ? home : "");
    if (n < 0 || (size_t)n >= sizeof(directory_path)) return semantic_error(call, "path_too_long", 2);
    selected_path[0] = 0;
    memory_store_t store = {-1, -1};
    recall_capture_t* captures = calloc(4096, sizeof(*captures));
    if (!captures) return semantic_error(call, "allocation_failed", 125);
    size_t captures_count = 0, emitted = 0, matched = 0, size = 0; char* bytes = NULL;
    int error = 0, present = 0; const char* failure = NULL;
    jdv_value_t results[500]; semantic_value_t* sem = &call->values;
    if (memory_store_open(&store, "overflow", 0) || recall_capture_scan(&store, captures, &captures_count)) error = errno;
    if (listing) present = store.directory >= 0;
    if (!error && listing) {
        present = store.directory >= 0; matched = captures_count;
        for (size_t i = (size_t)offset; i < captures_count && emitted < limit; i++) {
            recall_capture_t* capture = &captures[i];
            n = snprintf(selected_path, sizeof(selected_path), "%s/%s", directory_path, capture->name);
            if (n < 0 || (size_t)n >= sizeof(selected_path)) { error = ENAMETOOLONG; break; }
#if defined(__APPLE__)
            long ns = capture->info.st_mtimespec.tv_nsec;
#else
            long ns = capture->info.st_mtim.tv_nsec;
#endif
            jdv_record_input_t fields[] = {
                semantic_field("name", semantic_text(sem, capture->name)),
                semantic_field("path", semantic_path(sem, selected_path)),
                semantic_field("bytes", jdv_value_filesize((uint64_t)capture->info.st_size)),
                semantic_field("mtime_seconds", jdv_value_i64((int64_t)capture->info.st_mtime)),
                semantic_field("mtime_nanoseconds", jdv_value_u64((uint64_t)ns))
            };
            results[emitted++] = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
        }
    } else if (!error) {
        const recall_capture_t* selected = NULL;
        for (size_t i = 0; i < captures_count; i++) if (!handle || strstr(captures[i].name, handle)) { selected = &captures[i]; break; }
        if (!selected) { error = ENOENT; failure = "capture_not_found"; }
        else {
            n = snprintf(selected_path, sizeof(selected_path), "%s/%s", directory_path, selected->name);
            if (n < 0 || (size_t)n >= sizeof(selected_path)) error = ENAMETOOLONG;
            else if (memory_store_read(&store, selected->name, 4u * 1024u * 1024u, &bytes, &size, &present)) error = errno;
            else if (!present) { error = ESTALE; failure = "capture_disappeared"; }
        }
        if (!error && !strcmp(selection, "read")) {
            matched = size;
            size_t begin = offset < size ? (size_t)offset : size;
            size_t length = size - begin; if (length > limit) length = (size_t)limit;
            jdv_record_input_t fields[] = {
                semantic_field("byte_offset", jdv_value_u64(begin)),
                semantic_field("bytes", jdv_value_filesize(length)),
                semantic_field("content", semantic_bytes(sem, bytes + begin, length))
            };
            results[emitted++] = semantic_record(sem, fields, 3);
        } else if (!error) {
            size_t lines = 0;
            for (size_t i = 0; i < size; i++) if (bytes[i] == '\n') lines++;
            if (size && bytes[size - 1] != '\n') lines++;
            if (lines > 65536) error = E2BIG;
            size_t begin_line = !strcmp(selection, "tail") && lines > line_limit ? lines - (size_t)line_limit : 0;
            size_t end_line = !strcmp(selection, "head") && lines > line_limit ? (size_t)line_limit : lines;
            if (!strcmp(selection, "tail") && !line_limit) begin_line = lines;
            size_t position = 0, line = 0;
            while (!error && position < size) {
                const char* newline = memchr(bytes + position, '\n', size - position);
                size_t length = newline ? (size_t)(newline - bytes - position) : size - position;
                int selected_line = line >= begin_line && line < end_line && (!pattern || recall_capture_contains(bytes + position, length, pattern));
                if (selected_line) {
                    size_t index = matched++;
                    if (index >= offset && emitted < limit) {
                        jdv_record_input_t fields[] = {
                            semantic_field("line_number", jdv_value_u64(line + 1)),
                            semantic_field("byte_offset", jdv_value_u64(position)),
                            semantic_field("content", semantic_bytes(sem, bytes + position, length)),
                            semantic_field("terminated", jdv_value_bool(newline != NULL))
                        };
                        results[emitted++] = semantic_record(sem, fields, 4);
                    }
                }
                position += length + (newline ? 1u : 0u); line++;
            }
        }
    }
    if (memory_store_close(&store) && !error) error = errno;
    if (error && !failure) failure = memory_store_error(error);
    int read_bytes = !listing && !strcmp(selection, "read");
    uint64_t consumed = read_bytes ? (offset < size ? (size - offset > limit ? limit : size - offset) : 0) : emitted;
    int more = !error && offset + consumed < matched;
    jdv_record_input_t source_fields[] = {
        semantic_field("path", semantic_path(sem, listing || !selected_path[0] ? directory_path : selected_path)),
        semantic_field("present", jdv_value_bool(present)),
        semantic_field("read_complete", jdv_value_bool(!error)),
        semantic_field("bytes", listing ? jdv_value_nothing() : jdv_value_filesize(size)),
        semantic_field("records", jdv_value_u64(captures_count)),
        semantic_field("selection", semantic_symbol(sem, "newest_mtime_then_lexical_name")),
        semantic_field("consistency", semantic_symbol(sem, listing ? "live_directory_scan" : "stable_regular_file_metadata")),
        semantic_field("error", semantic_symbol(sem, failure)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    char operation[32]; snprintf(operation, sizeof(operation), "overflow_%s", selection);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("source", semantic_record(sem, source_fields, sizeof(source_fields) / sizeof(source_fields[0]))),
        semantic_field("results", semantic_list(sem, results, error ? 0 : emitted)),
        semantic_field("analysis", jdv_value_nothing()),
        semantic_field("matched", error ? jdv_value_nothing() : jdv_value_u64(matched)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", more ? jdv_value_u64(offset + consumed) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(more)),
        semantic_field("error", semantic_symbol(sem, failure)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    int result = semantic_finish(call, semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0])), error ? 1 : 0);
    free(bytes); free(captures); return result;
}
