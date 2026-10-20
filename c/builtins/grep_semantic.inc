// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Structured variants whose results are lines, predicates or counts rather
 * than the historical one-match-span projection. Both encoders consume these
 * native observations. No terminal rendering is captured or parsed. */
#include "semantic_value.h"
#include "memory_chain.h"
#include <poll.h>

#define GS_SOURCE_BYTES (64u * 1024u * 1024u)
#define GS_TOTAL_BYTES (256u * 1024u * 1024u)
#define GS_ARENA_BYTES (24u * 1024u * 1024u)
#define GS_ARENA_RESERVE (512u * 1024u)
#define GS_FUZZY_WORK UINT64_C(50000000)
#define GS_CONTEXT_MAX 4096u

typedef struct {
    semantic_value_t values;
    jdv_value_t items[QUERY_DATA_MAX_ITEMS + 1u];
    size_t count;
    uint64_t sources, source_failures, selected_lines, errors, work;
    int flags, predicate, any_selected, scan_complete, quiet_stop, command_error;
    const char *mode, *matcher, *limit_reason;
} grep_semantic_t;
static grep_semantic_t* g_grep_semantic;

/* The recorded timestamp is a filter boundary, not proof of attestation.
 * Reuse the bounded store parser, which rejects FIFO/symlink/corrupt sources
 * and verifies the descriptor did not change while it was read. */
static int grep_query_since(uint64_t sequence) {
    grep_query_t* request = g_grep_query;
    if (grep_query_stop()) return request->cancelled;
    memory_chain_t chain = {0};
    int loaded = memory_chain_load(&chain, 0, 0);
    int result = 2;
    if (grep_query_stop()) result = request->cancelled;
    else if (loaded) request->preflight_error = chain.error == E2BIG ? "since_chain_limit" : "since_chain_invalid";
    else if (!chain.present) request->preflight_error = "since_chain_missing";
    else {
        request->preflight_error = "since_sequence_not_found";
        for (size_t i = 0; i < chain.count; i++) {
            if (!(i % 256u) && grep_query_stop()) { result = request->cancelled; break; }
            if (chain.entries[i].sequence != sequence) continue;
            uint64_t seconds = chain.entries[i].seconds;
            time_t timestamp = (time_t)seconds;
            if (timestamp < 0 || (uint64_t)timestamp != seconds) request->preflight_error = "since_timestamp_range";
            else {
                g_grep_since_cutoff = timestamp; g_grep_since_active = 1;
                request->since_given = 1; request->since_sequence = sequence;
                request->since_bytes = chain.size;
                memcpy(request->since_path, chain.path, strlen(chain.path) + 1u);
                request->preflight_error = NULL; result = 0;
            }
            break;
        }
    }
    memory_chain_free(&chain);
    return result;
}

