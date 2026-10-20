// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE   // FNM_CASEFOLD (-iname/-ipath) is __USE_GNU-gated in
                      // glibc's <fnmatch.h>; Darwin exposes it unconditionally.
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/find — recursive file search, in-process (ADR-0044).
//
// find is a top context-flooder (`find .` in a big tree dumps thousands of
// paths). In-process it is budget-GOVERNED by the single-command dispatch
// (eval.c / main.c): when stdout is a non-tty sink and JASH_LLM_BUDGET is set,
// the listing is capped and spilled to ~/.jash/overflow with a recall footer.
//
// SURFACE SAFETY: find has a huge predicate language. This builtin handles the
// common ones agents use daily — paths, -maxdepth/-mindepth, -type, -name/-iname,
// -path/-ipath, -empty, -not/!, -print/-print0 (AND-combined). ANY token it does
// not understand (-exec, -mtime, -size, -newer, -delete, -regex, -o, (, ) …)
// makes it FALL BACK to the external `find` with the original arguments — so no
// command is ever broken, and the fast/governed path serves the 95% case.

#include "builtin.h"
#include "query_file.h"
#include <parser/exec.h>
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <osif/osif_stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>

enum { FP_NAME, FP_INAME, FP_PATH, FP_IPATH, FP_TYPE, FP_EMPTY };
enum { FIND_JSON_DEFAULT_DEPTH = 16, FIND_JSON_MAX_DEPTH = 64,
       FIND_JSON_MAX_ITEMS = 4096, FIND_JSON_MAX_VISITS = 100000,
       FIND_JSON_MAX_ERRORS = 128, FIND_JSON_DEFAULT_LIMIT = 128,
       FIND_JSON_MAX_LIMIT = 256 };

typedef struct {
    int  kind;
    int  negate;
    char glob[512];
    char type;      /* FP_TYPE: f d l b c p s */
} find_pred_t;

typedef struct {
    char* path;
    struct stat metadata;
    char type;
} find_item_t;

typedef struct {
    char* path;
    int error;
} find_error_t;

typedef struct {
    find_pred_t   preds[64];
    int           npreds;
    int           maxdepth;
    int           mindepth;
    int           print0;
    int           json_mode;
    uint64_t      json_offset;
    uint64_t      json_limit;
    size_t        visited;
    int           truncated;
    int           cancelled;
    jash_state_t* state;
    uint64_t      errors_total;
    int           errors_truncated;
    find_item_t*  items;
    size_t        item_count;
    size_t        item_capacity;
    find_error_t  errors[FIND_JSON_MAX_ERRORS];
    size_t        error_count;
    jash_writer_t w;
    void*         ctx;
} find_ctx_t;

static char find_typechar(mode_t m) {
    if (S_ISREG(m))  return 'f';
    if (S_ISDIR(m))  return 'd';
    if (S_ISLNK(m))  return 'l';
    if (S_ISBLK(m))  return 'b';
    if (S_ISCHR(m))  return 'c';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    return '?';
}

static int find_is_empty(const char* path, const struct stat* st) {
    if (S_ISREG(st->st_mode)) return st->st_size == 0;
    if (S_ISDIR(st->st_mode)) {
        DIR* d = opendir(path);
        if (!d) return 0;
        int empty = 1;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            empty = 0; break;
        }
        closedir(d);
        return empty;
    }
    return 0;
}

static int find_matches(const find_ctx_t* fc, const char* path, const char* base,
                        const struct stat* st, char type) {
    for (int i = 0; i < fc->npreds; i++) {
        const find_pred_t* p = &fc->preds[i];
        int m = 0;
        switch (p->kind) {
            case FP_NAME:  m = fnmatch(p->glob, base, 0) == 0; break;
            case FP_INAME: m = fnmatch(p->glob, base, FNM_CASEFOLD) == 0; break;
            case FP_PATH:  m = fnmatch(p->glob, path, 0) == 0; break;   /* '*' spans '/' */
            case FP_IPATH: m = fnmatch(p->glob, path, FNM_CASEFOLD) == 0; break;
            case FP_TYPE:  m = (type == p->type); break;
            case FP_EMPTY: m = find_is_empty(path, st); break;
        }
        if (p->negate) m = !m;
        if (!m) return 0;
    }
    return 1;
}

