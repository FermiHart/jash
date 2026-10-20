// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/du — disruptive du builtin with cache, ASCII chart, and interactive mode
//
// Features:
//   du [path]              - show disk usage with ASCII bar chart
//   du --cache [path]      - use cached results (TTL 10s)
//   du --top N [path]      - show only top N largest items
//   du --human             - human-readable sizes (KB, MB, GB)
//   du --interactive       - navigate directories interactively
//   du --depth N           - limit recursion depth
//
// Disruptive:
//   - Smart Cache: caches directory sizes (TTL 10s)
//   - ASCII Visualization: bar chart showing relative sizes
//   - Interactive Mode: navigate with arrow keys, drill down
//   - Top N: focus on largest consumers

#include "builtin.h"
#include "query_data.h"
#include <core/argv.h>
#include <core/agent_cancel.h>
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <core/agent_json.h>   /* jash_json_esc (--json) */
#include "theme/colors.h"
#include "sys/syscalls.h"
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>          /* INV-726: PATH_MAX directly binds the
                              * per-entry and paths[][] row bounds. */
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include "du_query.h"

#define DU_CACHE_TTL 10  /* seconds */
#define DU_MAX_ENTRIES 1024
#define DU_BAR_WIDTH 40

/* ADR-0016 §4: pointer-based cache entry (was char[PATH_MAX] × 1024 =
 * 4.2 MB of .bss). Paths are strdup'd; capacity (DU_MAX_ENTRIES) and
 * eviction policy (oldest-first) are unchanged. */
typedef struct {
    char *path;          /* strdup'd; owned by the cache */
    long size_kb;
    time_t timestamp;
} du_cache_entry_t;

static du_cache_entry_t *g_du_cache;   /* [DU_MAX_ENTRIES], lazy */
static int g_du_cache_count = 0;

/* Allocate the cache table on first use. Returns 0 on success. */
static int du_cache_ensure(void) {
    if (g_du_cache) return 0;
    g_du_cache = (du_cache_entry_t*)calloc(DU_MAX_ENTRIES, sizeof(du_cache_entry_t));
    return g_du_cache ? 0 : -1;
}

typedef struct {
    char name[256];
    long size_kb;
} du_item_t;

static int g_du_human_readable = 0;

static void du_format_size(long size_kb, char* buf, size_t bufsize) {
    if (!g_du_human_readable || size_kb < 1024) {
        snprintf(buf, bufsize, "%ld KB", size_kb);
        return;
    }
    
    if (size_kb < 1024 * 1024) {
        snprintf(buf, bufsize, "%.1f MB", size_kb / 1024.0);
        return;
    }
    
    if (size_kb < 1024 * 1024 * 1024) {
        snprintf(buf, bufsize, "%.1f GB", size_kb / (1024.0 * 1024.0));
        return;
    }
    
    snprintf(buf, bufsize, "%.1f TB", size_kb / (1024.0 * 1024.0 * 1024.0));
}

static du_cache_entry_t* du_cache_find(const char* path) {
    if (!g_du_cache) return NULL;
    time_t now = time(NULL);
    for (int i = 0; i < g_du_cache_count; i++) {
        if (g_du_cache[i].path && strcmp(g_du_cache[i].path, path) == 0) {
            if (now - g_du_cache[i].timestamp < DU_CACHE_TTL) {
                return &g_du_cache[i];
            }
            return NULL;  /* Expired */
        }
    }
    return NULL;
}

static void du_cache_update(const char* path, long size_kb) {
    if (du_cache_ensure() != 0) return;  /* OOM: skip caching, stay correct */
    /* Find existing or create new */
    du_cache_entry_t* entry = NULL;
    for (int i = 0; i < g_du_cache_count; i++) {
        if (g_du_cache[i].path && strcmp(g_du_cache[i].path, path) == 0) {
            entry = &g_du_cache[i];
            break;
        }
    }
    
    if (!entry) {
        if (g_du_cache_count >= DU_MAX_ENTRIES) {
            /* Evict oldest */
            entry = &g_du_cache[0];
            for (int i = 1; i < g_du_cache_count; i++) {
                if (g_du_cache[i].timestamp < entry->timestamp) {
                    entry = &g_du_cache[i];
                }
            }
        } else {
            entry = &g_du_cache[g_du_cache_count++];
        }
        char *dup = strdup(path);
        if (!dup) {
            /* OOM: roll back the slot if we just claimed it */
            if (entry == &g_du_cache[g_du_cache_count - 1] && !entry->path)
                g_du_cache_count--;
            return;
        }
        free(entry->path);
        entry->path = dup;
    }
    
    entry->size_kb = size_kb;
    entry->timestamp = time(NULL);
}