static int gs_stop(grep_semantic_t* query) {
    return query->quiet_stop || query->limit_reason || grep_query_stop();
}
static int gs_row_begin(grep_semantic_t* query, size_t* saved) {
    if (query->count >= QUERY_DATA_MAX_ITEMS || query->values.arena.used > GS_ARENA_BYTES - GS_ARENA_RESERVE) {
        query->limit_reason = "query_collection_limit"; return -1;
    }
    *saved = query->values.arena.used; return 0;
}
static int gs_row_end(grep_semantic_t* query, size_t saved, const jdv_record_input_t* fields, size_t count) {
    jdv_value_t row = semantic_record(&query->values, fields, count);
    if (query->values.error) {
        query->values.error = 0; query->values.arena.used = saved;
        query->limit_reason = "query_collection_limit"; return -1;
    }
    query->items[query->count++] = row; return 0;
}
static int gs_error_kind(grep_semantic_t* query, const char* path, const char* code, int number, int command) {
    query->errors++; query->source_failures += !command; query->scan_complete = 0;
    query->command_error |= command;
    size_t saved; if (gs_row_begin(query, &saved)) return -1;
    semantic_value_t* values = &query->values;
    jdv_record_input_t fields[] = {
        semantic_field("record", semantic_symbol(values, "error")),
        semantic_field("source", semantic_symbol(values, command ? "command" : path ? "file" : "stdin")),
        semantic_field("file", semantic_path(values, path)),
        semantic_field("error", semantic_symbol(values, code)),
        semantic_field("errno", number ? jdv_value_i64(number) : jdv_value_nothing())
    };
    return gs_row_end(query, saved, fields, sizeof fields / sizeof fields[0]);
}
static int gs_error(grep_semantic_t* query, const char* path, const char* code, int number) {
    return gs_error_kind(query, path, code, number, 0);
}
static int gs_line(grep_semantic_t* query, const char* path, uint64_t line, const char* bytes,
                      size_t size, int context, int matched, int start, int end, int span) {
    size_t saved; if (gs_row_begin(query, &saved)) return -1;
    semantic_value_t* values = &query->values;
    jdv_value_t location = jdv_value_nothing();
    if (start >= 0) {
        jdv_record_input_t fields[] = {semantic_field("start", jdv_value_u64((uint64_t)start)),
            semantic_field("end", jdv_value_u64((uint64_t)end))};
        location = semantic_record(values, fields, 2);
    }
    jdv_record_input_t fields[] = {
        semantic_field("record", semantic_symbol(values, span ? "span" : "line")),
        semantic_field("source", semantic_symbol(values, path ? "file" : "stdin")),
        semantic_field("file", semantic_path(values, path)),
        semantic_field("line", jdv_value_u64(line)),
        semantic_field("content", semantic_bytes(values, bytes, size)),
        semantic_field("role", semantic_symbol(values, context ? "context" : "selected")),
        semantic_field("matched", matched < 0 ? jdv_value_nothing() : jdv_value_bool(matched)),
        semantic_field("span", location),
        semantic_field("unit", semantic_symbol(values, "byte"))
    };
    return gs_row_end(query, saved, fields, sizeof fields / sizeof fields[0]);
}
static int gs_count(grep_semantic_t* query, const char* path, uint64_t count, int complete) {
    size_t saved; if (gs_row_begin(query, &saved)) return -1;
    semantic_value_t* values = &query->values;
    jdv_record_input_t fields[] = {
        semantic_field("record", semantic_symbol(values, "count")),
        semantic_field("source", semantic_symbol(values, path ? "file" : "stdin")),
        semantic_field("file", semantic_path(values, path)),
        semantic_field("count", jdv_value_u64(count)),
        semantic_field("complete", jdv_value_bool(complete))
    };
    return gs_row_end(query, saved, fields, sizeof fields / sizeof fields[0]);
}
static int gs_file(grep_semantic_t* query, const char* path, int matched, int scanned) {
    size_t saved; if (gs_row_begin(query, &saved)) return -1;
    semantic_value_t* values = &query->values;
    jdv_record_input_t fields[] = {
        semantic_field("record", semantic_symbol(values, "file")),
        semantic_field("source", semantic_symbol(values, path ? "file" : "stdin")),
        semantic_field("file", semantic_path(values, path)),
        semantic_field("matched", jdv_value_bool(matched)),
        semantic_field("complete", jdv_value_bool(1)),
        semantic_field("scanned_to_end", jdv_value_bool(scanned))
    };
    return gs_row_end(query, saved, fields, sizeof fields / sizeof fields[0]);
}

/* Materialize one bounded, descriptor-stable regular source before any row.
 * Stdin is a separate byte stream. poll wakes every 50ms to observe runtime
 * cancellation; a named FIFO is never opened as a blocking file source. */