static int find_item_add(find_ctx_t* fc, const char* path,
                         const struct stat* st, char type) {
    if (fc->item_count >= FIND_JSON_MAX_ITEMS) {
        fc->truncated = 1;
        return -1;
    }
    if (fc->item_count == fc->item_capacity) {
        size_t capacity = fc->item_capacity ? fc->item_capacity * 2u : 64u;
        if (capacity > FIND_JSON_MAX_ITEMS) capacity = FIND_JSON_MAX_ITEMS;
        find_item_t* next = realloc(fc->items, capacity * sizeof(*next));
        if (!next) { fc->truncated = 1; return -1; }
        fc->items = next;
        fc->item_capacity = capacity;
    }
    find_item_t* item = &fc->items[fc->item_count];
    item->path = strdup(path);
    if (!item->path) { fc->truncated = 1; return -1; }
    item->metadata = *st;
    item->type = type;
    fc->item_count++;
    return 0;
}

static void find_error_add(find_ctx_t* fc, const char* path, int error) {
    fc->errors_total++;
    if (fc->error_count >= FIND_JSON_MAX_ERRORS) {
        fc->errors_truncated = 1;
        return;
    }
    find_error_t* record = &fc->errors[fc->error_count];
    record->path = strdup(path);
    if (!record->path) { fc->errors_truncated = 1; return; }
    record->error = error;
    fc->error_count++;
}

static void find_emit(find_ctx_t* fc, const char* path,
                     const struct stat* st, char type) {
    if (fc->json_mode) {
        (void)find_item_add(fc, path, st, type);
        return;
    }
    fc->w(fc->ctx, path, strlen(path));
    fc->w(fc->ctx, fc->print0 ? "\0" : "\n", 1);
}

/* Pre-order walk (print then descend, like find). Does not follow symlinks. */
static void find_walk(find_ctx_t* fc, const char* path, int depth) {
    if (fc->json_mode && (fc->truncated || fc->cancelled)) return;
    if (fc->json_mode && (fc->cancelled = jash_query_cancel_poll(fc->state))) return;
    if (fc->json_mode && ++fc->visited > FIND_JSON_MAX_VISITS) {
        fc->truncated = 1;
        return;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (fc->json_mode) find_error_add(fc, path, errno);
        else jash_writef(jash_stderr_writer, 0, "find: '%s': %s\n", path, strerror(errno));
        return;
    }
    char type = find_typechar(st.st_mode);
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;

    if (depth >= fc->mindepth && find_matches(fc, path, base, &st, type))
        find_emit(fc, path, &st, type);

    if (fc->json_mode && fc->truncated) return;
    if (S_ISDIR(st.st_mode) && depth < fc->maxdepth) {
        DIR* d = opendir(path);
        if (!d) {
            if (fc->json_mode) find_error_add(fc, path, errno);
            return;
        }
        struct dirent* e;
        int root_slash = (path[0] == '/' && path[1] == '\0');
        for (;;) {
            if (fc->json_mode && (fc->cancelled = jash_query_cancel_poll(fc->state))) break;
            errno = 0;
            e = readdir(d);
            if (!e) {
                if (errno && fc->json_mode) find_error_add(fc, path, errno);
                break;
            }
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            char child[PATH_MAX];
            int n = snprintf(child, sizeof child, "%s%s%s",
                             path, root_slash ? "" : "/", e->d_name);
            if (n < 0 || (size_t)n >= sizeof child) {
                if (fc->json_mode) find_error_add(fc, path, ENAMETOOLONG);
                else jash_writef(jash_stderr_writer, NULL,
                                 "find: child path exceeds PATH_MAX under '%s'\n", path);
                continue;
            }
            find_walk(fc, child, depth + 1);
            if (fc->json_mode && (fc->truncated || fc->cancelled)) break;
        }
        if (closedir(d) && fc->json_mode) find_error_add(fc, path, errno);
    }
}

static int find_item_compare(const void* left, const void* right) {
    const find_item_t* a = left;
    const find_item_t* b = right;
    return strcmp(a->path, b->path);
}

