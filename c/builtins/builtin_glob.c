// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
// jash — builtins/glob — bounded literal-pattern file discovery.

#include "builtin.h"
#include "query_file.h"
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <core/glob.h>
#include <osif/osif_stat.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define JASH_GLOB_DEFAULT_LIMIT 128u
#define JASH_GLOB_MAX_LIMIT 256u
#define JASH_GLOB_MAX_MATCHES 4096u
#define JASH_GLOB_MAX_VISITS 100000u
#define JASH_GLOB_MAX_DEPTH 64u
#define JASH_GLOB_MAX_COMPONENTS 128u
#define JASH_GLOB_PATH_CAP 4096u

typedef struct {
    char** paths;
    size_t count;
    size_t capacity;
    size_t visits;
    long match_steps;
    uint64_t errors;
    int overflow;
    int cancelled;
    jash_state_t* state;
} glob_paths_t;

static int glob_parse_u64(const char* text, uint64_t* out) {
    if (!text || !*text || !out) return -1;
    uint64_t value = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u) return -1;
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

static int glob_path_compare(const void* left, const void* right) {
    const char* const* a = left;
    const char* const* b = right;
    return strcmp(*a, *b);
}

static void glob_paths_free(glob_paths_t* paths) {
    if (!paths) return;
    for (size_t i = 0; i < paths->count; i++) free(paths->paths[i]);
    free(paths->paths);
    paths->paths = NULL;
    paths->count = paths->capacity = paths->visits = 0;
    paths->match_steps = 0;
    paths->errors = 0;
    paths->overflow = 0;
}

static int glob_paths_add(glob_paths_t* paths, const char* path) {
    for (size_t i = 0; i < paths->count; i++)
        if (!strcmp(paths->paths[i], path)) return 0;
    if (paths->count >= JASH_GLOB_MAX_MATCHES) { errno = E2BIG; return -1; }
    if (paths->count == paths->capacity) {
        size_t capacity = paths->capacity ? paths->capacity * 2u : 128u;
        if (capacity > JASH_GLOB_MAX_MATCHES) capacity = JASH_GLOB_MAX_MATCHES;
        char** next = realloc(paths->paths, capacity * sizeof(*next));
        if (!next) return -1;
        paths->paths = next;
        paths->capacity = capacity;
    }
    paths->paths[paths->count] = strdup(path);
    if (!paths->paths[paths->count]) return -1;
    paths->count++;
    return 0;
}