static const char* gs_input(grep_semantic_t* query, const char* path, char** output,
                              size_t* size, int* error_number) {
    *output = NULL; *size = 0; *error_number = 0;
    if (gs_stop(query)) return NULL;
    int descriptor = path ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : STDIN_FILENO;
    if (descriptor < 0) { *error_number = errno; return "open_failed"; }
    struct stat before = {0}, after, named;
    const char* error = NULL;
    if (fstat(descriptor, &before)) { error = "stat_failed"; *error_number = errno; }
    else if (path && !S_ISREG(before.st_mode)) error = "non_regular_input";
    else if (!path && isatty(descriptor)) error = "stdin_terminal_not_supported";
    else if (path && (before.st_size < 0 || (uint64_t)before.st_size > GS_SOURCE_BYTES)) error = "source_byte_limit";
    if (error) { if (path) (void)close(descriptor); return error; }
    size_t capacity = path ? (size_t)before.st_size + 1u : 65536u;
    char* bytes = malloc(capacity + 1u);
    if (!bytes) { error = "allocation_failed"; *error_number = ENOMEM; }
    size_t used = 0;
    while (!error && !gs_stop(query)) {
        if (used == capacity) {
            if (path) { error = "changed_during_read"; break; }
            if (capacity == GS_SOURCE_BYTES + 1u) { error = "source_byte_limit"; break; }
            size_t grown = capacity * 2u;
            if (grown > GS_SOURCE_BYTES + 1u) grown = GS_SOURCE_BYTES + 1u;
            char* resized = realloc(bytes, grown + 1u);
            if (!resized) { error = "allocation_failed"; *error_number = ENOMEM; break; }
            bytes = resized; capacity = grown;
        }
        if (!path && !S_ISREG(before.st_mode)) {
            struct pollfd ready = {.fd = descriptor, .events = POLLIN};
            int polled = poll(&ready, 1, 50);
            if (polled < 0) { if (errno == EINTR) continue; error = "stdin_poll_failed"; *error_number = errno; break; }
            if (!polled) continue;
            if (ready.revents & POLLNVAL) { *error_number = EBADF; error = "stdin_poll_failed"; break; }
            if (!(ready.revents & (POLLIN | POLLHUP | POLLERR))) continue;
        }
        size_t request = capacity - used; if (request > 65536u) request = 65536u;
        ssize_t got = read(descriptor, bytes + used, request);
        if (got < 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; error = "read_failed"; *error_number = errno; break; }
        if (!got) break;
        used += (size_t)got;
        if ((uint64_t)got > GS_TOTAL_BYTES - g_grep_query->scanned_bytes) {
            query->limit_reason = "total_source_byte_limit"; break;
        }
        g_grep_query->scanned_bytes += (uint64_t)got;
        if (used > GS_SOURCE_BYTES) { error = "source_byte_limit"; break; }
    }
    if (path) {
        if (!error && !gs_stop(query) && (fstat(descriptor, &after) || stat(path, &named))) {
            error = "stat_failed"; *error_number = errno;
        }
        if (!error && !gs_stop(query) && (!grep_stat_unchanged(&before, &after) ||
            !grep_stat_unchanged(&before, &named) || used != (size_t)before.st_size)) error = "changed_during_read";
        if (close(descriptor) && !error) { error = "close_failed"; *error_number = errno; }
    }
    if (error || gs_stop(query)) { free(bytes); return error; }
    bytes[used] = 0;
    if (memchr(bytes, 0, used)) { free(bytes); return "binary_input"; }
    size_t line = 0;
    for (size_t i = 0; i < used; i++) {
        if (!(i % 65536u) && gs_stop(query)) { free(bytes); return NULL; }
        if (bytes[i] == '\n') line = 0;
        else if (++line > GREP_JSON_MAX_LINE_BYTES) { free(bytes); return "line_exceeds_json_limit"; }
    }
    *output = bytes; *size = used; return NULL;
}