static const char* find_type_name(char type) {
    switch (type) {
        case 'f': return "file";
        case 'd': return "dir";
        case 'l': return "symlink";
        case 'b': return "block";
        case 'c': return "char";
        case 'p': return "fifo";
        case 's': return "socket";
        default: return "unknown";
    }
}

static void find_context_free(find_ctx_t* fc) {
    for (size_t i = 0; i < fc->item_count; i++) free(fc->items[i].path);
    free(fc->items);
    fc->items = NULL;
    for (size_t i = 0; i < fc->error_count; i++) free(fc->errors[i].path);
    fc->item_count = fc->item_capacity = fc->error_count = 0;
}

static int find_data_emit(find_ctx_t* fc) {
    if (fc->item_count > 1)
        qsort(fc->items, fc->item_count, sizeof(fc->items[0]), find_item_compare);
    if (fc->json_offset > fc->item_count && !fc->cancelled && !fc->truncated)
        return query_data_error(fc->w, fc->ctx, &QUERY_FILE_SCHEMA, "offset_exceeds_matches", 2);
    size_t count = fc->json_offset < fc->item_count ? fc->item_count - (size_t)fc->json_offset : 0;
    if ((uint64_t)count > fc->json_limit) count = (size_t)fc->json_limit;
    size_t capacity = count + fc->error_count;
    jdv_record_field_t (*fields)[QF_FIELDS] = capacity ? calloc(capacity, sizeof(*fields)) : NULL;
    jdv_value_t* items = capacity ? calloc(capacity, sizeof(*items)) : NULL;
    if (capacity && (!fields || !items)) {
        free(fields); free(items);
        return query_data_error(fc->w, fc->ctx, &QUERY_FILE_SCHEMA, "allocation_failed", 1);
    }
    uint64_t errors = fc->errors_total;
    size_t observed = 0;
    for (; observed < count; observed++) {
        const find_item_t* item = &fc->items[(size_t)fc->json_offset + observed];
        items[observed] = query_file_item(fields[observed], item->path, &item->metadata,
            find_type_name(item->type), 1, fc->json_offset + observed, NULL, 0);
        if (fields[observed][QF_ERROR].value.kind != JDV_NOTHING) errors++;
    }
    for (size_t i = 0; i < fc->error_count; i++, observed++)
        items[observed] = query_file_item(fields[observed], fc->errors[i].path, NULL,
            NULL, 0, 0, "observation_failed", fc->errors[i].error);
    int capped = fc->truncated || fc->errors_truncated;
    int incomplete = capped || fc->json_offset + count < fc->item_count;
    int status = fc->cancelled ? fc->cancelled : capped ? 75 : errors ? 1 : 0;
    int result = query_data_emit(fc->w, fc->ctx, &QUERY_FILE_SCHEMA, items, observed,
        errors, status, fc->cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        fc->cancelled ? "request_cancelled" : capped ? "traversal_limit" : incomplete ? "page_limit" : NULL);
    free(fields); free(items);
    return result;
}