static int glob_join(char* output, size_t capacity,
                     const char* base, const char* component) {
    size_t base_len = strlen(base), component_len = strlen(component);
    int slash = base_len && base[base_len - 1] != '/';
    if (base_len + (size_t)slash + component_len >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(output, base, base_len);
    size_t at = base_len;
    if (slash) output[at++] = '/';
    memcpy(output + at, component, component_len + 1u);
    return 0;
}

static int glob_component_has_meta(const char* component) {
    return jash_glob_has_meta(component) || strstr(component, "@(") ||
           strstr(component, "+(") || strstr(component, "!(") ||
           strstr(component, "?(" ) || strstr(component, "*(");
}

static int glob_path_add_checked(glob_paths_t* paths, const char* path) {
    if (glob_paths_add(paths, path) == 0) return 0;
    paths->overflow = 1;
    return -1;
}

static int glob_entry_match(glob_paths_t* paths, const char* pattern,
                            const char* name) {
    int matched = jash_glob_match_ctx(pattern, name, &paths->match_steps);
    if (paths->match_steps <= 0) {
        paths->overflow = 1;
        errno = E2BIG;
        return -1;
    }
    return matched;
}

static void glob_walk(glob_paths_t* matches, char* const* components,
                      size_t component_count, size_t index,
                      const char* directory, const char* output_path,
                      unsigned depth) {
    if (matches->overflow || matches->cancelled) return;
    if ((matches->cancelled = jash_query_cancel_poll(matches->state))) return;
    if (depth > JASH_GLOB_MAX_DEPTH) {
        matches->overflow = 1; errno = E2BIG; return;
    }
    if (index >= component_count) {
        if (output_path[0] && strcmp(output_path, ".") != 0)
            (void)glob_path_add_checked(matches, output_path);
        return;
    }
    const char* component = components[index];
    int last = index + 1u == component_count;
    int recursive = !strcmp(component, "**");

    if (recursive && !last) {
        /* Recursive wildcard also matches zero directory components. */
        glob_walk(matches, components, component_count, index + 1u,
                  directory, output_path, depth + 1u);
        if (matches->overflow) return;
    }

    if (!recursive && !glob_component_has_meta(component)) {
        char child_dir[JASH_GLOB_PATH_CAP];
        char child_output[JASH_GLOB_PATH_CAP];
        if (glob_join(child_dir, sizeof child_dir, directory, component) < 0 ||
            glob_join(child_output, sizeof child_output, output_path, component) < 0) {
            matches->overflow = 1; return;
        }
        struct stat st;
        if (lstat(child_dir, &st) != 0) {
            if (errno != ENOENT && errno != ENOTDIR) matches->errors++;
            return;
        }
        if (last) { (void)glob_path_add_checked(matches, child_output); return; }
        if (S_ISDIR(st.st_mode))
            glob_walk(matches, components, component_count, index + 1u,
                      child_dir, child_output, depth + 1u);
        return;
    }

    DIR* stream = opendir(directory);
    if (!stream) {
        if (errno != ENOENT && errno != ENOTDIR) matches->errors++;
        return;
    }
    struct dirent* entry;
    for (;;) {
        if (matches->cancelled || (matches->cancelled = jash_query_cancel_poll(matches->state))) break;
        errno = 0;
        entry = readdir(stream);
        if (!entry) {
            if (errno) matches->errors++;
            break;
        }
        if (++matches->visits > JASH_GLOB_MAX_VISITS) {
            matches->overflow = 1; errno = E2BIG; break;
        }
        const char* name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (name[0] == '.' && (recursive || component[0] != '.')) continue;
        int matched = recursive ? 1 : glob_entry_match(matches, component, name);
        if (matches->overflow || matches->cancelled) break;
        if (!matched) continue;

        char child_dir[JASH_GLOB_PATH_CAP];
        char child_output[JASH_GLOB_PATH_CAP];
        if (glob_join(child_dir, sizeof child_dir, directory, name) < 0 ||
            glob_join(child_output, sizeof child_output, output_path, name) < 0) {
            matches->overflow = 1; break;
        }
        struct stat st;
        if (lstat(child_dir, &st) != 0) {
            if (errno != ENOENT && errno != ENOTDIR) matches->errors++;
            continue;
        }

        if (last || (recursive && last))
            (void)glob_path_add_checked(matches, child_output);
        if (matches->overflow) break;
        if (S_ISDIR(st.st_mode) && (!recursive || !S_ISLNK(st.st_mode))) {
            if (recursive)
                glob_walk(matches, components, component_count, index,
                          child_dir, child_output, depth + 1u);
            else if (!last)
                glob_walk(matches, components, component_count, index + 1u,
                          child_dir, child_output, depth + 1u);
        }
        if (matches->overflow) break;
    }
    if (closedir(stream)) matches->errors++;
}

/* Bounded path-component walker: no external `find`, no process shell and no
 * glob(3) allocation of an unbounded result vector. */
static int glob_collect(const char* pattern, glob_paths_t* matches) {
    size_t pattern_len = strlen(pattern);
    if (!pattern_len || pattern_len >= JASH_GLOB_PATH_CAP) { errno = EINVAL; return -1; }
    char* copy = strdup(pattern);
    if (!copy) { errno = ENOMEM; return -1; }
    char* components[JASH_GLOB_MAX_COMPONENTS];
    size_t component_count = 0;
    char* cursor = copy;
    int absolute = cursor[0] == '/';
    while (*cursor == '/') cursor++;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        if (component_count == JASH_GLOB_MAX_COMPONENTS) {
            free(copy); errno = E2BIG; return -1;
        }
        components[component_count++] = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor) *cursor++ = '\0';
    }
    char directory[JASH_GLOB_PATH_CAP];
    char output_path[JASH_GLOB_PATH_CAP];
    strlcpy(directory, absolute ? "/" : ".", sizeof directory);
    strlcpy(output_path, absolute ? "/" : "", sizeof output_path);
    matches->match_steps = JASH_GLOB_LINE_STEPS;
    if (component_count) {
        glob_walk(matches, components, component_count, 0,
                  directory, output_path, 0);
    } else if (absolute) {
        (void)glob_path_add_checked(matches, "/");
    }
    int overflow = matches->overflow;
    free(copy);
    if (matches->count > 1)
        qsort(matches->paths, matches->count, sizeof(matches->paths[0]), glob_path_compare);
    if (overflow) {
        if (!errno) errno = E2BIG;
        return -1;
    }
    return 0;
}

