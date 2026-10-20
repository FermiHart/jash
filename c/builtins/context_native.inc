// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Structured context uses the existing declaration/brace/query heuristics on
 * checked source observations. It never parses the human context renderer. */
#include "analysis_output.h"
#include "memory_store.h"
#include "json_git_observe.h"
#include <core/data_json.h>
#include <core/query_cancel.h>

#define CN_FILE_BYTES (1024u * 1024u)
#define CN_SOURCE_BYTES (64u * 1024u * 1024u)
#define CN_VISITS 65536u
typedef struct {
    char* path;
    struct stat observation;
    uint64_t bytes, lines;
    long score;
    jdv_value_t value;
} cn_file_t;
typedef struct {
    semantic_builtin_t* call;
    jash_state_t* state;
    cn_file_t* files;
    size_t count;
    uint64_t visited, excluded, source_bytes;
    int root, error, ask;
    ask_query_t query;
} cn_run_t;

static int cn_fail(cn_run_t* run, int error) {
    if (!run->error) run->error = error ? error : EIO;
    errno = run->error; return -1;
}
static int cn_poll(cn_run_t* run) {
    int status = jash_query_cancel_poll(run->state);
    if (!status) return 0;
    run->call->cancellation_status = status;
    return cn_fail(run, ECANCELED);
}

static int cn_read(cn_run_t* run, int directory, const char* name,
                   char** result, size_t* length, struct stat* observation) {
    *result = NULL; *length = 0;
    if (cn_poll(run)) return -1;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return cn_fail(run, errno);
    struct stat before, after; int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode)) error = EINVAL;
    else if (before.st_size < 0 || (uint64_t)before.st_size > CN_FILE_BYTES ||
             (uint64_t)before.st_size > CN_SOURCE_BYTES - run->source_bytes) error = E2BIG;
    size_t size = error ? 0 : (size_t)before.st_size;
    char* bytes = error ? NULL : malloc(size + 1u);
    if (!error && !bytes) error = ENOMEM;
    size_t used = 0;
    while (!error && used < size) {
        if (cn_poll(run)) { error = run->error; break; }
        size_t request = size - used; if (request > 16384u) request = 16384u;
        ssize_t n = read(fd, bytes + used, request);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n ? errno : ESTALE; break; }
        used += (size_t)n;
    }
    if (!error) {
        char extra; ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n) error = n < 0 ? errno : ESTALE;
        else if (fstat(fd, &after)) error = errno;
        else if (!memory_store_same(&before, &after)) error = ESTALE;
        else if (memchr(bytes, 0, size)) error = EINVAL;
    }
    if (close(fd) && !error) error = errno;
    if (error) { free(bytes); return cn_fail(run, error); }
    bytes[size] = 0; *result = bytes; *length = size; *observation = before;
    run->source_bytes += size; return 0;
}

/* Complete physical lines, including an unterminated final line. Native
 * observations do not turn a long line into several fictitious line numbers. */