static int find_json_emit(find_ctx_t* fc) {
    if (!fc->cancelled) fc->cancelled = jash_query_cancel_poll(fc->state);
    if (fc->item_count > 1)
        qsort(fc->items, fc->item_count, sizeof(fc->items[0]), find_item_compare);
    if (fc->json_offset > fc->item_count) {
        jash_writes(jash_stderr_writer, NULL, "find: --offset exceeds observed match count\n");
        return 2;
    }
    uint64_t end = fc->json_offset + fc->json_limit;
    if (end < fc->json_offset || end > fc->item_count) end = fc->item_count;
    int complete = !fc->truncated && !fc->cancelled && fc->errors_total == 0;
    /* Source/page bounds apply independently of the presentation budget. */
    jash_structured_output_t output;
    if (jash_structured_begin(&output, fc->w, fc->ctx)) return 125;
    jash_json_writer_t* json = &output.json;
    jash_jsonw_raw(json, "{\"schema\":\"jash.query.find.v1\",\"consistency\":\"live\",\"maxdepth\":");
    jash_jsonw_i64(json, fc->maxdepth);
    jash_jsonw_raw(json, ",\"offset\":"); jash_jsonw_safe_u64(json, fc->json_offset);
    jash_jsonw_raw(json, ",\"limit\":"); jash_jsonw_safe_u64(json, fc->json_limit);
    jash_jsonw_raw(json, ",\"count\":"); jash_jsonw_safe_u64(json, end - fc->json_offset);
    jash_jsonw_raw(json, ",\"matches_seen\":"); jash_jsonw_safe_u64(json, fc->item_count);
    jash_jsonw_raw(json, ",\"total_matches\":");
    if (complete) jash_jsonw_safe_u64(json, fc->item_count);
    else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"next_offset\":");
    if (end < fc->item_count) jash_jsonw_safe_u64(json, end);
    else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"page_complete\":");
    jash_jsonw_raw(json, end == fc->item_count ? "true" : "false");
    jash_jsonw_raw(json, ",\"complete\":");
    jash_jsonw_raw(json, complete ? "true" : "false");
    jash_jsonw_raw(json, ",\"truncated\":");
    jash_jsonw_raw(json, fc->truncated ? "true" : "false");
    jash_jsonw_raw(json, ",\"items\":[");
    for (uint64_t i = fc->json_offset; i < end; i++) {
        if (i != fc->json_offset) jash_jsonw_raw(json, ",");
        const find_item_t* item = &fc->items[i];
        jash_jsonw_raw(json, "{\"path\":"); jash_jsonw_string_or_bytes(json, item->path);
        jash_jsonw_raw(json, ",\"type\":"); jash_jsonw_string(json, find_type_name(item->type));
        jash_jsonw_raw(json, ",\"size_bytes\":");
        if (item->metadata.st_size >= 0)
            jash_jsonw_safe_u64(json, (uint64_t)item->metadata.st_size);
        else jash_jsonw_raw(json, "null");
        jash_jsonw_raw(json, ",\"mtime\":"); jash_jsonw_safe_i64(json, item->metadata.st_mtime);
#ifdef __APPLE__
        jash_jsonw_raw(json, ",\"mtime_nsec\":"); jash_jsonw_i64(json, item->metadata.st_mtimespec.tv_nsec);
#else
        jash_jsonw_raw(json, ",\"mtime_nsec\":"); jash_jsonw_i64(json, item->metadata.st_mtim.tv_nsec);
#endif
        jash_jsonw_raw(json, "}");
    }
    jash_jsonw_raw(json, "],\"errors\":[");
    for (size_t i = 0; i < fc->error_count; i++) {
        if (i) jash_jsonw_raw(json, ",");
        jash_jsonw_raw(json, "{\"path\":"); jash_jsonw_string_or_bytes(json, fc->errors[i].path);
        jash_jsonw_raw(json, ",\"errno\":"); jash_jsonw_i64(json, fc->errors[i].error);
        jash_jsonw_raw(json, "}");
    }
    jash_jsonw_raw(json, "],\"errors_total\":"); jash_jsonw_safe_u64(json, fc->errors_total);
    jash_jsonw_raw(json, ",\"errors_truncated\":");
    jash_jsonw_raw(json, fc->errors_truncated ? "true" : "false");
    jash_jsonw_raw(json, "}\n");
    return jash_structured_finish(&output, fc->cancelled ? fc->cancelled : complete ? 0 : 1);
}