static const char* glob_type(mode_t mode) {
    if (S_ISREG(mode)) return "file";
    if (S_ISDIR(mode)) return "dir";
    if (S_ISLNK(mode)) return "symlink";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    if (S_ISBLK(mode)) return "block";
    if (S_ISCHR(mode)) return "char";
    return "other";
}

typedef struct { struct stat metadata; int error; } glob_observation_t;

static glob_observation_t glob_observe(const char* path) {
    glob_observation_t result = {0};
    if (lstat(path, &result.metadata)) result.error = errno;
    return result;
}

static int glob_data_emit(jash_writer_t writer, void* context,
                           glob_paths_t* matches, uint64_t offset, uint64_t limit) {
    size_t count = offset < matches->count ? matches->count - (size_t)offset : 0;
    if ((uint64_t)count > limit) count = (size_t)limit;
    jdv_record_field_t (*fields)[QF_FIELDS] = count ? calloc(count, sizeof(*fields)) : NULL;
    jdv_value_t* items = count ? calloc(count, sizeof(*items)) : NULL;
    if (count && (!fields || !items)) {
        free(fields); free(items);
        return query_data_error(writer, context, &QUERY_FILE_SCHEMA, "allocation_failed", 1);
    }
    uint64_t errors = matches->errors;
    size_t observed = 0;
    for (; observed < count && !matches->cancelled; observed++) {
        if ((matches->cancelled = jash_query_cancel_poll(matches->state))) break;
        const char* path = matches->paths[(size_t)offset + observed];
        glob_observation_t observation = glob_observe(path);
        items[observed] = query_file_item(fields[observed], path,
            observation.error ? NULL : &observation.metadata,
            observation.error ? NULL : glob_type(observation.metadata.st_mode),
            1, offset + observed, observation.error ? "stat_failed" : NULL, observation.error);
        if (fields[observed][QF_ERROR].value.kind != JDV_NOTHING) errors++;
    }
    int incomplete = matches->overflow || offset + observed < matches->count;
    int status = matches->cancelled ? matches->cancelled : matches->overflow ? 75 : errors ? 1 : 0;
    int result = query_data_emit(writer, context, &QUERY_FILE_SCHEMA, items, observed, errors, status,
        matches->cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        matches->cancelled ? "request_cancelled" : matches->overflow ? "traversal_limit" : incomplete ? "page_limit" : NULL);
    free(fields); free(items);
    return result;
}