static char* cn_line(char* bytes, size_t size, size_t* offset, size_t* length,
                      char* saved) {
    if (*offset == size) return NULL;
    char* start = bytes + *offset;
    char* newline = memchr(start, '\n', size - *offset);
    *length = newline ? (size_t)(newline - start) + 1u : size - *offset;
    *offset += *length; *saved = bytes[*offset]; bytes[*offset] = 0;
    return start;
}
static int cn_match(const char* line, const char* term) {
    size_t length = strlen(term);
    for (const unsigned char* p = (const unsigned char*)line; *p; p++) {
        size_t i = 0;
        while (i < length && p[i] && (unsigned char)tolower(p[i]) == (unsigned char)term[i]) i++;
        if (i == length) return 1;
    }
    return 0;
}
static jdv_value_t cn_line_value(semantic_value_t* values, uint64_t number,
                                  const char* text, size_t size) {
    jdv_record_input_t fields[] = {
        semantic_field("line", jdv_value_u64(number)),
        semantic_field("text", semantic_text_n(values, text, size))
    };
    return semantic_record(values, fields, 2);
}
static jdv_value_t cn_map_file(cn_run_t* run, cn_file_t* file, char* bytes, size_t size) {
    semantic_value_t* values = &run->call->values;
    jdv_value_t signatures[64]; size_t kept = 0, total = 0, offset = 0;
    char* line; size_t length; char saved;
    while ((line = cn_line(bytes, size, &offset, &length, &saved))) {
        if (cn_poll(run)) break;
        file->lines++;
        if (ctx_is_decl(line)) {
            total++;
            if (kept < 64) {
                ctx_clean_sig(line);
                signatures[kept++] = cn_line_value(values, file->lines, line, strlen(line));
            }
        }
        bytes[offset] = saved;
    }
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, file->path)),
        semantic_field("bytes", jdv_value_filesize(size)),
        semantic_field("lines", jdv_value_u64(file->lines)),
        semantic_field("signatures", semantic_list(values, signatures, kept)),
        semantic_field("signatures_total", jdv_value_u64(total)),
        semantic_field("signatures_omitted", jdv_value_u64(total - kept))
    };
    return semantic_record(values, fields, 6);
}
static long cn_quick_score(cn_run_t* run, const char* path, char* bytes, size_t size) {
    long score = ask_path_score(&run->query, path) * 3;
    size_t offset = 0, lines = 0, length; char saved; char* line;
    while (lines < ASK_PHASE1_MAX_LINES && (line = cn_line(bytes, size, &offset, &length, &saved))) {
        if (cn_poll(run)) break;
        lines++; int declaration = ctx_is_decl(line);
        for (int t = 0; t < run->query.n_terms; t++) if (cn_match(line, run->query.terms[t])) {
            score++;
            if (declaration) score += 5;
        }
        bytes[offset] = saved;
    }
    return score;
}
static int cn_name_compare(const void* left, const void* right) {
    return strcmp(*(char* const*)left, *(char* const*)right);
}
static int cn_walk(cn_run_t* run, int directory, const char* prefix, unsigned depth) {
    int duplicate = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (duplicate < 0) return cn_fail(run, errno);
    DIR* stream = fdopendir(duplicate);
    if (!stream) { int error = errno; close(duplicate); return cn_fail(run, error); }
    char** names = calloc(CN_VISITS, sizeof(*names)); size_t count = 0;
    if (!names) { closedir(stream); return cn_fail(run, ENOMEM); }
    struct dirent* entry;
    for (;;) {
        errno = 0; entry = readdir(stream);
        if (!entry) { if (errno) cn_fail(run, errno); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (cn_poll(run)) break;
        if (++run->visited > CN_VISITS || count == CN_VISITS) { cn_fail(run, E2BIG); break; }
        names[count] = strdup(entry->d_name);
        if (!names[count]) { cn_fail(run, ENOMEM); break; }
        count++;
    }
    if (closedir(stream)) cn_fail(run, errno);
    qsort(names, count, sizeof(*names), cn_name_compare);
    for (size_t i = 0; i < count && !run->error; i++) {
        if (cn_poll(run)) break;
        struct stat st;
        if (fstatat(directory, names[i], &st, AT_SYMLINK_NOFOLLOW)) { cn_fail(run, errno); break; }
        if ((S_ISDIR(st.st_mode) && (ctx_skip_dir(names[i]) || names[i][0] == '.')) ||
            (!S_ISDIR(st.st_mode) && (!S_ISREG(st.st_mode) || !ctx_is_source(names[i])))) {
            run->excluded++; continue;
        }
        char relative[1024];
        int n = snprintf(relative, sizeof(relative), "%s%s%s", prefix, *prefix ? "/" : "", names[i]);
        if (n < 0 || (size_t)n >= sizeof(relative)) { cn_fail(run, ENAMETOOLONG); break; }
        if (S_ISDIR(st.st_mode)) {
            if (depth == CTX_MAX_DEPTH) { cn_fail(run, E2BIG); break; }
            int child = openat(directory, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { cn_fail(run, errno); break; }
            (void)cn_walk(run, child, relative, depth + 1u);
            if (close(child)) cn_fail(run, errno);
        } else {
            if (run->count == CTX_MAX_FILES) { cn_fail(run, E2BIG); break; }
            cn_file_t* file = &run->files[run->count++];
            file->path = strdup(relative);
            if (!file->path) { cn_fail(run, ENOMEM); break; }
            char* bytes = NULL; size_t size = 0;
            if (cn_read(run, directory, names[i], &bytes, &size, &file->observation)) break;
            file->bytes = size;
            if (run->ask) file->score = cn_quick_score(run, relative, bytes, size);
            else file->value = cn_map_file(run, file, bytes, size);
            free(bytes);
            if (run->call->values.error) { cn_fail(run, ENOMEM); break; }
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names); return run->error ? -1 : 0;
}

static int cn_reopen(cn_run_t* run, const cn_file_t* file, char** bytes, size_t* size) {
    char path[1024]; memcpy(path, file->path, strlen(file->path) + 1u);
    int parent = openat(run->root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return cn_fail(run, errno);
    char* component = path; char* slash;
    while ((slash = strchr(component, '/'))) {
        *slash = 0;
        int next = openat(parent, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int error = next < 0 ? errno : 0;
        if (close(parent) && !error) error = errno;
        if (error) { if (next >= 0) close(next); return cn_fail(run, error); }
        parent = next; component = slash + 1;
    }
    struct stat actual;
    int status = cn_read(run, parent, component, bytes, size, &actual);
    if (close(parent)) cn_fail(run, errno);
    if (!status && !memory_store_same(&file->observation, &actual)) cn_fail(run, ESTALE);
    return run->error ? -1 : 0;
}
static int cn_file_compare(const void* left, const void* right) {
    const cn_file_t* a = left; const cn_file_t* b = right;
    return a->score != b->score ? (a->score > b->score ? -1 : 1) : strcmp(a->path, b->path);
}
static jdv_value_t cn_intent(cn_run_t* run, char** text) {
    const char* environment = getenv("JASH_INTENT");
    char* bytes = NULL; size_t size = 0; int present = 0, error = 0;
    const char* origin = environment && *environment ? "environment" : "intent_store";
    if (environment && *environment) {
        size = strlen(environment); present = 1;
        if (size > 65536u) error = E2BIG;
        else if (!(bytes = strdup(environment))) error = ENOMEM;
    } else {
        memory_store_t store = {-1, -1};
        if (memory_store_open(&store, NULL, 0) ||
            memory_store_read(&store, "intent", 65536u, &bytes, &size, &present)) error = errno;
        if (memory_store_close(&store) && !error) error = errno;
        if (!error && size && memchr(bytes, 0, size)) error = EINVAL;
        /* The stored intent is the first logical line, as in the text engine. */
        if (!error && bytes) { char* newline = memchr(bytes, '\n', size); if (newline) *newline = 0; }
    }
    if (error) cn_fail(run, error);
    *text = error ? NULL : bytes;
    if (error) free(bytes);
    semantic_value_t* values = &run->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("origin", semantic_symbol(values, origin)),
        semantic_field("present", jdv_value_bool(present)),
        semantic_field("read_complete", jdv_value_bool(!error)),
        semantic_field("source_bytes", jdv_value_filesize(size)),
        semantic_field("text", error ? jdv_value_nothing() : semantic_text(values, bytes)),
        semantic_field("error", semantic_symbol(values, error ? memory_store_error(error) : NULL))
    };
    return semantic_record(values, fields, 6);
}
static jdv_value_t cn_extract_ask(cn_run_t* run, cn_file_t* file, long intent_score) {
    char* bytes = NULL; size_t size = 0;
    if (cn_reopen(run, file, &bytes, &size)) { free(bytes); return jdv_value_nothing(); }
    semantic_value_t* values = &run->call->values;
    jdv_value_t signatures[ASK_MAX_SIGS], snippets[ASK_MAX_SNIPPETS];
    uint64_t snippet_lines[ASK_MAX_SNIPPETS];
    size_t signatures_kept = 0, signatures_total = 0, snippets_kept = 0, snippets_total = 0, offset = 0, length;
    int body_match = 0; char* line; char saved;
    file->score += intent_score;
    while ((line = cn_line(bytes, size, &offset, &length, &saved))) {
        if (cn_poll(run)) break;
        file->lines++; int match = 0, signature_matches = 0;
        for (int t = 0; t < run->query.n_terms; t++) if (cn_match(line, run->query.terms[t])) match = 1;
        if (match) body_match = 1;
        if (ctx_is_decl(line)) {
            /* Preserve snippet bytes before signature whitespace cleanup. */
            char* signature = strdup(line);
            if (!signature) { cn_fail(run, ENOMEM); bytes[offset] = saved; break; }
            ctx_clean_sig(signature);
            for (int t = 0; t < run->query.n_terms; t++) if (cn_match(signature, run->query.terms[t])) signature_matches++;
            if (signature_matches || (body_match && signatures_kept < 12)) {
                signatures_total++;
                if (signatures_kept < ASK_MAX_SIGS) {
                    signatures[signatures_kept++] = cn_line_value(values, file->lines, signature, strlen(signature));
                    file->score += signature_matches * 5;
                }
            }
            free(signature);
        }
        if (match) {
            int duplicate = 0;
            for (size_t i = 0; i < snippets_kept; i++) if (file->lines - snippet_lines[i] < ASK_SNIPPET_LINES * 2u) duplicate = 1;
            if (!duplicate) {
                snippets_total++;
                if (snippets_kept < ASK_MAX_SNIPPETS) {
                    snippet_lines[snippets_kept] = file->lines;
                    snippets[snippets_kept++] = cn_line_value(values, file->lines, line, length);
                }
            }
        }
        bytes[offset] = saved;
    }
    file->score += (long)snippets_kept * run->query.n_terms;
    free(bytes);
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, file->path)),
        semantic_field("bytes", jdv_value_filesize(size)),
        semantic_field("lines", jdv_value_u64(file->lines)),
        semantic_field("score", jdv_value_i64(file->score)),
        semantic_field("signatures", semantic_list(values, signatures, signatures_kept)),
        semantic_field("signatures_omitted", jdv_value_u64(signatures_total - signatures_kept)),
        semantic_field("snippets", semantic_list(values, snippets, snippets_kept)),
        semantic_field("snippets_omitted", jdv_value_u64(snippets_total - snippets_kept))
    };
    return semantic_record(values, fields, 8);
}

static jdv_value_t cn_inventory(cn_run_t* run, const char* root, const char* query) {
    semantic_value_t* values = &run->call->values;
    run->root = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (run->root < 0) { cn_fail(run, errno); return jdv_value_nothing(); }
    run->files = calloc(CTX_MAX_FILES, sizeof(*run->files));
    if (!run->files) { cn_fail(run, ENOMEM); return jdv_value_nothing(); }
    if (cn_walk(run, run->root, "", 0)) return jdv_value_nothing();
    size_t selected = run->count, positive = 0, extracted = 0;
    jdv_value_t intent = jdv_value_nothing(); char* intent_text = NULL;
    if (run->ask) {
        intent = cn_intent(run, &intent_text);
        long intent_score = 0;
        if (intent_text) for (int t = 0; t < run->query.n_terms; t++) if (cn_match(intent_text, run->query.terms[t])) intent_score += 5;
        free(intent_text);
        if (run->error) return jdv_value_nothing();
        qsort(run->files, run->count, sizeof(*run->files), cn_file_compare);
        while (positive < run->count && run->files[positive].score > 0) positive++;
        extracted = positive < 32 ? positive : 32;
        for (size_t i = 0; i < extracted && !run->error; i++) run->files[i].value = cn_extract_ask(run, &run->files[i], intent_score);
        if (run->error) return jdv_value_nothing();
        qsort(run->files, extracted, sizeof(*run->files), cn_file_compare);
        selected = extracted < 10 ? extracted : 10;
    }
    jdv_value_t* rows = calloc(selected ? selected : 1, sizeof(*rows));
    if (!rows) { cn_fail(run, ENOMEM); return jdv_value_nothing(); }
    for (size_t i = 0; i < selected; i++) rows[i] = run->files[i].value;
    jdv_record_input_t fields[] = {
        semantic_field("root", semantic_path(values, root)),
        semantic_field("files", semantic_list(values, rows, selected)),
        semantic_field("source_files", jdv_value_u64(run->count)),
        semantic_field("source_bytes_read", jdv_value_filesize(run->source_bytes)),
        semantic_field("entries_visited", jdv_value_u64(run->visited)),
        semantic_field("excluded_entries", jdv_value_u64(run->excluded)),
        semantic_field("query", semantic_text(values, query)),
        semantic_field("intent", intent),
        semantic_field("candidates", run->ask ? jdv_value_u64(positive) : jdv_value_nothing()),
        semantic_field("deeply_analyzed", run->ask ? jdv_value_u64(extracted) : jdv_value_nothing()),
        semantic_field("has_more", jdv_value_bool(run->ask && positive > selected)),
        semantic_field("heuristic", semantic_symbol(values, run->ask ? "path-and-first-50-lines-then-top-32-declarations-and-snippets" : "column-zero-declarations")),
        semantic_field("snapshot", semantic_symbol(values, "individually-stable-files-not-an-atomic-directory-snapshot"))
    };
    jdv_value_t result = semantic_record(values, fields, 13); free(rows); return result;
}

static jdv_value_t cn_fold(cn_run_t* run, const char* path) {
    char* bytes = NULL; size_t size = 0; struct stat observed;
    if (cn_read(run, AT_FDCWD, path, &bytes, &size, &observed)) return jdv_value_nothing();
    semantic_value_t* values = &run->call->values;
    jdv_value_t* lines = calloc(65536u, sizeof(*lines));
    if (!lines) { free(bytes); cn_fail(run, ENOMEM); return jdv_value_nothing(); }
    size_t count = 0, offset = 0, length; uint64_t number = 0, collapsed = 0, suppressed = 0;
    int depth = 0; char* line; char saved;
    while ((line = cn_line(bytes, size, &offset, &length, &saved))) {
        if (cn_poll(run)) break;
        number++; int delta = ctx_brace_delta(line);
        if (!depth) {
            if (count == 65536u) { cn_fail(run, E2BIG); break; }
            jdv_record_input_t fields[] = {
                semantic_field("line", jdv_value_u64(number)),
                semantic_field("text", semantic_text_n(values, line, length)),
                semantic_field("body_collapsed_after", jdv_value_bool(delta > 0))
            };
            lines[count++] = semantic_record(values, fields, 3);
            if (delta > 0) { depth += delta; collapsed++; }
        } else { depth += delta; if (depth <= 0) depth = 0; suppressed++; }
        bytes[offset] = saved;
    }
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, path)),
        semantic_field("bytes", jdv_value_filesize(size)),
        semantic_field("source_lines", jdv_value_u64(number)),
        semantic_field("lines", semantic_list(values, lines, count)),
        semantic_field("bodies_collapsed", jdv_value_u64(collapsed)),
        semantic_field("lines_suppressed", jdv_value_u64(suppressed)),
        semantic_field("remaining_brace_depth", jdv_value_i64(depth)),
        semantic_field("heuristic", semantic_symbol(values, "brace-depth-with-quoted-string-escapes"))
    };
    jdv_value_t result = semantic_record(values, fields, 8); free(lines); free(bytes); return result;
}