static int find_parse_u64(const char* text, uint64_t* out) {
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

static int find_parse_depth(const char* text, int* out) {
    uint64_t value;
    if (find_parse_u64(text, &value) < 0 || value > FIND_JSON_MAX_DEPTH) return -1;
    *out = (int)value;
    return 0;
}

/* Rebuild argv into a single-quoted args string and defer to external find. */
static int find_fallback(jash_state_t* s, jash_writer_t w, void* ctx,
                         int argc, char** argv) {
    char args[8192];
    size_t o = 0;
    for (int i = 0; i < argc && o + 4 < sizeof args; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p && o + 5 < sizeof args; p++) {
            if (*p == '\'') { args[o++]='\''; args[o++]='\\'; args[o++]='\''; args[o++]='\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    return jash_exec_external(s, w, ctx, "find", args);
}

int jash_b_find(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    find_ctx_t fc;
    memset(&fc, 0, sizeof fc);
    fc.state = s;
    fc.json_mode = argc > 0 && !strcmp(argv[0], "--json") ? 1 :
        argc > 0 && (!strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2")) ? 2 : 0;
    fc.maxdepth = fc.json_mode ? FIND_JSON_DEFAULT_DEPTH : INT_MAX;
    fc.mindepth = 0;
    fc.json_limit = FIND_JSON_DEFAULT_LIMIT;
    fc.w = w; fc.ctx = ctx;
#define FIND_ERROR(code, status) (fc.json_mode == 2 ? query_data_error(w, ctx, &QUERY_FILE_SCHEMA, (code), (status)) : \
    fc.json_mode ? jash_structured_error(w, ctx, "find", (code), (status)) : (status))

    char** filtered = NULL;
    char** args = argv;
    int args_count = argc;
    if (fc.json_mode) {
        filtered = malloc((size_t)argc * sizeof(*filtered));
        if (!filtered) return FIND_ERROR("allocation_failed", 1);
        int i = 1, offset_seen = 0, limit_seen = 0;
        while (i < argc) {
            const char* arg = argv[i];
            int is_offset = !strcmp(arg, "--offset") || !strncmp(arg, "--offset=", 9);
            int is_limit = !strcmp(arg, "--limit") || !strncmp(arg, "--limit=", 8);
            if (!is_offset && !is_limit) break;
            if ((is_offset && offset_seen) || (is_limit && limit_seen)) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: duplicate pagination option\n");
                return FIND_ERROR("duplicate_page_option", 2);
            }
            uint64_t value;
            if (!strcmp(arg, "--offset") || !strcmp(arg, "--limit")) {
                if (i + 1 >= argc || find_parse_u64(argv[i + 1], &value) < 0) {
                    free(filtered);
                    jash_writef(jash_stderr_writer, NULL,
                                "find: %s requires a nonnegative decimal\n", arg);
                    return FIND_ERROR("invalid_page", 2);
                }
                i += 2;
            } else {
                size_t prefix = is_offset ? 9u : 8u;
                if (find_parse_u64(arg + prefix, &value) < 0) {
                    free(filtered);
                    jash_writes(jash_stderr_writer, NULL, "find: invalid pagination value\n");
                    return FIND_ERROR("invalid_page", 2);
                }
                i++;
            }
            if (is_offset) { fc.json_offset = value; offset_seen = 1; }
            else { fc.json_limit = value; limit_seen = 1; }
        }
        if (fc.json_limit < 1 || fc.json_limit > FIND_JSON_MAX_LIMIT) {
            free(filtered);
            jash_writes(jash_stderr_writer, NULL, "find: --limit must be 1..256\n");
            return FIND_ERROR("invalid_page", 2);
        }
        args_count = argc - i;
        for (int n = 0; n < args_count; n++) filtered[n] = argv[i + n];
        args = filtered;
    }

    /* Leading path operands: everything before the first predicate/operator. */
    char paths[64][PATH_MAX];
    int npaths = 0;
    int literal_paths = args_count > 0 && !strcmp(args[0], "--");
    int i = literal_paths;
    for (; i < args_count; i++) {
        const char* a = args[i];
        if (!literal_paths && (a[0] == '-' || strcmp(a, "!") == 0 || strcmp(a, "(") == 0 || strcmp(a, ")") == 0))
            break;
        if (strlen(a) >= PATH_MAX || npaths >= 64) {
            if (fc.json_mode) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: path exceeds JSON input bounds\n");
                return FIND_ERROR("path_input_limit", 2);
            }
            if (npaths >= 64) break;
        }
        strlcpy(paths[npaths++], a, PATH_MAX);
    }
    if (npaths == 0) strlcpy(paths[npaths++], ".", PATH_MAX);

    /* Predicates (AND-combined). Unsupported → external fallback. */
    int negate_next = 0;
    for (; i < args_count; i++) {
        char* a = args[i];
        find_pred_t* p = (fc.npreds < 64) ? &fc.preds[fc.npreds] : NULL;
        int added = 0;
        if (strcmp(a, "-maxdepth") == 0 && i + 1 < args_count) {
            if (fc.json_mode) {
                if (find_parse_depth(args[++i], &fc.maxdepth) < 0) {
                    free(filtered);
                    jash_writes(jash_stderr_writer, NULL, "find: JSON maxdepth must be 0..64\n");
                    return FIND_ERROR("invalid_depth", 2);
                }
            } else fc.maxdepth = atoi(args[++i]);
        } else if (strcmp(a, "-mindepth") == 0 && i + 1 < args_count) {
            if (fc.json_mode) {
                if (find_parse_depth(args[++i], &fc.mindepth) < 0) {
                    free(filtered);
                    jash_writes(jash_stderr_writer, NULL, "find: JSON mindepth must be 0..64\n");
                    return FIND_ERROR("invalid_depth", 2);
                }
            } else fc.mindepth = atoi(args[++i]);
        } else if ((strcmp(a, "-name") == 0 || strcmp(a, "-iname") == 0) && i + 1 < args_count && p) {
            p->kind = (a[1] == 'i') ? FP_INAME : FP_NAME;
            if (fc.json_mode && strlen(args[i + 1]) >= sizeof p->glob) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: predicate exceeds JSON input bounds\n");
                return FIND_ERROR("predicate_limit", 2);
            }
            strlcpy(p->glob, args[++i], sizeof p->glob); added = 1;
        } else if ((strcmp(a, "-path") == 0 || strcmp(a, "-ipath") == 0 ||
                    strcmp(a, "-wholename") == 0) && i + 1 < args_count && p) {
            p->kind = (a[1] == 'i') ? FP_IPATH : FP_PATH;
            if (fc.json_mode && strlen(args[i + 1]) >= sizeof p->glob) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: predicate exceeds JSON input bounds\n");
                return FIND_ERROR("predicate_limit", 2);
            }
            strlcpy(p->glob, args[++i], sizeof p->glob); added = 1;
        } else if (strcmp(a, "-type") == 0 && i + 1 < args_count && p) {
            const char* type = args[++i];
            if (fc.json_mode && (strlen(type) != 1 ||
                !strchr("fdlbcps", type[0]))) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: unsupported -type value for JSON\n");
                return FIND_ERROR("unsupported_type", 2);
            }
            p->kind = FP_TYPE; p->type = type[0]; added = 1;
        } else if (strcmp(a, "-empty") == 0 && p) {
            p->kind = FP_EMPTY; added = 1;
        } else if (strcmp(a, "-not") == 0 || strcmp(a, "!") == 0) {
            negate_next = !negate_next; continue;
        } else if (strcmp(a, "-print") == 0) {
            /* default action — no-op */
        } else if (strcmp(a, "-print0") == 0) {
            if (fc.json_mode) {
                free(filtered);
                jash_writes(jash_stderr_writer, NULL, "find: -print0 is incompatible with --json\n");
                return FIND_ERROR("incompatible_output_mode", 2);
            }
            fc.print0 = 1;
        } else if (strcmp(a, "-a") == 0 || strcmp(a, "-and") == 0) {
            /* implicit AND — no-op */
        } else {
            /* -exec, -mtime, -size, -newer, -delete, -regex, -o, (, ) … */
            if (fc.json_mode) {
                jash_writef(jash_stderr_writer, NULL,
                            "find: unsupported predicate in --json: %s\n", a);
                free(filtered);
                return FIND_ERROR("unsupported_predicate", 2);
            }
            return find_fallback(s, w, ctx, argc, argv);
        }
        if (added) { p->negate = negate_next; negate_next = 0; fc.npreds++; }
    }

    if (fc.json_mode && (negate_next || fc.mindepth > fc.maxdepth)) {
        free(filtered);
        jash_writes(jash_stderr_writer, NULL, "find: incomplete or invalid JSON predicate list\n");
        return FIND_ERROR("invalid_predicates", 2);
    }

    for (int k = 0; k < npaths; k++)
        find_walk(&fc, paths[k], 0);
    free(filtered);
    int rc = fc.json_mode == 2 ? find_data_emit(&fc) : fc.json_mode ? find_json_emit(&fc) : 0;
    find_context_free(&fc);
#undef FIND_ERROR
    return rc;
}