#ifdef JASH_HAVE_REGEX
static int gs_fuzzy(grep_semantic_t* query, const char* line, const char* pattern, int from, int* start, int* end) {
    size_t pattern_bytes = strlen(pattern);
    if (!pattern_bytes) return 0;
    const char* word = line;
    while (*word) {
        while (*word == ' ' || *word == '\t') word++;
        const char* finish = word; while (*finish && *finish != ' ' && *finish != '\t') finish++;
        size_t size = (size_t)(finish - word);
        size_t skip = from > word - line ? (size_t)(from - (word - line)) : 0;
        for (size_t i = skip; pattern_bytes <= size && i <= size - pattern_bytes; i++) {
            uint64_t cost = (pattern_bytes + 1u) * (pattern_bytes + 1u);
            if (cost > GS_FUZZY_WORK - query->work) { query->limit_reason = "fuzzy_work_limit"; return 0; }
            query->work += cost;
            if (!(i % 64u) && gs_stop(query)) return 0;
            char window[65]; memcpy(window, word + i, pattern_bytes); window[pattern_bytes] = 0;
            if (levenshtein(window, pattern) <= 2) {
                *start = (int)(word + i - line); *end = *start + (int)pattern_bytes; return 1;
            }
        }
        word = finish;
    }
    return 0;
}
static int gs_match(grep_semantic_t* query, const char* line, int from, int* start, int* end) {
    if (!(query->flags & 0x06)) return grep_query_span_multi(line, query->flags, from, start, end);
    const grep_query_matcher_t* matchers = g_grep_query->matchers;
    int found = 0;
    for (size_t i = 0; i < g_grep_query->matcher_count; i++) {
        int left = -1, right = -1;
        if (query->flags & 0x02) {
            (void)gs_fuzzy(query, line, matchers[i].text, from, &left, &right);
        } else if (smart_match(line + from, matchers[i].text)) {
            /* The legacy smart engine is OR of case-insensitive word
             * substrings. Its suffix probes are contained by the base probe.
             * Retain the actual base-word span which established the result. */
            char words[256]; memcpy(words, matchers[i].text, strlen(matchers[i].text) + 1u);
            char* state = NULL;
            for (char* word = strtok_r(words, " \t", &state); word; word = strtok_r(NULL, " \t", &state)) {
                const char* match = jash_strcasestr_portable(line + from, word);
                if (!match) continue;
                int begin = (int)(match - line), finish = begin + (int)strlen(word);
                if (left < 0 || begin < left || (begin == left && finish > right)) { left = begin; right = finish; }
            }
        }
        if (left >= 0 && (!found || left < *start || (left == *start && right > *end))) {
            *start = left; *end = right; found = 1;
        }
        if (gs_stop(query)) break;
    }
    return found;
}
#endif