static int glob_emit_json(jash_writer_t writer, void* context,
                          const char* pattern, char* const* paths, size_t count,
                          uint64_t scan_errors, uint64_t offset,
                          uint64_t limit) {
    uint64_t total = (uint64_t)count;
    uint64_t end = offset + limit;
    if (end < offset || end > total) end = total;
    uint64_t errors = scan_errors;

    /* A JSON page must remain one document even under the prose governor. */
    jash_structured_output_t output;
    if (jash_structured_begin(&output, writer, context)) return 125;
    jash_json_writer_t* json = &output.json;
    jash_jsonw_raw(json, "{\"schema\":\"jash.glob.v1\",\"pattern\":");
    jash_jsonw_string_or_bytes(json, pattern);
    jash_jsonw_raw(json, ",\"consistency\":\"live\",\"total_matches\":");
    jash_jsonw_safe_u64(json, total);
    jash_jsonw_raw(json, ",\"offset\":"); jash_jsonw_safe_u64(json, offset);
    jash_jsonw_raw(json, ",\"limit\":"); jash_jsonw_safe_u64(json, limit);
    jash_jsonw_raw(json, ",\"returned\":"); jash_jsonw_safe_u64(json, end - offset);
    jash_jsonw_raw(json, ",\"matches\":[");
    for (uint64_t i = offset; i < end; i++) {
        if (i != offset) jash_jsonw_raw(json, ",");
        const char* path = paths[i];
        glob_observation_t observation = glob_observe(path);
        struct stat st = observation.metadata;
        if (observation.error) {
            errors++;
            jash_jsonw_raw(json, "{\"path\":");
            jash_jsonw_string_or_bytes(json, path);
            jash_jsonw_raw(json, ",\"type\":null,\"size_bytes\":null,\"error\":\"stat_failed\",\"errno\":");
            jash_jsonw_i64(json, observation.error);
            jash_jsonw_raw(json, "}");
            continue;
        }
        jash_jsonw_raw(json, "{\"path\":");
        jash_jsonw_string_or_bytes(json, path);
        jash_jsonw_raw(json, ",\"type\":"); jash_jsonw_string(json, glob_type(st.st_mode));
        jash_jsonw_raw(json, ",\"size_bytes\":");
        if (st.st_size >= 0) jash_jsonw_safe_u64(json, (uint64_t)st.st_size);
        else jash_jsonw_raw(json, "null");
        jash_jsonw_raw(json, ",\"mtime\":"); jash_jsonw_safe_i64(json, st.st_mtime);
#ifdef __APPLE__
        jash_jsonw_raw(json, ",\"mtime_nsec\":"); jash_jsonw_i64(json, st.st_mtimespec.tv_nsec);
#else
        jash_jsonw_raw(json, ",\"mtime_nsec\":"); jash_jsonw_i64(json, st.st_mtim.tv_nsec);
#endif
        jash_jsonw_raw(json, "}");
    }
    jash_jsonw_raw(json, "],\"errors\":"); jash_jsonw_safe_u64(json, errors);
    jash_jsonw_raw(json, ",\"next_offset\":");
    if (end < total) jash_jsonw_safe_u64(json, end);
    else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"complete\":");
    jash_jsonw_raw(json, end == total && errors == 0 ? "true" : "false");
    jash_jsonw_raw(json, "}\n");
    return jash_structured_finish(&output, errors ? 1 : 0);
}

static void glob_usage(jash_writer_t writer, void* context) {
    jash_writes(writer, context,
        "usage: glob [--json|--data|--data-v2] [--offset N] [--limit N] [--] PATTERN\n"
        "Expand one quoted pathname pattern without invoking a shell program.\n");
}

int jash_b_glob(jash_state_t* state, jash_writer_t writer, void* context,
                int argc, char** argv) {
    int json = 0, data = 0, end_options = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) data = 1;
        else if (!strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
    }