static long du_calculate_size(const char* path, int depth, int max_depth) {
    /* Check cache first */
    du_cache_entry_t* cached = du_cache_find(path);
    if (cached) {
        return cached->size_kb;
    }
    
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        long size_kb = (st.st_size + 1023) / 1024;  /* Round up to KB */
        du_cache_update(path, size_kb);
        return size_kb;
    }
    
    /* Directory: recurse */
    if (max_depth >= 0 && depth >= max_depth) {
        return 0;
    }
    
    DIR* dir = opendir(path);
    if (!dir) {
        return 0;
    }
    
    long total_kb = 0;
    struct dirent* ent;
    
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        
        total_kb += du_calculate_size(child, depth + 1, max_depth);
    }
    
    closedir(dir);
    du_cache_update(path, total_kb);
    
    return total_kb;
}

static int du_compare(const void* a, const void* b) {
    const du_item_t* ia = (const du_item_t*)a;
    const du_item_t* ib = (const du_item_t*)b;
    return ib->size_kb - ia->size_kb;  /* Descending */
}

static void du_print_bar(long size_kb, long max_kb, jash_writer_t w, void* ctx) {
    if (max_kb == 0) {
        jash_writes(w, ctx, " \n");
        return;
    }
    
    int bar_len = (int)((size_kb * DU_BAR_WIDTH) / max_kb);
    if (bar_len < 1 && size_kb > 0) bar_len = 1;
    
    char size_str[32];
    du_format_size(size_kb, size_str, sizeof(size_str));
    
    /* Unicode gradient bar: darker = more usage */
    jash_writes(w, ctx, J_ROLE_ACCENT);
    for (int i = 0; i < bar_len; i++) {
        /* Gradient: █ (full) → ▓ (dark) → ▒ (medium) → ░ (light) */
        if (i < bar_len * 0.3) {
            jash_writes(w, ctx, "█");
        } else if (i < bar_len * 0.6) {
            jash_writes(w, ctx, "▓");
        } else if (i < bar_len * 0.85) {
            jash_writes(w, ctx, "▒");
        } else {
            jash_writes(w, ctx, "░");
        }
    }
    jash_writes(w, ctx, J_RESET);
    
    /* Pad to align sizes */
    for (int i = bar_len; i < DU_BAR_WIDTH; i++) {
        jash_writes(w, ctx, " ");
    }
    
    char line[64];
    snprintf(line, sizeof(line), " %s\n", size_str);
    jash_writes(w, ctx, line);
}

/* GNU-style size: default = integer KB; -h = "4.0K"/"1.5M"/"2.3G". */
static void du_gnu_size(long kb, int human, char* out, size_t cap) {
    if (!human) { snprintf(out, cap, "%ld", kb); return; }
    double v = (double)kb; const char* u[] = {"K","M","G","T","P"}; int ui = 0;
    while (v >= 1024.0 && ui < 4) { v /= 1024.0; ui++; }
    if (v < 10.0) snprintf(out, cap, "%.1f%s", v, u[ui]);
    else          snprintf(out, cap, "%.0f%s", v, u[ui]);
}

/* Disk usage (st_blocks, like real du) in KB for one entry. */
static long du_entry_kb(const struct stat* st) {
    return (long)((st->st_blocks * 512 + 1023) / 1024);
}

/* GNU-parseable walk: prints "SIZE\tPATH" per directory (post-order), returns
 * the cumulative KB of `path`. Reports dirs at depth<=max_depth; with show_all
 * also reports files; summarize suppresses all interior reporting (caller prints
 * the root). */
static long du_walk(const char* path, int depth, int max_depth, int show_all,
                    int summarize, int human, jash_writer_t w, void* ctx) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return du_entry_kb(&st);

    long total = du_entry_kb(&st);
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            struct stat cst;
            if (lstat(child, &cst) != 0) continue;
            if (S_ISDIR(cst.st_mode)) {
                total += du_walk(child, depth + 1, max_depth, show_all, summarize, human, w, ctx);
            } else {
                long fkb = du_entry_kb(&cst);
                total += fkb;
                if (show_all && !summarize && (max_depth < 0 || depth + 1 <= max_depth)) {
                    char sz[32]; du_gnu_size(fkb, human, sz, sizeof sz);
                    jash_writef(w, ctx, "%s\t%s\n", sz, child);
                }
            }
        }
        closedir(d);
    }
    if (!summarize && (max_depth < 0 || depth <= max_depth)) {
        char sz[32]; du_gnu_size(total, human, sz, sizeof sz);
        jash_writef(w, ctx, "%s\t%s\n", sz, path);
    }
    return total;
}