typedef struct { size_t offset, size; uint64_t number; } gs_previous_t;
static void gs_source(grep_semantic_t* query, const char* path) {
    char* bytes = NULL; size_t size = 0; int number = 0;
    const char* error = gs_input(query, path, &bytes, &size, &number);
    if (error) { (void)gs_error(query, path, error, number); return; }
    if (!bytes) return;
    query->sources++;
    unsigned before = (unsigned)g_ctx_before, after = (unsigned)g_ctx_after;
    gs_previous_t* previous = before ? calloc(before, sizeof(*previous)) : NULL;
    if (before && !previous) { free(bytes); (void)gs_error(query, path, "allocation_failed", ENOMEM); return; }
    uint64_t selected = 0, line = 0, emitted_line = 0;
    size_t offset = 0, ring = 0; unsigned after_pending = 0;
    int whole = 1, maximum_reached = 0;
    while (offset < size && !gs_stop(query)) {
        size_t first = offset; while (offset < size && bytes[offset] != '\n') offset++;
        size_t length = offset - first;
        if (offset < size) bytes[offset++] = 0;
        line++;
        int left = -1, right = -1, matched = 0;
#ifdef JASH_HAVE_REGEX
        if (!maximum_reached) matched = gs_match(query, bytes + first, 0, &left, &right);
#endif
        if (gs_stop(query)) { whole = 0; break; }
        int hit = !maximum_reached && ((query->flags & 0x20) ? !matched : matched);
        if (hit) {
            selected++; query->selected_lines++; query->any_selected = 1;
            if (!(query->flags & 0x400000)) query->predicate = 1;
            if (query->flags & 0x100) {
                query->predicate = 1; query->quiet_stop = 1; whole = offset == size; break;
            }
            if (query->flags & 0x400000) { whole = offset == size; break; }
            if (query->flags & 0x80) {
                /* Counts have no per-line records. */
            } else if (query->flags & 0x1000) {
                (void)gs_file(query, path, 1, offset == size); query->predicate = 1; whole = offset == size; break;
            } else {
                for (unsigned i = 0; i < before && !gs_stop(query); i++) {
                    const gs_previous_t* old = &previous[(ring + i) % before];
                    if (old->number && old->number > emitted_line) {
                        (void)gs_line(query, path, old->number, bytes + old->offset, old->size, 1, -1, -1, -1, 0);
                        emitted_line = old->number;
                    }
                }
                if (query->flags & 0x2000) {
#ifdef JASH_HAVE_REGEX
                    if (!(query->flags & 0x20)) {
                        int from = 0;
                        while (from <= (int)length && gs_match(query, bytes + first, from, &left, &right)) {
                            if (gs_stop(query)) break;
                            (void)gs_line(query, path, line, bytes + first + left, (size_t)(right - left), 0, 1, left, right, 1);
                            from = right > left ? right : right + 1;
                        }
                    }
#endif
                } else (void)gs_line(query, path, line, bytes + first, length, 0, matched,
                    (query->flags & 0x20) ? -1 : left, (query->flags & 0x20) ? -1 : right, 0);
                emitted_line = line; after_pending = after;
            }
            if (g_grep_max > 0 && selected >= (uint64_t)g_grep_max) {
                maximum_reached = 1;
                if (offset < size) g_grep_query->match_limited = 1;
                if (!after_pending) { whole = offset == size; break; }
            }
        } else if (after_pending) {
            (void)gs_line(query, path, line, bytes + first, length, 1, -1, -1, -1, 0);
            emitted_line = line; after_pending--;
            if (maximum_reached && !after_pending) { whole = offset == size; break; }
        }
        if (before) { previous[ring] = (gs_previous_t){first, length, line}; ring = (ring + 1u) % before; }
    }
    if (offset < size || gs_stop(query)) whole = 0;
    if (!whole) query->scan_complete = 0;
    if (!(query->flags & 0x100) && !gs_stop(query)) {
        if (query->flags & 0x400000) {
            if (!selected && whole) { (void)gs_file(query, path, 0, 1); query->predicate = 1; }
        } else if (query->flags & 0x80) { (void)gs_count(query, path, selected, whole); if (selected) query->predicate = 1; }
        else if (!(query->flags & 0x1000) && selected) query->predicate = 1;
    }
    free(previous); free(bytes);
}

static void gs_walk(grep_semantic_t* query, const char* path, unsigned depth) {
    if (gs_stop(query)) return;
    if (depth > 64u) { query->limit_reason = "walk_depth_limit"; return; }
    DIR* directory = opendir(path);
    if (!directory) { (void)gs_error(query, path, "directory_open_failed", errno); return; }
    for (;;) {
        if (gs_stop(query)) break;
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) (void)gs_error(query, path, "directory_read_failed", errno); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++g_grep_query->visits > 100000u) { query->limit_reason = "walk_visit_limit"; break; }
        char child[4096]; int written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof child) { (void)gs_error(query, path, "path_exceeds_json_limit", 0); continue; }
        struct stat status;
        if (lstat(child, &status)) { (void)gs_error(query, child, "stat_failed", errno); continue; }
        if (S_ISDIR(status.st_mode)) {
            if (!grep_name_excluded(g_grep_excl_dir, g_grep_excl_dir_n, entry->d_name)) gs_walk(query, child, depth + 1u);
        } else if (S_ISREG(status.st_mode)) {
            if (!grep_name_included(g_grep_incl, g_grep_incl_n, entry->d_name) ||
                grep_name_excluded(g_grep_excl, g_grep_excl_n, entry->d_name) ||
                (g_grep_since_active && status.st_mtime < g_grep_since_cutoff)) continue;
            gs_source(query, child);
        }
    }
    if (closedir(directory)) (void)gs_error(query, path, "directory_close_failed", errno);
}