#define GLOB_ERROR(code, status) (data ? query_data_error(writer, context, &QUERY_FILE_SCHEMA, (code), (status)) : \
    json ? jash_structured_error(writer, context, "glob", (code), (status)) : (status))
    if (json && data) return GLOB_ERROR("incompatible_output_modes", 2);
    uint64_t offset = 0, limit = JASH_GLOB_DEFAULT_LIMIT;
    const char* pattern = NULL;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!end_options && !strcmp(arg, "--")) { end_options = 1; continue; }
        if (!end_options && !strcmp(arg, "--help")) { glob_usage(writer, context); return 0; }
        if (!end_options && !strcmp(arg, "--json")) { json = 1; continue; }
        if (!end_options && (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2"))) continue;
        if (!end_options && (!strcmp(arg, "--offset") || !strcmp(arg, "--limit"))) {
            int is_offset = !strcmp(arg, "--offset");
            uint64_t value;
            if (i + 1 >= argc || glob_parse_u64(argv[i + 1], &value) < 0) {
                jash_writef(jash_stderr_writer, NULL,
                            "glob: %s requires a nonnegative decimal\n", arg);
                return GLOB_ERROR("invalid_page", 2);
            }
            i++;
            if (is_offset) offset = value; else limit = value;
            continue;
        }
        if (!end_options && (!strncmp(arg, "--offset=", 9) ||
                             !strncmp(arg, "--limit=", 8))) {
            int is_offset = !strncmp(arg, "--offset=", 9);
            uint64_t value;
            if (glob_parse_u64(arg + (is_offset ? 9 : 8), &value) < 0) {
                jash_writes(jash_stderr_writer, NULL, "glob: invalid pagination value\n");
                return GLOB_ERROR("invalid_page", 2);
            }
            if (is_offset) offset = value; else limit = value;
            continue;
        }
        if (!end_options && arg[0] == '-' && arg[1]) {
            jash_writef(jash_stderr_writer, NULL, "glob: unsupported option '%s'\n", arg);
            return GLOB_ERROR("unsupported_option", 2);
        }
        if (pattern) {
            jash_writes(jash_stderr_writer, NULL, "glob: expected exactly one pattern\n");
            return GLOB_ERROR("expected_one_pattern", 2);
        }
        pattern = arg;
    }
    if (!pattern) {
        if (!data && !json) glob_usage(writer, context);
        return GLOB_ERROR("missing_pattern", 2);
    }
    if (limit < 1 || limit > JASH_GLOB_MAX_LIMIT) {
        jash_writes(jash_stderr_writer, NULL, "glob: --limit must be 1..256\n");
        return GLOB_ERROR("invalid_page", 2);
    }
    if (strlen(pattern) > 4096) {
        jash_writes(jash_stderr_writer, NULL, "glob: pattern exceeds 4096 bytes\n");
        return GLOB_ERROR("pattern_limit", 2);
    }

    glob_paths_t matches = {.state = state};
    int result = glob_collect(pattern, &matches);
    if (result < 0) {
        int saved = errno;
        if (data && matches.overflow) {
            int rc = glob_data_emit(writer, context, &matches, offset, limit);
            glob_paths_free(&matches);
            return rc;
        }
        glob_paths_free(&matches);
        if (saved == E2BIG)
            jash_writes(jash_stderr_writer, NULL,
                        "glob: traversal budget/depth or 4096-match limit exceeded\n");
        else
            jash_writef(jash_stderr_writer, NULL,
                        "glob: expansion failed for pattern (%s)\n", strerror(saved));
        return GLOB_ERROR(saved == E2BIG ? "traversal_limit" : "expansion_failed", saved == ENOMEM ? 1 : 2);
    }
    if (offset > (uint64_t)matches.count) {
        glob_paths_free(&matches);
        jash_writes(jash_stderr_writer, NULL, "glob: offset exceeds match count\n");
        return GLOB_ERROR("offset_exceeds_matches", 2);
    }
    if (!matches.cancelled) matches.cancelled = jash_query_cancel_poll(state);
    int rc;
    if (data) {
        rc = glob_data_emit(writer, context, &matches, offset, limit);
    } else if (matches.cancelled) {
        rc = GLOB_ERROR("request_cancelled", matches.cancelled);
    } else if (json) {
        rc = glob_emit_json(writer, context, pattern, matches.paths, matches.count,
                            matches.errors, offset, limit);
    } else {
        uint64_t end = offset + limit;
        if (end < offset || end > (uint64_t)matches.count) end = (uint64_t)matches.count;
        for (uint64_t i = offset; i < end; i++) {
            const char* path = matches.paths[i];
            size_t length = strlen(path);
            if (writer(context, path, length) != (long)length ||
                writer(context, "\n", 1) != 1) { rc = 125; glob_paths_free(&matches); return rc; }
        }
        rc = matches.errors ? 1 : 0;
    }
    glob_paths_free(&matches);
#undef GLOB_ERROR
    return rc;
}