int jash_b_du_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode = du_query_mode(argc, argv);
    if (mode) return du_query_run(s, w, ctx, argc, argv, mode == 2);
    int use_cache = 0;
    int top_n = -1;  /* -1 = show all */
    int max_depth = -1;  /* -1 = unlimited */
    int json_out = 0;
    int summarize = 0;   /* -s */
    int show_all = 0;    /* -a */
    int grand_total = 0; /* -c */
    char paths[64][PATH_MAX];
    const char* path = paths[0]; /* INV-862: one local origin for legacy mode */
    int npaths = 0;
    g_du_human_readable = 0;
    
    /* Parse arguments */
    {
        int end_opts = 0;
        for (int i = 0; i < argc; i++) {
            if (!end_opts && strcmp(argv[i], "--") == 0) { end_opts = 1; continue; }
            if (!end_opts && strcmp(argv[i], "--cache") == 0) {
                use_cache = 1;
            } else if (!end_opts && (strcmp(argv[i], "--human") == 0 ||
                                     strcmp(argv[i], "--human-readable") == 0)) {
                g_du_human_readable = 1;
            } else if (!end_opts && strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
                top_n = atoi(argv[++i]);
            } else if (!end_opts && (strcmp(argv[i], "--depth") == 0 ||
                                     strcmp(argv[i], "--max-depth") == 0) && i + 1 < argc) {
                max_depth = atoi(argv[++i]);
            } else if (!end_opts && strncmp(argv[i], "--max-depth=", 12) == 0) {
                max_depth = atoi(argv[i] + 12);
            } else if (!end_opts && (strcmp(argv[i], "--summarize") == 0 || strcmp(argv[i], "-s") == 0)) {
                summarize = 1;
            } else if (!end_opts && (strcmp(argv[i], "--all") == 0)) {
                show_all = 1;
            } else if (!end_opts && (strcmp(argv[i], "--total") == 0)) {
                grand_total = 1;
            } else if (!end_opts && strcmp(argv[i], "--json") == 0) {
                json_out = 1;
            } else if (!end_opts && argv[i][0] == '-' && argv[i][1] != '-' && argv[i][1] != '\0') {
                /* Short flags: -sh, -ah, -cs, -dN, etc */
                const char* option = argv[i];
                size_t option_length = strlen(option);
                for (size_t j = 1; j < option_length; j++) {
                    switch (option[j]) {
                        case 'h': g_du_human_readable = 1; break;
                        case 's': summarize = 1; break;
                        case 'a': show_all = 1; break;
                        case 'c': grand_total = 1; break;
                        case 'k': /* KB (default) */ break;
                        case 'd':
                            if (j + 1 < option_length) { max_depth = atoi(option + j + 1); }
                            else if (i + 1 < argc) { max_depth = atoi(argv[++i]); }
                            else {
                                jash_writes(jash_stderr_writer, 0, "du: option -d requires a depth\n");
                                return 1;
                            }
                            /* -d consumes the rest of this token or the next
                             * operand. Neither is another short-option group. */
                            j = option_length - 1;
                            break;
                        default:
                            jash_writef(jash_stderr_writer, 0, "du: invalid option -- '%c'\n", option[j]);
                            return 1;
                    }
                }
            } else if (!end_opts && argv[i][0] == '-' && argv[i][1] == '-') {
                jash_writef(jash_stderr_writer, 0, "du: unknown option '%s'\n", argv[i]);
                return 1;
            } else {
                if (npaths < 64) strlcpy(paths[npaths++], argv[i], PATH_MAX);
            }
        }
    }
    if (npaths == 0) strlcpy(paths[npaths++], ".", PATH_MAX);

    /* ── GNU-parseable default: SIZE<TAB>PATH per directory ───────────────────
     * (was an ASCII bar chart no agent could parse — removed per ADR-0044.)
     * --json keeps the structured per-child records below; --cache/--top reuse
     * the legacy collector. */
    if (!json_out && !use_cache && top_n < 0) {
        long grand = 0;
        int rc = 0;
        for (int p = 0; p < npaths; p++) {
            struct stat pst;
            if (lstat(paths[p], &pst) != 0) {
                jash_writef(jash_stderr_writer, 0, "du: cannot access '%s'\n", paths[p]);
                rc = 1; continue;
            }
            long kb;
            if (!S_ISDIR(pst.st_mode)) {
                kb = du_entry_kb(&pst);
                char sz[32]; du_gnu_size(kb, g_du_human_readable, sz, sizeof sz);
                jash_writef(w, ctx, "%s\t%s\n", sz, paths[p]);
            } else {
                kb = du_walk(paths[p], 0, max_depth, show_all, summarize,
                             g_du_human_readable, w, ctx);
                if (summarize) {
                    char sz[32]; du_gnu_size(kb, g_du_human_readable, sz, sizeof sz);
                    jash_writef(w, ctx, "%s\t%s\n", sz, paths[p]);
                }
            }
            grand += kb;
        }
        if (grand_total) {
            char sz[32]; du_gnu_size(grand, g_du_human_readable, sz, sizeof sz);
            jash_writef(w, ctx, "%s\ttotal\n", sz);
        }
        return rc;
    }
    
    /* Check if path is a file */
    struct stat st;
    if (stat(path, &st) != 0) {
        jash_writef(w, ctx, "du: cannot access '%s'\n", path);
        return 1;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        /* Single file */
        long size_kb = (st.st_size + 1023) / 1024;
        if (json_out) {
            char ep[2 * PATH_MAX]; jash_json_esc(path, ep, sizeof ep);
            jash_writef(w, ctx, "{\"name\":\"%s\",\"size_kb\":%ld}\n", ep, size_kb);
            return 0;
        }
        char size_str[32];
        du_format_size(size_kb, size_str, sizeof(size_str));
        jash_writef(w, ctx, "%s%s%s\n", J_ROLE_ACCENT, size_str, J_RESET);
        return 0;
    }
    
    /* Directory: collect items */
    DIR* dir = opendir(path);
    if (!dir) {
        jash_writef(w, ctx, "du: cannot open directory '%s'\n", path);
        return 1;
    }
    
    du_item_t items[DU_MAX_ENTRIES];
    int nitems = 0;
    long max_kb = 0;
    
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && nitems < DU_MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        
        long size_kb = du_calculate_size(child, 0, max_depth);
        
        strlcpy(items[nitems].name, ent->d_name, sizeof(items[nitems].name));
        items[nitems].size_kb = size_kb;
        
        if (size_kb > max_kb) {
            max_kb = size_kb;
        }
        
        nitems++;
    }
    
    closedir(dir);
    
    /* Sort by size (descending) */
    qsort(items, nitems, sizeof(du_item_t), du_compare);
    
    /* Limit to top N */
    if (top_n >= 0 && nitems > top_n) {
        nitems = top_n;
    }

    /* --json: one object per entry, isolated from the columnar text path. */
    if (json_out) {
        for (int i = 0; i < nitems; i++) {
            char en[600]; jash_json_esc(items[i].name, en, sizeof en);
            jash_writef(w, ctx, "{\"name\":\"%s\",\"size_kb\":%ld}\n",
                        en, items[i].size_kb);
        }
        return 0;
    }

    /* Print results in columnar format (Unix-style) */
    if (!use_cache) {
        /* Header */
        char header[512];
        snprintf(header, sizeof(header), "%sDisk Usage: %s%s\n\n",
                J_ROLE_USER, path, J_RESET);
        jash_writes(w, ctx, header);
    }
    
    /* Calculate column widths for alignment */
    int max_name_len = 0;
    int max_size_len = 0;
    for (int i = 0; i < nitems; i++) {
        int name_len = strlen(items[i].name);
        if (name_len > max_name_len) max_name_len = name_len;
        
        char size_str[32];
        du_format_size(items[i].size_kb, size_str, sizeof(size_str));
        int size_len = strlen(size_str);
        if (size_len > max_size_len) max_size_len = size_len;
    }
    
    /* Cap name length to prevent overflow */
    if (max_name_len > 40) max_name_len = 40;
    
    for (int i = 0; i < nitems; i++) {
        /* Name (left-aligned, padded) - use snprintf for dynamic width */
        char name_padded[256];
        snprintf(name_padded, sizeof(name_padded), "%-*s", max_name_len, items[i].name);
        jash_writef(w, ctx, "%s%s%s ", J_ROLE_PATH, name_padded, J_RESET);
        
        /* Size (right-aligned) */
        char size_str[32];
        du_format_size(items[i].size_kb, size_str, sizeof(size_str));
        char size_padded[64];
        snprintf(size_padded, sizeof(size_padded), "%*s", max_size_len, size_str);
        jash_writef(w, ctx, "%s%s%s ", J_ROLE_ACCENT, size_padded, J_RESET);
        
        /* Bar (gradient) */
        du_print_bar(items[i].size_kb, max_kb, w, ctx);
    }
    
    /* Total */
    long total_kb = 0;
    for (int i = 0; i < nitems; i++) {
        total_kb += items[i].size_kb;
    }
    char total_str[32];
    du_format_size(total_kb, total_str, sizeof(total_str));
    
    jash_writes(w, ctx, "\n");
    jash_writef(w, ctx, "%sTotal: %s%s\n", J_ROLE_USER, total_str, J_RESET);
    
    return 0;
}

int jash_b_du(jash_state_t* state, jash_writer_t writer, void* context, const char* args) {
    jash_argv_t parsed = {0};
    if (jash_argv_build(args, &parsed) < 0) return 1;
    int result = jash_b_du_argv(state, writer, context, parsed.argc, parsed.argv);
    jash_argv_free(&parsed);
    return result;
}