static int grep_semantic_run(int flags, int recursive, int count, const char* const* operands) {
    grep_semantic_t* query = calloc(1, sizeof(*query));
    if (!query || semantic_value_init(&query->values, GS_ARENA_BYTES, QUERY_DATA_MAX_BYTES)) { free(query); return 125; }
    g_grep_semantic = query; query->flags = flags; query->scan_complete = 1; g_grep_query->started = 1;
    query->mode = flags & 0x100 ? "quiet" : flags & 0x400000 ? "files-without-match" :
        flags & 0x80 ? "count" : flags & 0x1000 ? "files-with-matches" : flags & 0x2000 ? "spans" : "lines";
    query->matcher = flags & 0x02 ? "fuzzy" : flags & 0x04 ? "smart" : flags & 0x400 ? "fixed" :
        flags & 0x200000 ? "pcre-lite" : flags & 0x200 ? "ere" : "bre";
#ifndef JASH_HAVE_REGEX
    (void)recursive; (void)count; (void)operands;
    (void)gs_error_kind(query, NULL, "structured_matcher_unavailable", 0, 1); return 2;
#else
    const grep_query_matcher_t* matchers = g_grep_query->matchers;
    for (size_t i = 0; i < g_grep_query->matcher_count; i++) {
        size_t length = strlen(matchers[i].text);
        if (((flags & 0x02) && length > 64u) || (!(flags & 0x02) && (flags & 0x04) && length > 255u)) {
            (void)gs_error_kind(query, NULL, "pattern_byte_limit", 0, 1); return 2;
        }
    }
    if (g_grep_query->max_zero) {
        /* Zero admits no source observation, including an unclosed stdin.
         * An unobserved per-file count is not manufactured as an exact zero. */
        g_grep_query->match_limited = 1; query->scan_complete = 0;
        return 1;
    }
    if (!count) gs_source(query, NULL);
    for (int i = 0; i < count && !gs_stop(query); i++) {
        if (!strcmp(operands[i], "-")) { gs_source(query, NULL); continue; }
        struct stat status;
        if (stat(operands[i], &status)) { (void)gs_error(query, operands[i], "stat_failed", errno); continue; }
        if (S_ISDIR(status.st_mode)) {
            if (recursive) gs_walk(query, operands[i], 0);
            else (void)gs_error(query, operands[i], "directory_requires_recursive", 0);
        } else gs_source(query, operands[i]);
    }
    if (g_grep_query->cancelled) return g_grep_query->cancelled;
    if (query->limit_reason || g_grep_query->capped) return 75;
    return query->errors ? 2 : query->predicate ? 0 : 1;
#endif
}