static int cn_json_integer(const jdj_value_t* value, int64_t* result) {
    if (!value || value->kind != JDJ_NUMBER || !value->scalar.len || value->scalar.len >= 32) return -1;
    char number[32]; memcpy(number, value->scalar.data, value->scalar.len); number[value->scalar.len] = 0;
    char* end = NULL; errno = 0; long long n = strtoll(number, &end, 10);
    if (errno || *end) return -1;
    *result = (int64_t)n; return 0;
}
static jdv_value_t cn_recent(cn_run_t* run) {
    memory_store_t store = {-1, -1}; char* bytes = NULL; size_t size = 0; int present = 0, error = 0;
    if (memory_store_open(&store, NULL, 0) || memory_store_read(&store, "session_meta.jsonl", CN_FILE_BYTES, &bytes, &size, &present)) error = errno;
    if (memory_store_close(&store) && !error) error = errno;
    size_t starts[10] = {0}, lengths[10] = {0}, count = 0, offset = 0, kept = 0;
    if (!error && size && (memchr(bytes, 0, size) || bytes[size - 1] != '\n')) error = EINVAL;
    semantic_value_t parsed;
    int initialized = !semantic_value_init(&parsed, 4u * 1024u * 1024u, CN_FILE_BYTES);
    if (!initialized && !error) error = ENOMEM;
    while (!error && offset < size) {
        if (cn_poll(run)) { error = run->error; break; }
        char* newline = memchr(bytes + offset, '\n', size - offset);
        size_t length = newline ? (size_t)(newline - bytes - offset) : 0;
        parsed.arena.used = 0; jdj_value_t row; int64_t timestamp, duration, status;
        if (!length || jdj_parse(&parsed.arena, bytes + offset, length, &parsed.limits, &row) || row.kind != JDJ_OBJECT || row.count != 4 ||
            cn_json_integer(jdj_get(&row, "ts", 2), &timestamp) || cn_json_integer(jdj_get(&row, "exit", 4), &status) ||
            cn_json_integer(jdj_get(&row, "dur_ms", 6), &duration) || duration < 0 ||
            !jdj_get(&row, "cmd", 3) || jdj_get(&row, "cmd", 3)->kind != JDJ_STRING) { error = EINVAL; break; }
        if (count == 65536u) { error = E2BIG; break; }
        if (kept == 10) { memmove(starts, starts + 1, 9u * sizeof(*starts)); memmove(lengths, lengths + 1, 9u * sizeof(*lengths)); kept--; }
        starts[kept] = offset; lengths[kept++] = length; count++; offset += length + 1u;
    }
    jdv_value_t records[10]; semantic_value_t* values = &run->call->values;
    for (size_t i = 0; i < kept && !error; i++) {
        parsed.arena.used = 0; jdj_value_t row; int64_t timestamp = 0, duration = 0, status = 0;
        if (jdj_parse(&parsed.arena, bytes + starts[i], lengths[i], &parsed.limits, &row)) { error = EINVAL; break; }
        (void)cn_json_integer(jdj_get(&row, "ts", 2), &timestamp);
        (void)cn_json_integer(jdj_get(&row, "exit", 4), &status);
        (void)cn_json_integer(jdj_get(&row, "dur_ms", 6), &duration);
        const jdj_value_t* command = jdj_get(&row, "cmd", 3);
        jdv_record_input_t fields[] = {
            semantic_field("source_line", jdv_value_u64(count - kept + i + 1u)),
            semantic_field("timestamp_seconds", jdv_value_i64(timestamp)),
            semantic_field("status", jdv_value_i64(status)),
            semantic_field("duration_ms", jdv_value_u64((uint64_t)duration)),
            semantic_field("command_recorded", semantic_text_n(values, command->scalar.data, command->scalar.len))
        };
        records[i] = semantic_record(values, fields, 5);
    }
    if (initialized) semantic_value_free(&parsed);
    if (error) cn_fail(run, error);
    jdv_record_input_t fields[] = {
        semantic_field("source", semantic_symbol(values, "session_meta.jsonl")),
        semantic_field("present", jdv_value_bool(present)),
        semantic_field("read_complete", jdv_value_bool(!error)),
        semantic_field("source_bytes", jdv_value_filesize(size)),
        semantic_field("total_records", error ? jdv_value_nothing() : jdv_value_u64(count)),
        semantic_field("records", semantic_list(values, records, error ? 0 : kept)),
        semantic_field("has_more", jdv_value_bool(!error && count > kept)),
        semantic_field("cryptographically_verified", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(values, error ? memory_store_error(error) : NULL))
    };
    jdv_value_t result = semantic_record(values, fields, 9); free(bytes); return result;
}
static jdv_value_t cn_bundle(cn_run_t* run) {
    char* intent_text = NULL;
    jdv_value_t intent = cn_intent(run, &intent_text); free(intent_text);
    jdv_value_t recent = cn_recent(run), git = jdv_value_nothing(), diff = jdv_value_nothing();
    jash_git_observation_t git_outcome = {0}, diff_outcome = {0};
    if (!run->call->cancellation_status) {
        /* These fixed subprocesses observe state. Admit the actual output
         * sink before starting them; final result still gets the ordinary
         * complete-document budget check after the bounded observations. */
        jdv_record_input_t candidate_fields[] = {
            semantic_field("intent", intent), semantic_field("recent", recent),
            semantic_field("pending_observation", semantic_symbol(&run->call->values, "git-status-and-diff"))
        };
        jdv_value_t candidate = semantic_record(&run->call->values, candidate_fields, 3);
        int admitted = semantic_preflight(run->call, &candidate);
        if (admitted) { cn_fail(run, admitted == 75 ? E2BIG : ENOMEM); return jdv_value_nothing(); }
        if (semantic_admit(run->call)) { cn_fail(run, EIO); return jdv_value_nothing(); }
        int status = jash_git_observe(&run->call->values, 0, &git, &git_outcome);
        if (status) cn_fail(run, EIO);
        /* A failed status query does not masquerade as a clean empty diff. */
        if (!status && jash_git_diff_stat_observe(&run->call->values, &diff, &diff_outcome)) cn_fail(run, EIO);
    }
    jdv_record_input_t fields[] = {
        semantic_field("intent", intent), semantic_field("recent", recent),
        semantic_field("git", git), semantic_field("diff_stat", diff),
        semantic_field("consistency", semantic_symbol(&run->call->values, "independent-read-only-observations"))
    };
    return semantic_record(&run->call->values, fields, 5);
}