static int grep_semantic_emit(grep_semantic_t* query, jash_writer_t writer, void* context, int producer) {
    grep_query_t* request = g_grep_query;
    if (request->cancelled) producer = request->cancelled;
    size_t first = request->offset < query->count ? (size_t)request->offset : query->count;
    size_t count = query->count - first;
    if ((uint64_t)count > request->limit) count = (size_t)request->limit;
    int page = count < query->count - first;
    const char* reason = request->cancelled ? "request_cancelled" : query->limit_reason ? query->limit_reason :
        request->capped ? "query_collection_limit" : request->match_limited ? "match_limit" :
        query->command_error ? "command_error" : query->errors ? "source_errors" : page ? "page_limit" : NULL;
    int complete = reason == NULL;
    jdv_stream_state_t state = request->cancelled ? JDV_STREAM_CANCELLED :
        query->limit_reason || request->capped || request->match_limited || page ? JDV_STREAM_INCOMPLETE :
        query->errors ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    semantic_value_t* values = &query->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.grep.query.v1")),
        semantic_field("record", semantic_symbol(values, "summary")),
        semantic_field("mode", semantic_symbol(values, query->mode)),
        semantic_field("matcher", semantic_symbol(values, query->matcher)),
        semantic_field("inverted", jdv_value_bool((query->flags & 0x20) != 0)),
        semantic_field("any_selected_line", jdv_value_bool(query->any_selected)),
        semantic_field("predicate", jdv_value_bool(query->predicate)),
        semantic_field("complete", jdv_value_bool(complete)),
        semantic_field("scan_complete", jdv_value_bool(query->scan_complete && !reason)),
        semantic_field("sources_observed", jdv_value_u64(query->sources)),
        semantic_field("source_failures", jdv_value_u64(query->source_failures)),
        semantic_field("source_bytes", jdv_value_u64(request->scanned_bytes)),
        semantic_field("selected_lines_observed", jdv_value_u64(query->selected_lines)),
        semantic_field("records_total", jdv_value_u64(query->count)),
        semantic_field("records_returned", jdv_value_u64(count)),
        semantic_field("offset", jdv_value_u64(request->offset)),
        semantic_field("limit", request->limit == UINT64_MAX ? jdv_value_nothing() : jdv_value_u64(request->limit)),
        semantic_field("next_offset", page ? jdv_value_u64(request->offset + count) : jdv_value_nothing()),
        semantic_field("context_before", jdv_value_u64((uint64_t)g_ctx_before)),
        semantic_field("context_after", jdv_value_u64((uint64_t)g_ctx_after)),
        semantic_field("max_count", request->max_zero || g_grep_max ? jdv_value_u64((uint64_t)g_grep_max) : jdv_value_nothing()),
        semantic_field("stop_reason", semantic_symbol(values, reason)),
        semantic_field("exit_code", jdv_value_i64(producer)),
        semantic_field("source_policy", semantic_symbol(values, "stable-regular-or-stdin-eof")),
        semantic_field("since_sequence", request->since_given ? jdv_value_u64(request->since_sequence) : jdv_value_nothing()),
        semantic_field("since_seconds", request->since_given ? jdv_value_u64((uint64_t)g_grep_since_cutoff) : jdv_value_nothing()),
        semantic_field("since_source", semantic_path(values, request->since_given ? request->since_path : NULL)),
        semantic_field("since_source_bytes", request->since_given ? jdv_value_u64(request->since_bytes) : jdv_value_nothing()),
        semantic_field("fuzzy_distance", query->flags & 0x02 ? jdv_value_u64(2) : jdv_value_nothing()),
        semantic_field("fuzzy_work", jdv_value_u64(query->work)),
        semantic_field("smart_rule", query->flags & 0x04 && !(query->flags & 0x02) ?
            semantic_symbol(values, "case-insensitive-any-pattern-word-substring") : jdv_value_nothing())
    };
    jdv_value_t summary = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    if (values->error) return 125;
    if (request->native) {
        if (count) memmove(query->items, query->items + first, count * sizeof(query->items[0]));
        query->items[count] = summary;
        jdv_schema_t record = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        return query_data_emit(writer, context, &record, query->items, count + 1u, query->errors, producer, state, reason);
    }
    jdv_record_input_t envelope[] = {
        semantic_field("schema", semantic_symbol(values, "jash.grep.query.v1")),
        semantic_field("items", semantic_list(values, query->items + first, count)),
        semantic_field("summary", summary)
    };
    jdv_value_t document = semantic_record(values, envelope, 3);
    if (values->error) return 125;
    return value_output_json(writer, context, "grep", &document, &values->limits, QUERY_DATA_MAX_BYTES, producer);
}
static void grep_semantic_free(void) {
    if (g_grep_semantic) { semantic_value_free(&g_grep_semantic->values); free(g_grep_semantic); g_grep_semantic = NULL; }
}