static int cn_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = argc ? argv[0] : "bundle";
    if (!strcmp(operation, "-h") || !strcmp(operation, "--help")) operation = "help";
    int first = 1; if (argc > 1 && !strcmp(argv[1], "--")) first++;
    int operands = argc > 0 ? argc - first : 0;
    if ((!strcmp(operation, "bundle") || !strcmp(operation, "help")) ? operands != 0 :
        !strcmp(operation, "map") ? operands < 0 || operands > 1 :
        !strcmp(operation, "fold") ? operands != 1 :
        !strcmp(operation, "ask") ? operands < 1 : 1) return semantic_error(call, "invalid_arguments", 2);
    cn_run_t run = {.call = call, .state = state, .root = -1, .ask = !strcmp(operation, "ask")};
    jdv_value_t result = jdv_value_nothing();
    char* query = NULL;
    if (run.ask) {
        query = semantic_legacy_join(operands, argv + first);
        if (!query) return semantic_error(call, "allocation_failed", 125);
        /* The lexical term limits are explicit, not silent token clipping. */
        ask_tokenize(query, &run.query);
        size_t terms = 0; const char* p = query;
        while (*p) {
            while (*p && !ask_is_ident_char((unsigned char)*p)) p++;
            const char* start = p; while (*p && ask_is_ident_char((unsigned char)*p)) p++;
            if ((size_t)(p - start) >= 48u) { free(query); return semantic_error(call, "query_term_limit", 2); }
            if (p - start >= 2 && ++terms > ASK_MAX_TERMS) { free(query); return semantic_error(call, "query_term_limit", 2); }
        }
        if (!run.query.n_terms) { free(query); return semantic_error(call, "no_searchable_terms", 2); }
        result = cn_inventory(&run, ".", query);
    } else if (!strcmp(operation, "map")) result = cn_inventory(&run, operands ? argv[first] : ".", NULL);
    else if (!strcmp(operation, "fold")) result = cn_fold(&run, argv[first]);
    else if (!strcmp(operation, "bundle")) result = cn_bundle(&run);
    else {
        jdv_value_t operations[] = {semantic_symbol(&call->values, "bundle"), semantic_symbol(&call->values, "map [DIR]"),
            semantic_symbol(&call->values, "fold FILE"), semantic_symbol(&call->values, "ask QUERY...")};
        jdv_record_input_t field = semantic_field("operations", semantic_list(&call->values, operations, 4));
        result = semantic_record(&call->values, &field, 1);
    }
    if (run.root >= 0 && close(run.root)) cn_fail(&run, errno);
    for (size_t i = 0; i < run.count; i++) free(run.files[i].path);
    free(run.files); free(query);
    if (call->values.error) return semantic_error(call, "semantic_value_limit", 125);
    int status = call->cancellation_status ? call->cancellation_status :
        run.error == E2BIG || run.error == ENAMETOOLONG ? 75 : run.error ? 1 : 0;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.context.v1")),
        semantic_field("operation", semantic_symbol(&call->values, operation)),
        semantic_field("source_basis", semantic_symbol(&call->values, "checked-source-and-recorded-state")),
        semantic_field("complete", jdv_value_bool(!run.error)), semantic_field("result", result),
        semantic_field("error", semantic_symbol(&call->values, run.error ? memory_store_error(run.error) : NULL)),
        semantic_field("errno", run.error ? jdv_value_i64(run.error) : jdv_value_nothing())
    };
    jdv_value_t value = semantic_record(&call->values, fields, 7);
    int preflight = semantic_preflight(call, &value);
    if (preflight) return semantic_error(call, preflight == 75 ? "output_budget" : "semantic_value_limit", preflight);
    return semantic_finish(call, value, status);
}

SEMANTIC_ARGV_WRAPPER(context, "context", "jash.context.v1", cn_provider)
