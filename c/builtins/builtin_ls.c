// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/ls — Unix-standard ls with columns, colors, and icons
//
// Features:
//   ls              — columnar output with colors and icons (GNU-style)
//   ls -l           — long format (permissions, size, date)
//   ls -a           — show hidden files
//   ls -1           — one file per line
//   ls *.py         — filter by pattern
//   ls --recent     — files modified in last 24h
//   ls --large      — top 10 largest files
//   ls --preview    — show first 3 lines of each file
//   ls --tree       — tree view (parent/child)
//
// Smart Cache: results cached with TTL 5s, invalidated on filesystem changes
// Pipe Detection: plain output when piped, colored/formatted when interactive
// Unix Standard: columnar layout, LS_COLORS, unicode icons

#include "builtin.h"
#include <core/argv.h>
#include <core/env.h>
#include <core/cache.h>
#include <core/gold.h>
#include <core/json_writer.h>
#include <core/structured_output.h>
#include <core/output_budget.h>
#include <core/data_wire.h>
#include <core/data_filter.h>
#include <core/unix_format.h>
#include "ls_collector.h"
#include "ls_data.h"
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <fnmatch.h>
#include "osif/osif_stat.h"
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>



/* ── Cache infrastructure ─────────────────────────────────────────────────── */

#define LS_CACHE_TTL 5  /* seconds */
#define LS_CACHE_MAX 32

typedef struct {
    char path[1024];   /* cache KEY: dir path + output-affecting flags (see BUG-035) */
    char* output;
    time_t timestamp;
} ls_cache_entry_t;

static ls_cache_entry_t g_ls_cache[LS_CACHE_MAX];
static int g_ls_cache_count = 0;

/* Check if pipe is connected (output is not a terminal) */
static int ls_is_piped(void) {
    struct stat st;
    return fstat(STDOUT_FILENO, &st) != 0 || !S_ISCHR(st.st_mode);
}

/* Find cache entry for path */
static ls_cache_entry_t* ls_cache_find(const char* path) {
    time_t now = time(NULL);
    for (int i = 0; i < g_ls_cache_count; i++) {
        if (strcmp(g_ls_cache[i].path, path) == 0) {
            if (now - g_ls_cache[i].timestamp < LS_CACHE_TTL) {
                return &g_ls_cache[i];
            }
            /* Expired */
            return NULL;
        }
    }
    return NULL;
}

/* Add or update cache entry */
static void ls_cache_update(const char* path, const char* output) {
    /* Find existing or empty slot */
    ls_cache_entry_t* entry = NULL;
    for (int i = 0; i < g_ls_cache_count; i++) {
        if (strcmp(g_ls_cache[i].path, path) == 0) {
            entry = &g_ls_cache[i];
            break;
        }
    }
    
    if (!entry && g_ls_cache_count < LS_CACHE_MAX) {
        entry = &g_ls_cache[g_ls_cache_count++];
    } else if (!entry) {
        /* Evict oldest */
        entry = &g_ls_cache[0];
        for (int i = 1; i < g_ls_cache_count; i++) {
            if (g_ls_cache[i].timestamp < entry->timestamp) {
                entry = &g_ls_cache[i];
            }
        }
    }
    
    strlcpy(entry->path, path, sizeof(entry->path));
    free(entry->output);
    entry->output = strdup(output);
    entry->timestamp = time(NULL);
}

/* ── File info structure ──────────────────────────────────────────────────── */

typedef struct {
    char name[256];
    char path[512];
    mode_t mode;
    off_t size;
    time_t mtime;
    uid_t uid;
    gid_t gid;
    int nlink;
    int is_dir;
} ls_file_info_t;

/* ── AI Enhancement Functions ──────────────────────────────────────────────────── */

/* AI-001: Describe file content/type automatically */
static const char* ls_get_ai_description(const char* filepath);

/* AI-002: Filter files by semantic content/description */
static int ls_filter_semantic(ls_file_info_t* files, int nfiles, const char* query);

/* AI-003: Inspect file details beyond basic properties */
static void ls_format_inspect_details(jash_writer_t w, void* ctx, const char* filepath);

/* AI-004: Find related files based on naming/type patterns */
static void ls_find_related_files(jash_writer_t w, void* ctx, const char* target_file);

/* AI-005: Monitor directory for real-time changes with JSON output */
static int ls_monitor_json_impl(jash_state_t* s, jash_writer_t w, void* ctx, const char* path);

/* Helper for case-insensitive substring search */
static int jash_strcasestr(const char* haystack, const char* needle);

/* ── Filtering functions ──────────────────────────────────────────────────── */

static int ls_filter_pattern(ls_file_info_t* files, int nfiles, const char* pattern) {
    int count = 0;
    for (int i = 0; i < nfiles; i++) {
        if (fnmatch(pattern, files[i].name, 0) == 0) {
            files[count++] = files[i];
        }
    }
    return count;
}

static int ls_filter_recent(ls_file_info_t* files, int nfiles) {
    time_t now = time(NULL);
    time_t cutoff = now - 86400;  /* 24 hours */
    int count = 0;
    for (int i = 0; i < nfiles; i++) {
        if (files[i].mtime >= cutoff) {
            files[count++] = files[i];
        }
    }
    return count;
}

static int ls_filter_large(ls_file_info_t* files, int nfiles, int top_n) {
    /* Simple selection sort for top N */
    for (int i = 0; i < top_n && i < nfiles; i++) {
        int max_idx = i;
        for (int j = i + 1; j < nfiles; j++) {
            if (files[j].size > files[max_idx].size) {
                max_idx = j;
            }
        }
        if (max_idx != i) {
            ls_file_info_t tmp = files[i];
            files[i] = files[max_idx];
            files[max_idx] = tmp;
        }
    }
    return top_n < nfiles ? top_n : nfiles;
}

/* ── Output formatting ────────────────────────────────────────────────────── */

static void ls_format_size(off_t size, char* buf, size_t bufsz) {
    if (size < 1024) {
        snprintf(buf, bufsz, "%ld", (long)size);
    } else if (size < 1024 * 1024) {
        snprintf(buf, bufsz, "%.1fK", size / 1024.0);
    } else if (size < 1024 * 1024 * 1024) {
        snprintf(buf, bufsz, "%.1fM", size / (1024.0 * 1024.0));
    } else {
        snprintf(buf, bufsz, "%.1fG", size / (1024.0 * 1024.0 * 1024.0));
    }
}

static void ls_format_time(time_t mtime, char* buf, size_t bufsz) {
    struct tm* tm = localtime(&mtime);
    time_t now = time(NULL);
    
    /* If file is older than 6 months, show year instead of time */
    if (now - mtime > 180 * 86400) {
        strftime(buf, bufsz, "%b %d  %Y", tm);
    } else {
        strftime(buf, bufsz, "%b %d %H:%M", tm);
    }
}

static void ls_format_permissions(mode_t mode, char* buf) {
    buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? ((mode & S_ISUID) ? 's' : 'x') : ((mode & S_ISUID) ? 'S' : '-');
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? ((mode & S_ISGID) ? 's' : 'x') : ((mode & S_ISGID) ? 'S' : '-');
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? ((mode & S_ISVTX) ? 't' : 'x') : ((mode & S_ISVTX) ? 'T' : '-');
    buf[10] = '\0';
}

static int ls_visual_name(const char* name, char* out, size_t cap, int max_width) {
    if (!name || !out || cap == 0) return 0;
    if (max_width < 8) max_width = 8;

    size_t o = 0;
    int width = 0;
    int truncated = 0;
    const unsigned char* p = (const unsigned char*)name;

    while (*p && o + 8 < cap) {
        if (width >= max_width) { truncated = 1; break; }
        if (*p < 0x20 || *p == 0x7f) {
            out[o++] = '?';
            p++;
            width++;
            continue;
        }
        if (*p < 0x80) {
            out[o++] = (char)*p++;
            width++;
            continue;
        }

        int bytes = 1;
        int w = 1;
        if ((*p & 0xE0) == 0xC0) bytes = 2;
        else if ((*p & 0xF0) == 0xE0) bytes = 3;
        else if ((*p & 0xF8) == 0xF0) { bytes = 4; w = 2; }
        if (width + w > max_width) { truncated = 1; break; }
        for (int i = 0; i < bytes && p[i] && o + 1 < cap; i++) out[o++] = (char)p[i];
        p += bytes;
        width += w;
    }

    if (*p) truncated = 1;
    if (truncated && o + 4 < cap) {
        memcpy(out + o, "...", 3);
        o += 3;
        width += 3;
    }
    out[o] = '\0';
    return width;
}

/* Maximum files to process without pagination */
#define LS_PAGINATION_THRESHOLD 100

/* ── AI Enhancement Functions ────────────────────────────────────────────────── */

/* AI-001: Describe file content/type automatically */
static const char* ls_description_for_mode(const char* filepath, mode_t mode) {
    
    if (S_ISDIR(mode)) return "directory";
    if (S_ISLNK(mode)) return "symbolic link";
    
    /* Analyze file extension and content for AI description */
    const char* ext = strrchr(filepath, '.');
    if (ext) {
        if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0) return "shell script";
        if (strcmp(ext, ".c") == 0) return "C source code";
        if (strcmp(ext, ".h") == 0) return "C header";
        if (strcmp(ext, ".py") == 0) return "Python script";
        if (strcmp(ext, ".js") == 0) return "JavaScript code";
        if (strcmp(ext, ".json") == 0) return "JSON data";
        if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return "YAML configuration";
        if (strcmp(ext, ".toml") == 0) return "TOML configuration";
        if (strcmp(ext, ".txt") == 0) return "text document";
        if (strcmp(ext, ".md") == 0) return "Markdown document";
        if (strcmp(ext, ".conf") == 0 || strcmp(ext, ".config") == 0) return "configuration file";
        if (strcmp(ext, ".log") == 0) return "log file";
        if (strcmp(ext, ".sql") == 0) return "SQL database script";
    }
    
    /* For executable files */
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return "executable program";
    
    return "data file";
}

static const char* ls_get_ai_description(const char* filepath) {
    struct stat st;
    if (stat(filepath, &st)) return "file not found";
    return ls_description_for_mode(filepath, st.st_mode);
}

/* AI-002: Filter files by semantic content/description */
static int ls_filter_semantic(ls_file_info_t* files, int nfiles, const char* query) {
    /* Placeholder for semantic search — for now, simple keyword matching */
    if (!query || !*query) return nfiles;
    
    int count = 0;
    for (int i = 0; i < nfiles; i++) {
        /* Match against filename */
        if (jash_strcasestr(files[i].name, query)) {
            files[count++] = files[i];
            continue;
        }
        
        /* Match against AI description */
        const char* desc = ls_get_ai_description(files[i].path);
        if (desc && jash_strcasestr(desc, query)) {
            files[count++] = files[i];
            continue;
        }
        
        /* For future: match against file content */
    }
    return count;
}

/* AI-003: Inspect file details beyond basic properties */
static void ls_format_inspect_details(jash_writer_t w, void* ctx, const char* filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        jash_writef(w, ctx, "  [file not accessible]\n");
        return;
    }
    
    /* File type */
    const char* type = S_ISDIR(st.st_mode) ? "directory" :
                      S_ISLNK(st.st_mode) ? "symlink" :
                      S_ISREG(st.st_mode) ? "regular file" : "special";
    
    /* Size in human-readable format */
    char size_buf[32];
    if (st.st_size < 1024) {
        snprintf(size_buf, sizeof(size_buf), "%ld B", (long)st.st_size);
    } else if (st.st_size < 1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1f KB", st.st_size / 1024.0);
    } else if (st.st_size < 1024 * 1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1f MB", st.st_size / (1024.0 * 1024.0));
    } else {
        snprintf(size_buf, sizeof(size_buf), "%.1f GB", st.st_size / (1024.0 * 1024.0 * 1024.0));
    }
    
    /* Permissions */
    char perms[11];
    ls_format_permissions(st.st_mode, perms);
    
    /* Last modified */
    char time_buf[32];
    ls_format_time(st.st_mtime, time_buf, sizeof(time_buf));
    
    /* Owner/group */
    struct passwd* pwd = getpwuid(st.st_uid);
    struct group* grp = getgrgid(st.st_gid);
    const char* user = pwd ? pwd->pw_name : "unknown";
    const char* group = grp ? grp->gr_name : "unknown";
    
    jash_writef(w, ctx, "  Type: %s\n", type);
    jash_writef(w, ctx, "  Size: %s\n", size_buf);
    jash_writef(w, ctx, "  Perm: %s\n", perms);
    jash_writef(w, ctx, "  Ownr: %s:%s\n", user, group);
    jash_writef(w, ctx, "  Time: %s\n", time_buf);
    
    /* AI-description */
    const char* ai_desc = ls_get_ai_description(filepath);
    jash_writef(w, ctx, "  AI: %s\n", ai_desc);
}

/* AI-004: Find related files based on naming/type patterns */
static void ls_find_related_files(jash_writer_t w, void* ctx, const char* target_file) {
    /* Extract base name without extension */
    char base[256];
    strlcpy(base, target_file, sizeof(base));
    char* ext = strrchr(base, '.');
    if (ext) *ext = '\0';
    
    /* Find directory of target file */
    char dir_path[512];
    const char* last_slash = strrchr(target_file, '/');
    if (last_slash) {
        size_t dirlen = last_slash - target_file + 1;
        if (dirlen >= sizeof(dir_path)) dirlen = sizeof(dir_path) - 1;
        memcpy(dir_path, target_file, dirlen);
        dir_path[dirlen] = '\0';
    } else {
        strlcpy(dir_path, "./", sizeof(dir_path));
    }
    
    DIR* dir = opendir(dir_path);
    if (!dir) return;
    
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* Skip hidden */
        
        /* Check if name contains base name or has related extension */
        if (strstr(ent->d_name, base)) {
            char full_path[1024];
            snprintf(full_path, sizeof(full_path), "%s%s", dir_path, ent->d_name);
            
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                const char* color = unix_get_file_color(ent->d_name, st.st_mode);
                const char* icon = unix_get_file_icon(ent->d_name, st.st_mode);
                jash_writef(w, ctx, "  %s%s%s\033[0m\n", icon, color, ent->d_name);
            }
        }
    }
    closedir(dir);
}

/* Helper for case-insensitive substring search */
static int jash_strcasestr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);
    if (needle_len > haystack_len) return 0;
    
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        int match = 1;
        for (size_t j = 0; j < needle_len; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* AI-005: Monitor directory for real-time changes with JSON output */
static int ls_monitor_json_impl(jash_state_t* s, jash_writer_t w, void* ctx, const char* path) {
    jash_writef(jash_stderr_writer, 0, "ls: --monitor feature not fully implemented yet\n");
    return 1;
}

/* ── Comparison function for qsort ────────────────────────────────────────── */

/* ADR-0044 ls parity: sort mode + reverse, set from flags. File-static (like
 * grep's g_ctx_*) so every qsort(...ls_name_cmp) site honors the requested
 * order without threading state through. Fixes the silent-lie flags: `ls -t`
 * used to return ALPHABETICAL order, `-S` too, and `-r` was a no-op. */
static int g_ls_sort = 0;     /* 0 = name, 1 = mtime (newest first), 2 = size (largest) */
static int g_ls_reverse = 0;  /* -r: reverse the final ordering */

static int ls_locale_name_cmp(const char* left, const char* right) {
    int order = strcoll(left, right);
    /* Locale collation can consider distinct names equivalent. Keep a stable,
     * bytewise tie-break so repeated listings never shuffle equal-sort names. */
    return order ? order : strcmp(left, right);
}

static int ls_name_cmp(const void* a, const void* b) {
    const ls_file_info_t* fa = (const ls_file_info_t*)a;
    const ls_file_info_t* fb = (const ls_file_info_t*)b;
    int r;
    if (g_ls_sort == 1) {            /* -t: newest mtime first */
        r = (fa->mtime < fb->mtime) - (fa->mtime > fb->mtime);
        if (r == 0) r = ls_locale_name_cmp(fa->name, fb->name);
    } else if (g_ls_sort == 2) {     /* -S: largest size first */
        r = (fa->size < fb->size) - (fa->size > fb->size);
        if (r == 0) r = ls_locale_name_cmp(fa->name, fb->name);
    } else {                          /* default: name */
        r = ls_locale_name_cmp(fa->name, fb->name);
    }
    return g_ls_reverse && r ? (r < 0 ? 1 : -1) : r;
}

/* ADR-0044 parity: emit ONE `ls -l` line for a single path operand. `ls -l
 * file` and `ls -ld dir` used to print just the bare name (show_long ignored on
 * the file-operand path). Uses lstat so a symlink shows as a symlink, and mirrors
 * the long-format columns of ls_format_listing. */
static void ls_emit_long_line(jash_writer_t w, void* ctx, const char* path,
                              const char* display, int piped) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        jash_writef(jash_stderr_writer, 0,
                    "ls: cannot access '%s': No such file or directory\n", path);
        return;
    }
    char perms[12]; ls_format_permissions(st.st_mode, perms);
    char size_buf[32]; ls_format_size(st.st_size, size_buf, sizeof(size_buf));
    char time_buf[32]; ls_format_time(st.st_mtime, time_buf, sizeof(time_buf));
    struct passwd* pw = getpwuid(st.st_uid);
    struct group*  gr = getgrgid(st.st_gid);
    const char* user  = pw ? pw->pw_name : "unknown";
    const char* group = gr ? gr->gr_name : "unknown";
    char linkarrow[1100]; linkarrow[0] = '\0';
    if (S_ISLNK(st.st_mode)) {
        char tgt[1024];
        ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
        if (n >= 0) { tgt[n] = '\0'; snprintf(linkarrow, sizeof linkarrow, " -> %s", tgt); }
    }
    if (piped) {
        jash_writef(w, ctx, "%s %3d %-8s %-8s %8s %s %s%s\n",
                    perms, (int)st.st_nlink, user, group, size_buf, time_buf, display, linkarrow);
    } else {
        const char* color = unix_get_file_color(display, st.st_mode);
        const char* icon  = unix_get_file_icon(display, st.st_mode);
        jash_writef(w, ctx, "%s %3d %-8s %-8s %8s %s %s%s%s\033[0m%s\n",
                    perms, (int)st.st_nlink, user, group, size_buf, time_buf,
                    icon, color, display, linkarrow);
    }
}

/* JSONL is an explicit adapter over the same filesystem observations used by
 * the native data plane. Names/paths retain invalid UTF-8 as tagged bytes. */
typedef struct {
    jash_writer_t writer;
    void* context;
    int bare_directory;
} ls_json_context_t;

static jash_ls_callback_result_t ls_json_item(void* context,
                                              const jash_ls_item_t* item) {
    ls_json_context_t* output = context;
    jash_json_writer_t json;
    jash_jsonw_init(&json, output->writer, output->context);
    const char* type = item->error.operation == JASH_LS_ITEM_ERROR_METADATA
        ? "missing" : item->type == JASH_LS_TYPE_DIRECTORY ? "dir"
        : item->type == JASH_LS_TYPE_SYMLINK ? "symlink"
        : item->type == JASH_LS_TYPE_REGULAR ? "file" : "other";
    const char* path = (const char*)(output->bare_directory
        ? item->name.data : item->path.data);
    char permissions[12] = "----------";
    if (item->metadata.available & JASH_LS_META_MODE)
        ls_format_permissions((mode_t)item->metadata.mode, permissions);

    jash_jsonw_raw(&json, "{\"name\":");
    jash_jsonw_string_or_bytes(&json, (const char*)item->name.data);
    jash_jsonw_raw(&json, ",\"path\":");
    jash_jsonw_string_or_bytes(&json, path);
    jash_jsonw_raw(&json, ",\"type\":");
    jash_jsonw_string(&json, type);
    jash_jsonw_raw(&json, ",\"size\":");
    jash_jsonw_u64(&json, (item->metadata.available & JASH_LS_META_SIZE)
                           ? item->metadata.size : 0);
    jash_jsonw_raw(&json, ",\"mode\":");
    jash_jsonw_string(&json, permissions);
    jash_jsonw_raw(&json, ",\"mtime\":");
    jash_jsonw_i64(&json, item->metadata.modified.available
                           ? item->metadata.modified.seconds : 0);
    if (item->error.operation != JASH_LS_ITEM_ERROR_NONE) {
        jash_jsonw_raw(&json, ",\"error\":{\"operation\":");
        jash_jsonw_string(&json,
            item->error.operation == JASH_LS_ITEM_ERROR_UNSTABLE ? "unstable" :
            item->error.operation == JASH_LS_ITEM_ERROR_READLINK ? "readlink" :
            "metadata");
        jash_jsonw_raw(&json, ",\"errno\":");
        jash_jsonw_i64(&json, item->error.error_number);
        jash_jsonw_raw(&json, "}");
    }
    jash_jsonw_raw(&json, "}\n");
    return jash_jsonw_finish(&json) == 0
        ? JASH_LS_CALLBACK_CONTINUE : JASH_LS_CALLBACK_ERROR;
}

static int ls_json_collect(jash_writer_t w, void* ctx, const char* root,
                           int directory, int show_all) {
    jash_ls_collect_options_t options = {
        .root = {(const unsigned char*)root, strlen(root)},
        .mode = directory ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW,
        .follow_root_symlink = 0,
        .include_hidden = show_all,
        .max_items = 65536,
        .max_logical_bytes = 32u * 1024u * 1024u,
        .max_memory_bytes = 32u * 1024u * 1024u,
        .max_symlink_bytes = 0
    };
    ls_json_context_t output = {w, ctx, directory && strcmp(root, ".") == 0};
    jash_ls_collect_summary_t summary;
    int result = jash_ls_collect(&options, ls_json_item, &output, &summary);
    if (result || summary.state != JASH_LS_TERMINAL_COMPLETE || summary.errors_seen) {
        jash_writef(jash_stderr_writer, 0,
            "ls --json: incomplete listing for '%s' (state %d, errno %d)\n",
            root, (int)summary.state, summary.error_number);
        return 1;
    }
    return 0;
}

static int ls_json_rows(jash_writer_t w, void* ctx, char targets[][1024],
                        int ntargets, int show_all) {
    if (!ntargets) return ls_json_collect(w, ctx, ".", 1, show_all);
    int result = 0;
    for (int i = 0; i < ntargets; i++) {
        struct stat st;
        if (lstat(targets[i], &st) != 0) {
            jash_writef(jash_stderr_writer, 0,
                        "ls: %s: No such file or directory\n", targets[i]);
            result = 1;
            continue;
        }
        if (ls_json_collect(w, ctx, targets[i], S_ISDIR(st.st_mode), show_all))
            result = 1;
    }
    return result;
}

static int ls_json(jash_writer_t w, void* ctx, char targets[][1024],
                   int ntargets, int show_all) {
    /* Preserve the established JSONL fields, but publish only after the whole
     * bounded result fits. The prose governor must never splice a marker into
     * a JSON row or report truncated machine output as a successful query. */
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, 64u * 1024u * 1024u);
    int status = ls_json_rows(jash_structured_buffer_sink, &buffer, targets, ntargets, show_all);
    if (buffer.error)
        status = jash_structured_error(w, ctx, "ls", "output_buffer_failed", 125);
    else
        status = jash_structured_document(w, ctx, "ls", buffer.data ? buffer.data : "",
            buffer.used, buffer.required, jash_budget_limit(), status);
    jash_structured_buffer_free(&buffer);
    return status;
}

typedef struct { jash_writer_t writer; void* context; } ls_wire_output_t;

static long ls_wire_sink(void* context, const void* data, size_t size) {
    ls_wire_output_t* output = context;
    return output->writer(output->context, (const char*)data, size);
}

/* --data explicitly exports the native stream to bytes (JTV1). A regular
 * POSIX pipe remains a byte pipe; no external stdout inference takes place. */
static int ls_data_emit(jash_writer_t w, void* ctx,
                        const char* const* roots, size_t root_count,
                        int show_all, const char* where_type, int data_version) {
    const char* root = roots[0];
    jash_ls_collect_options_t options = {
        .root = {(const unsigned char*)root, strlen(root)},
        .mode = JASH_LS_COLLECT_OPERAND,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW,
        .include_hidden = show_all,
        .max_items = 65536,
        .max_logical_bytes = 32u * 1024u * 1024u,
        .max_memory_bytes = 32u * 1024u * 1024u,
        .max_symlink_bytes = 1024
    };
    unsigned char value_storage[32768], wire_storage[32768];
    jash_ls_data_t source = {0};
    if (jash_ls_data_open_many(&source, &options, roots, root_count,
                               value_storage, sizeof(value_storage), NULL) < 0) {
        jash_writes(jash_stderr_writer, 0,
                    "ls --data: cannot open typed observation\n");
        return 1;
    }
    jdv_filter_t filter = {0};
    if (where_type) {
        jdv_value_t expected = {0};
        expected.kind = JDV_SYMBOL;
        expected.as.slice.data = (const uint8_t*)where_type;
        expected.as.slice.len = strlen(where_type);
        if (jdv_filter_equal_open(&filter, &source.cursor,
                                  JDV_LITERAL("type"), &expected) < 0) {
            (void)jash_ls_data_close(&source);
            jash_writes(jash_stderr_writer, 0,
                        "ls --data: cannot compose typed filter\n");
            return 1;
        }
    }
    jash_structured_output_t checked;
    if (jash_structured_begin(&checked, w, ctx)) {
        if (where_type) (void)jdv_filter_close(&filter);
        else (void)jash_ls_data_close(&source);
        return 125;
    }
    ls_wire_output_t output = {checked.json.sink, checked.json.context};
    jdv_stream_end_t terminal;
    /* INV-861: keep each local cursor's origin explicit across the call.
     * Selecting between pointers to source/filter on a joined expression
     * makes Bear's local-escape proof ambiguous. */
    int result;
    if (where_type) {
        if (data_version == 2)
            result = jdv_stream_write_frames_v2(&filter.cursor, ls_wire_sink, &output,
                                                 wire_storage, sizeof(wire_storage), &terminal);
        else
            result = jdv_stream_write_frames(&filter.cursor, ls_wire_sink, &output,
                                              wire_storage, sizeof(wire_storage), &terminal);
    } else {
        if (data_version == 2)
            result = jdv_stream_write_frames_v2(&source.cursor, ls_wire_sink, &output,
                                                 wire_storage, sizeof(wire_storage), &terminal);
        else
            result = jdv_stream_write_frames(&source.cursor, ls_wire_sink, &output,
                                              wire_storage, sizeof(wire_storage), &terminal);
    }
    int saved = errno;
    if (where_type) (void)jdv_filter_close(&filter);
    else (void)jash_ls_data_close(&source);
    if (result < 0) {
        jash_writef(jash_stderr_writer, 0,
                    "ls --data: incomplete transport for '%s' (errno %d)\n",
                    root, saved);
        return jash_structured_finish(&checked, 125);
    }
    return jash_structured_finish(&checked,
        terminal.state == JDV_STREAM_COMPLETE && !terminal.errors_seen ? 0 : 1);
}

/* ── ls_list_dir: collect entries from one directory ─────────────────────── */

/* Collect entries from `dirpath` into `files[0..*pnfiles-1]`.
 * Returns 0 on success, -1 if the directory cannot be opened.
 * `show_all` controls whether dotfiles are included (except . and .. always
 * excluded).  `max_files` is the capacity of `files`. */
typedef struct {
    ls_file_info_t* files;
    int capacity;
    int count;
} ls_text_context_t;

static jash_ls_callback_result_t ls_text_item(void* context,
                                              const jash_ls_item_t* item) {
    ls_text_context_t* output = context;
    /* The historic formatter uses fixed-size rows. Reject unrepresentable
     * names and paths instead of inventing metadata for silently cut strings. */
    if (item->name.len >= sizeof(output->files[0].name) ||
        item->path.len >= sizeof(output->files[0].path) ||
        output->count >= output->capacity) {
        errno = EOVERFLOW;
        return JASH_LS_CALLBACK_ERROR;
    }
    if (item->error.operation == JASH_LS_ITEM_ERROR_METADATA) return JASH_LS_CALLBACK_CONTINUE;
    if (!(item->metadata.available & JASH_LS_META_MODE) ||
        !(item->metadata.available & JASH_LS_META_SIZE) ||
        !(item->metadata.available & JASH_LS_META_UID) ||
        !(item->metadata.available & JASH_LS_META_GID) ||
        !(item->metadata.available & JASH_LS_META_NLINK) ||
        !item->metadata.modified.available ||
        item->metadata.nlink > INT_MAX ||
        (uint64_t)(off_t)item->metadata.size != item->metadata.size ||
        (int64_t)(time_t)item->metadata.modified.seconds !=
            item->metadata.modified.seconds) {
        errno = EOVERFLOW;
        return JASH_LS_CALLBACK_ERROR;
    }
    ls_file_info_t* file = &output->files[output->count++];
    memcpy(file->name, item->name.data, item->name.len);
    file->name[item->name.len] = '\0';
    memcpy(file->path, item->path.data, item->path.len);
    file->path[item->path.len] = '\0';
    file->mode = (mode_t)item->metadata.mode;
    file->size = (off_t)item->metadata.size;
    file->mtime = (time_t)item->metadata.modified.seconds;
    file->uid = (uid_t)item->metadata.uid;
    file->gid = (gid_t)item->metadata.gid;
    file->nlink = (int)item->metadata.nlink;
    file->is_dir = item->type == JASH_LS_TYPE_DIRECTORY;
    return JASH_LS_CALLBACK_CONTINUE;
}

static int ls_collect_dir(const char* dirpath, ls_file_info_t* files,
                          int max_files, int* pnfiles, int show_all) {
    *pnfiles = 0;
    ls_text_context_t output = {files, max_files, 0};
    jash_ls_collect_options_t options = {
        .root = {(const unsigned char*)dirpath, strlen(dirpath)},
        .mode = JASH_LS_COLLECT_DIRECTORY,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW,
        .follow_root_symlink = 1,
        .include_hidden = show_all,
        .max_items = (size_t)max_files,
        .max_logical_bytes = 2u * 1024u * 1024u,
        .max_memory_bytes = 2u * 1024u * 1024u,
        .max_symlink_bytes = 0
    };
    jash_ls_collect_summary_t summary;
    int result = jash_ls_collect(&options, ls_text_item, &output, &summary);
    if (result || summary.state != JASH_LS_TERMINAL_COMPLETE) {
        jash_writef(jash_stderr_writer, 0,
            "ls: incomplete listing for '%s' (state %d, errno %d)\n",
            dirpath, (int)summary.state, summary.error_number);
        errno = result ? summary.error_number : EOVERFLOW;
        return -1;
    }
    *pnfiles = output.count;
    return 0;
}

/* ── directory-listing core (format + output) ─────────────────────────────── */

/* Format `files[0..nfiles-1]` into a malloc'd string using the same output
 * logic as the existing builtin.  Flags: long, one-per-line, piped.  After
 * this function returns the caller owns the malloc'd string.
 *
 * Pass `dir_header` != NULL to prepend "\ndirpath:\n" before the listing
 * (recursive-mode convention). */
static char* ls_format_listing(ls_file_info_t* files, int nfiles,
                               int show_long, int show_one_per_line,
                               int piped, const char* dir_header) {
    size_t cap = 65536;
    char*  out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    size_t len = 0;

    /* Helper macro — append `str` to `out`, growing as needed */
#define LS_APPEND_OUT(_str) do { \
    size_t _l = strlen(_str); \
    while (len + _l >= cap) { cap *= 2; char* _n = realloc(out, cap); \
    if (!_n) { free(out); return NULL; } out = _n; } \
    memcpy(out + len, _str, _l); len += _l; out[len] = '\0'; \
} while(0)

    /* Optional directory header for recursive mode */
    if (dir_header) {
        LS_APPEND_OUT("\n");
        LS_APPEND_OUT(dir_header);
        LS_APPEND_OUT(":\n");
    }

    /* Sort alphabetically */
    if (nfiles > 1) qsort(files, (size_t)nfiles, sizeof(ls_file_info_t), ls_name_cmp);

    if (show_long) {
        unsigned long total_blocks = 0;
        for (int i = 0; i < nfiles; i++)
            total_blocks += (unsigned long)(files[i].size + 1023) / 1024;
        char total_line[64];
        snprintf(total_line, sizeof(total_line), "total %lu\n", total_blocks);
        LS_APPEND_OUT(total_line);
        for (int i = 0; i < nfiles; i++) {
            ls_file_info_t* f = &files[i];
            char perms[12]; ls_format_permissions(f->mode, perms);
            char size_buf[32]; ls_format_size(f->size, size_buf, sizeof(size_buf));
            char time_buf[32]; ls_format_time(f->mtime, time_buf, sizeof(time_buf));
            struct passwd* pw = getpwuid(f->uid);
            struct group*  gr = getgrgid(f->gid);
            const char* user  = pw ? pw->pw_name : "unknown";
            const char* group = gr ? gr->gr_name : "unknown";
            /* ADR-0044: symlinks show `name -> target` in -l (GNU). */
            char linkarrow[1100]; linkarrow[0] = '\0';
            if (S_ISLNK(f->mode)) {
                char tgt[1024];
                ssize_t n = readlink(f->path, tgt, sizeof(tgt) - 1);
                if (n >= 0) { tgt[n] = '\0'; snprintf(linkarrow, sizeof linkarrow, " -> %s", tgt); }
            }
            char line[2200];
            if (piped) {
                snprintf(line, sizeof(line), "%s %3d %-8s %-8s %8s %s %s%s\n",
                         perms, f->nlink, user, group, size_buf, time_buf, f->name, linkarrow);
            } else {
                const char* color = unix_get_file_color(f->name, f->mode);
                const char* icon  = unix_get_file_icon(f->name, f->mode);
                snprintf(line, sizeof(line), "%s %3d %-8s %-8s %8s %s %s%s%s%s%s\n",
                         perms, f->nlink, user, group, size_buf, time_buf,
                         icon, color, f->name, "\033[0m", linkarrow);
            }
            LS_APPEND_OUT(line);
        }
    } else if (show_one_per_line || piped) {
        for (int i = 0; i < nfiles; i++) {
            ls_file_info_t* f = &files[i];
            char line[1024];
            if (piped) snprintf(line, sizeof(line), "%s\n", f->name);
            else {
                const char* color = unix_get_file_color(f->name, f->mode);
                const char* icon  = unix_get_file_icon(f->name, f->mode);
                snprintf(line, sizeof(line), "%s%s%s%s\n", icon, color, f->name, "\033[0m");
            }
            LS_APPEND_OUT(line);
        }
    } else {
        /* Columnar output */
        const char* items[512]; int widths[512]; char formatted[512][512];
        int term_width = unix_get_terminal_width();
        int max_name_width = term_width / 3;
        if (max_name_width < 24) max_name_width = 24;
        if (max_name_width > 48) max_name_width = 48;
        for (int i = 0; i < nfiles; i++) {
            ls_file_info_t* f = &files[i];
            const char* color = unix_get_file_color(f->name, f->mode);
            const char* icon  = unix_get_file_icon(f->name, f->mode);
            char visible_name[256];
            int name_width = ls_visual_name(f->name, visible_name, sizeof(visible_name), max_name_width);
            snprintf(formatted[i], sizeof(formatted[i]), "%s%s%s%s",
                     icon, color, visible_name, "\033[0m");
            items[i]  = formatted[i];
            widths[i] = name_width + (icon[0] ? 2 : 0);
        }
        char* columns = unix_format_columns(items, nfiles, widths);
        if (columns) { LS_APPEND_OUT(columns); free(columns); }
    }
    return out;  /* caller must free() */
}

enum {
    LS_PRETTY_DIR,
    LS_PRETTY_LINK,
    LS_PRETTY_CODE,
    LS_PRETTY_DOC,
    LS_PRETTY_CONFIG,
    LS_PRETTY_EXEC,
    LS_PRETTY_FILE,
    LS_PRETTY_CATEGORIES
};

static int ls_pretty_category(const ls_file_info_t* file) {
    if (S_ISDIR(file->mode)) return LS_PRETTY_DIR;
    if (S_ISLNK(file->mode)) return LS_PRETTY_LINK;
    if (file->name[0] == '.' || !strcmp(file->name, "Makefile") ||
        !strcmp(file->name, "GNUmakefile") ||
        !strcmp(file->name, "CMakeLists.txt") ||
        !strcmp(file->name, "Dockerfile") || !strcmp(file->name, "Containerfile"))
        return LS_PRETTY_CONFIG;
    const char* ext = strrchr(file->name, '.');
    if (ext && (!strcasecmp(ext, ".c") || !strcasecmp(ext, ".h") ||
                !strcasecmp(ext, ".cc") || !strcasecmp(ext, ".cpp") ||
                !strcasecmp(ext, ".py") || !strcasecmp(ext, ".rs") ||
                !strcasecmp(ext, ".js") || !strcasecmp(ext, ".ts") ||
                !strcasecmp(ext, ".go"))) return LS_PRETTY_CODE;
    if (ext && (!strcasecmp(ext, ".md") || !strcasecmp(ext, ".txt") ||
                !strcasecmp(ext, ".rst") || !strcasecmp(ext, ".pdf")))
        return LS_PRETTY_DOC;
    if (ext && (!strcasecmp(ext, ".json") || !strcasecmp(ext, ".yaml") ||
                !strcasecmp(ext, ".yml") || !strcasecmp(ext, ".toml") ||
                !strcasecmp(ext, ".ini") || !strcasecmp(ext, ".conf")))
        return LS_PRETTY_CONFIG;
    if (file->mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return LS_PRETTY_EXEC;
    return LS_PRETTY_FILE;
}

static const char* ls_pretty_label(int category) {
    static const char* labels[LS_PRETTY_CATEGORIES] = {
        "DIR", "LINK", "CODE", "DOC", "CONFIG", "EXEC", "FILE"
    };
    return category >= 0 && category < LS_PRETTY_CATEGORIES
        ? labels[category] : "FILE";
}

static const char* ls_pretty_color(int category) {
    static const char* colors[LS_PRETTY_CATEGORIES] = {
        "\033[38;5;39m", "\033[38;5;45m", "\033[38;5;82m",
        "\033[38;5;51m", "\033[38;5;220m", "\033[38;5;201m",
        "\033[38;5;250m"
    };
    return category >= 0 && category < LS_PRETTY_CATEGORIES
        ? colors[category] : "\033[38;5;250m";
}

static int ls_pretty_append(char** output, size_t* length, size_t* capacity,
                            const char* text) {
    size_t n = strlen(text);
    if (n > SIZE_MAX - *length - 1u) return -1;
    if (*length + n + 1u > *capacity) {
        size_t next_capacity = *capacity ? *capacity : 4096u;
        while (next_capacity < *length + n + 1u) {
            if (next_capacity > SIZE_MAX / 2u) return -1;
            next_capacity *= 2u;
        }
        char* next = realloc(*output, next_capacity);
        if (!next) return -1;
        *output = next;
        *capacity = next_capacity;
    }
    memcpy(*output + *length, text, n);
    *length += n;
    (*output)[*length] = '\0';
    return 0;
}

/* A compact inventory for people and AI AUTO output: summary first, stable
 * indices, type groups, semantic colors, and an explicit visible-item limit. */
static char* ls_format_pretty(ls_file_info_t* files, int nfiles,
                              const char* path, int plain, int limit) {
    static const char* headings[LS_PRETTY_CATEGORIES] = {
        "DIRECTORIES", "SYMLINKS", "SOURCE CODE", "DOCUMENTS",
        "CONFIGURATION", "EXECUTABLES", "OTHER FILES"
    };
    if (nfiles > 1) qsort(files, (size_t)nfiles, sizeof(*files), ls_name_cmp);
    int counts[LS_PRETTY_CATEGORIES] = {0};
    for (int i = 0; i < nfiles; i++) counts[ls_pretty_category(&files[i])]++;

    int target_shown = limit > 0 && limit < nfiles ? limit : nfiles;
    int category_shown[LS_PRETTY_CATEGORIES] = {0};
    int shown = 0;
    while (shown < target_shown) {
        int progress = 0;
        for (int category = 0;
             category < LS_PRETTY_CATEGORIES && shown < target_shown;
             category++) {
            if (category_shown[category] < counts[category]) {
                category_shown[category]++;
                shown++;
                progress = 1;
            }
        }
        if (!progress) break;
    }
    char* output = NULL;
    size_t length = 0, capacity = 0;
    char line[2048];
    int n = snprintf(line, sizeof(line),
        "╭─ Directory · %s\n│ %d entries · %d directories · %d files\n",
        path && path[0] ? path : ".", nfiles, counts[LS_PRETTY_DIR],
        nfiles - counts[LS_PRETTY_DIR]);
    if (n < 0 || (size_t)n >= sizeof(line) ||
        ls_pretty_append(&output, &length, &capacity, line) < 0) goto fail;

    int index = 0;
    for (int category = 0; category < LS_PRETTY_CATEGORIES; category++) {
        if (!category_shown[category]) continue;
        const char* color = plain ? "" : ls_pretty_color(category);
        const char* reset = plain ? "" : "\033[0m";
        n = snprintf(line, sizeof(line), "├─ %s%s%s · %d/%d\n",
                     color, headings[category], reset,
                     category_shown[category], counts[category]);
        if (n < 0 || (size_t)n >= sizeof(line) ||
            ls_pretty_append(&output, &length, &capacity, line) < 0) goto fail;

        int category_index = 0;
        for (int i = 0; i < nfiles && category_index < category_shown[category]; i++) {
            if (ls_pretty_category(&files[i]) != category) continue;
            char display[256], name[272], size[32];
            int name_width = unix_get_terminal_width() - 31;
            if (name_width < 16) name_width = 16;
            if (name_width > 180) name_width = 180;
            ls_visual_name(files[i].name, display, sizeof(display), name_width);
            snprintf(name, sizeof(name), "%s%s", display,
                     files[i].is_dir ? "/" : "");
            ls_format_size(files[i].size, size, sizeof(size));
            const char* name_color = plain ? "" :
                unix_get_file_color(files[i].name, files[i].mode);
            const char* icon = unix_get_file_icon(files[i].name, files[i].mode);
            const char* label = ls_pretty_label(category);
            n = snprintf(line, sizeof(line), "│ %s%03d%s %s[%s]%s%*s %s%s%s%s %8s\n",
                         color, index + 1, reset, color, label, reset,
                         6 - (int)strlen(label), "",
                         icon, name_color, name, reset, size);
            if (n < 0 || (size_t)n >= sizeof(line) ||
                ls_pretty_append(&output, &length, &capacity, line) < 0)
                goto fail;
            index++;
            category_index++;
        }
    }

    n = shown < nfiles
        ? snprintf(line, sizeof(line),
                   "╰─ showing %d of %d · %d more; use --pretty-limit 0 to show all\n",
                   shown, nfiles, nfiles - shown)
        : snprintf(line, sizeof(line), "╰─ showing all %d entries\n", nfiles);
    if (n < 0 || (size_t)n >= sizeof(line) ||
        ls_pretty_append(&output, &length, &capacity, line) < 0) goto fail;
    return output;

fail:
    free(output);
    return NULL;
}

/* ── recursive walk ───────────────────────────────────────────────────────── */

/* Walk *dirpath* recursively, appending formatted listings to *w*.
 * `depth` controls nesting (0 = root, then depth+1 for children).
 * Avoids symlink loops by only descending into real directories. */
static int ls_walk_recursive(const char* dirpath, int show_long,
                             int show_all, int show_one_per_line,
                             int depth, jash_writer_t w, void* ctx) {
    int piped = ls_is_piped();
    ls_file_info_t files[512];
    int nfiles = 0;

    if (ls_collect_dir(dirpath, files, 512, &nfiles, show_all) < 0) {
        jash_writef(jash_stderr_writer, 0, "ls: cannot open '%s'\n", dirpath);
        return 1;
    }

    /* Format and emit this directory's listing */
    char* listing = ls_format_listing(files, nfiles, show_long,
                                      show_one_per_line, piped,
                                      (depth > 0) ? dirpath : NULL);
    if (listing) {
        jash_writes(w, ctx, listing);
        free(listing);
    }

    /* Recurse into subdirectories — sort them first for deterministic
     * output (POSIX ls -R sorts the directory listing, then recurses in
     * the same sorted order). */
    for (int i = 0; i < nfiles; i++) {
        if (files[i].is_dir) {
            char sub[2048];
            /* Avoid `..` and `.` (shouldn't be in the list anyway) */
            if (strcmp(files[i].name, ".") == 0 ||
                strcmp(files[i].name, "..") == 0)
                continue;
            snprintf(sub, sizeof(sub), "%s/%s", dirpath, files[i].name);
            ls_walk_recursive(sub, show_long, show_all, show_one_per_line,
                              depth + 1, w, ctx);
        }
    }
    return 0;
}

/* ── ls --delta (ADR-0044 Wave A1): session-aware change listing ──────────────
 *
 * Only-in-jash: bash / GNU ls has no session and no ledger, so it cannot answer
 * "what changed in this directory since I last looked?". `ls --delta` records a
 * per-(session, absolute-dir) snapshot of {name, mtime, size, type} and, on the
 * next call in the same session, emits ONLY the added / removed / modified
 * entries. An unchanged directory costs a one-line envelope (≤20 tokens) instead
 * of a full re-listing — the token win the ADR targets for the agent recon loop.
 *
 * Fully gated behind the explicit --delta flag: the default and --json listing
 * paths are byte-for-byte untouched. `JASH_GOLD=0` degrades --delta to a normal
 * listing (doctrine: gold persona off ⇒ POSIX behavior). */

#define LSD_MAX 4096

/* A1 now stands on the Wave A0 gold contract (c/core/gold.{c,h}): session id,
 * key hashing, per-(session,dir) snapshot load/store and age formatting are the
 * shared DELTA capability every gold builtin reuses. ls owns only the
 * ls-specific collection (fingerprint = nanosecond mtime) and rendering. */

static char lsd_typechar(mode_t m) {
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISREG(m)) return 'f';
    return 'o';
}

static int lsd_cmp(const void* a, const void* b) {
    return strcmp(((const jash_gold_dent_t*)a)->name,
                  ((const jash_gold_dent_t*)b)->name);
}

/* Snapshot `dir` into ents[] (sorted by name), fingerprint = nanosecond mtime
 * so an edit + re-ls within the same wall-clock second is still a change.
 * Returns count, -1 on open error. */
static int lsd_collect(const char* dir, int show_all, jash_gold_dent_t* ents, int max) {
    ls_file_info_t* files = (ls_file_info_t*)malloc((size_t)max * sizeof *files);
    if (!files) return -1;
    int n = 0;
    if (ls_collect_dir(dir, files, max, &n, show_all) != 0) { free(files); return -1; }
    int m = 0;
    for (int i = 0; i < n && m < max; i++) {
        jash_gold_dent_t* e = &ents[m++];
        strlcpy(e->name, files[i].name, sizeof e->name);
        e->size = (long long)files[i].size;
        e->tag  = lsd_typechar(files[i].mode);
        struct stat st;
        if (stat(files[i].path, &st) == 0)
            e->fp = (long long)BEAR_ST_MTIM(st).tv_sec * 1000000000LL
                  + (long long)BEAR_ST_MTIM(st).tv_nsec;
        else
            e->fp = (long long)files[i].mtime * 1000000000LL;
    }
    free(files);
    qsort(ents, (size_t)m, sizeof(jash_gold_dent_t), lsd_cmp);
    return m;
}

/* ls --delta driver. `dir` defaults to "." (or the first target). When as_json,
 * emits one JSONL record per change plus a trailing summary object. */
static int ls_delta(jash_writer_t w, void* ctx, const char* dir, int show_all, int as_json) {
    char absdir[4096];
    if (!realpath(dir, absdir)) strlcpy(absdir, dir, sizeof absdir);

    jash_gold_dent_t* cur  = (jash_gold_dent_t*)malloc(LSD_MAX * sizeof *cur);
    jash_gold_dent_t* prev = (jash_gold_dent_t*)malloc(LSD_MAX * sizeof *prev);
    if (!cur || !prev) { free(cur); free(prev);
        jash_writef(jash_stderr_writer, 0, "ls: --delta: out of memory\n"); return 1; }

    int nc = lsd_collect(dir, show_all, cur, LSD_MAX);
    if (nc < 0) {
        free(cur); free(prev);
        jash_writef(jash_stderr_writer, 0, "ls: %s: cannot open\n", dir);
        return 1;
    }

    /* Gold DELTA contract: key on (session, absolute-dir); load prior snapshot,
     * store the fresh one, diff. */
    char key[32];
    jash_gold_key(jash_gold_session(), absdir, key, sizeof key);
    long long age_s = 0;
    int np = jash_gold_delta_load("ls", key, prev, LSD_MAX, &age_s);
    jash_gold_delta_store("ls", key, cur, nc);   /* refresh to current state */

    if (np < 0) {
        /* Baseline: first look this session. Show entries + arm tracking. */
        if (as_json) {
            for (int i = 0; i < nc; i++) {
                char en[520]; jash_gold_json_esc(cur[i].name, en, sizeof en);
                jash_writef(w, ctx,
                    "{\"change\":\"baseline\",\"name\":\"%s\",\"type\":\"%c\",\"size\":%lld}\n",
                    en, cur[i].tag, cur[i].size);
            }
            jash_writef(w, ctx,
                "{\"summary\":{\"baseline\":true,\"entries\":%d}}\n", nc);
        } else {
            for (int i = 0; i < nc; i++)
                jash_writef(w, ctx, "  %s%s\n", cur[i].name, cur[i].tag == 'd' ? "/" : "");
            jash_writef(w, ctx, "[delta] baseline armed · %d entr%s\n",
                        nc, nc == 1 ? "y" : "ies");
        }
        free(cur); free(prev);
        return 0;
    }

    /* Merge-diff prev vs cur (both sorted by name). */
    char age[32]; jash_gold_age(age_s, age, sizeof age);
    int i = 0, j = 0, changes = 0;
    while (i < np || j < nc) {
        int c;
        if (i >= np)      c = 1;    /* only cur left  → added   */
        else if (j >= nc) c = -1;   /* only prev left → removed */
        else              c = strcmp(prev[i].name, cur[j].name);

        if (c < 0) {                /* removed */
            if (as_json) { char en[520]; jash_gold_json_esc(prev[i].name, en, sizeof en);
                jash_writef(w, ctx, "{\"change\":\"removed\",\"name\":\"%s\"}\n", en); }
            else jash_writef(w, ctx, "- %s\n", prev[i].name);
            changes++; i++;
        } else if (c > 0) {         /* added */
            if (as_json) { char en[520]; jash_gold_json_esc(cur[j].name, en, sizeof en);
                jash_writef(w, ctx,
                    "{\"change\":\"added\",\"name\":\"%s\",\"type\":\"%c\",\"size\":%lld}\n",
                    en, cur[j].tag, cur[j].size); }
            else jash_writef(w, ctx, "+ %s%s\n", cur[j].name, cur[j].tag == 'd' ? "/" : "");
            changes++; j++;
        } else {                    /* same name → modified? */
            if (prev[i].fp != cur[j].fp || prev[i].size != cur[j].size) {
                long long dsz = cur[j].size - prev[i].size;
                if (as_json) { char en[520]; jash_gold_json_esc(cur[j].name, en, sizeof en);
                    jash_writef(w, ctx,
                        "{\"change\":\"modified\",\"name\":\"%s\",\"size\":%lld,\"size_delta\":%lld}\n",
                        en, cur[j].size, dsz); }
                else if (dsz != 0)
                    /* jash_writef has no %+ flag — sign the value by hand. */
                    jash_writef(w, ctx, "~ %s  %s%lldB\n", cur[j].name,
                                dsz > 0 ? "+" : "", dsz);
                else
                    jash_writef(w, ctx, "~ %s  (touched)\n", cur[j].name);
                changes++;
            }
            i++; j++;
        }
    }

    if (as_json)
        jash_writef(w, ctx,
            "{\"summary\":{\"changes\":%d,\"entries\":%d,\"since_s\":%lld}}\n",
            changes, nc, age_s);
    else if (changes == 0)
        jash_writef(w, ctx, "[delta] no changes since %s · %d entr%s\n",
                    age, nc, nc == 1 ? "y" : "ies");
    else
        jash_writef(w, ctx, "[delta] %d change%s since %s · %d entr%s\n",
                    changes, changes == 1 ? "" : "s", age, nc, nc == 1 ? "y" : "ies");

    free(cur); free(prev);
    return 0;
}

/* ── Main ls builtin ──────────────────────────────────────────────────────── */

#define LS_MAX_TARGETS 1024
#define LS_ARGS_MAX    (64 * 1024)

#include "ls_structured.inc"
#include "ls_delta_structured.inc"

int jash_b_ls_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                   int argc, char** argv) {
    (void)s;

    if (argc < 0 || (argc && !argv)) return 1;
    if (argc >= LS_MAX_TARGETS) {
        jash_writef(jash_stderr_writer, 0,
                    "ls: argumentos demais (max %d)\n", LS_MAX_TARGETS - 1);
        return 1;
    }
    size_t argument_bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return 1;
        size_t length = strlen(argv[i]);
        if (length >= LS_ARGS_MAX ||
            argument_bytes >= LS_ARGS_MAX - length) {
            jash_writef(jash_stderr_writer, 0,
                        "ls: lista de argumentos longa demais (max %d bytes)\n",
                        LS_ARGS_MAX - 1);
            return 1;
        }
        argument_bytes += length + 1;
    }

    /* ADR-0044 parity: the sort-mode globals are static and jash persists state
     * across `-c` calls — reset per invocation so a prior `ls -t`/`-r` never
     * leaks its ordering into the next `ls`. */
    g_ls_sort = 0;
    g_ls_reverse = 0;

    /* Parse arguments */
    int show_long = 0;
    int show_all = 0;
    int show_one_per_line = 0;
    int show_preview = 0;
    int show_tree = 0;
    int show_pretty = 0;
    int pretty_limit = 48;
    int show_recursive = 0;
    int show_dir_itself = 0;  /* ADR-0044: -d — list the dir entry, not contents */
    int filter_recent_flag = 0;
    int filter_large_flag = 0;
    int force_color = 0;
    int force_columns = 0;
    int json_flag = 0;   /* Wave 7c: structured output */
    int data_flag = 0; /* 1=JTV1, 2=JTV2 with verified logical item credits */
    int delta_flag = 0;  /* ADR-0044 A1: session-aware change listing */
    int ai_describe_flag = 0;     /* AI-001: --ai --describe */
    int semantic_search_flag = 0; /* AI-002: --semantic="query" */
    int inspect_flag = 0;         /* AI-003: --inspect */
    int relate_flag = 0;          /* AI-004: --relate=file */
    int monitor_flag = 0;         /* AI-005: --monitor=dir */
    const char* semantic_query = NULL;  /* Query for semantic search */
    const char* relate_target = NULL;   /* Target file for relation mapping */
    const char* monitor_path = NULL;    /* Path for monitoring */
    const char* where_type = NULL;
    const char* pattern = NULL;
    char path_buf[1024];
    strlcpy(path_buf, ".", sizeof(path_buf));
    const char* path = path_buf;

    /* JINV-012 — ls nao pode PERDER argumento nem FABRICAR "No such file".
     *
     * Eram tres tetos, todos silenciosos: `args_copy[1024]` cortava a string
     * de argumentos inteira; `argv[32]` + `argc < 31` descartava do 31o em
     * diante; `targets[32]` idem. Reproduzido em 2026-07-27: diretorio com 40
     * arquivos, `ls f*.txt` listava 31 — e o corte no MEIO de um nome ainda
     * fazia o ls reportar "No such file or directory" para um arquivo que
     * EXISTE. Mentir sobre o filesystem e pior que truncar.
     *
     * Slab estatico (idioma ja usado em cmdsub.c) em vez de pilha: 1024
     * alvos x 1024 bytes = 1 MiB em .bss, sem free a perseguir em 6 caminhos
     * de retorno. `ls` nao e reentrante, entao static e seguro aqui.
     * Overflow agora e erro ALTO, nao descarte mudo.
     * Gate: tests/test_jinv011_printf_ls_args.sh
     */
    static char targets[LS_MAX_TARGETS][1024];
    int ntargets = 0;
    int explicit_path = 0;
    int operands_only = 0;

    for (int i = 0; i < argc; i++) {
        if (!operands_only && strcmp(argv[i], "--") == 0) {
            operands_only = 1;
            continue;
        }
        /* The evaluator and --exec-json already know the argv boundaries.
         * Never flatten them: whitespace, quotes and shell metacharacters
         * in an operand are filesystem bytes, not another command line. */
        if (operands_only || argv[i][0] != '-' || argv[i][1] == '\0') {
            if (strlen(argv[i]) >= sizeof(targets[0])) {
                jash_writes(jash_stderr_writer, 0,
                            "ls: path argument exceeds 1023 bytes\n");
                return 1;
            }
            strlcpy(targets[ntargets++], argv[i], sizeof(targets[0]));
            continue;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--long") == 0) {
            show_long = 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            show_all = 1;
        } else if (strcmp(argv[i], "-R") == 0
                   || strcmp(argv[i], "--recursive") == 0) {
            show_recursive = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reverse") == 0) {
            /* ADR-0044 parity: -r is REVERSE (GNU), not recursive. It was
             * mis-wired to recursion — a surprising, expensive wrong answer. */
            g_ls_reverse = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            g_ls_sort = 1;   /* sort by mtime (newest first) */
        } else if (strcmp(argv[i], "-S") == 0) {
            g_ls_sort = 2;   /* sort by size (largest first) */
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--directory") == 0) {
            show_dir_itself = 1;
        } else if (strcmp(argv[i], "-1") == 0) {
            show_one_per_line = 1;
        } else if (strcmp(argv[i], "--preview") == 0) {
            show_preview = 1;
        } else if (strcmp(argv[i], "--pretty") == 0 ||
                   strcmp(argv[i], "--index") == 0) {
            show_pretty = 1;
        } else if (strcmp(argv[i], "--pretty-limit") == 0) {
            if (i + 1 >= argc) {
                jash_writes(jash_stderr_writer, 0,
                            "ls: --pretty-limit requires a number from 0 to 512\n");
                return 2;
            }
            char* end = NULL;
            errno = 0;
            long parsed = strtol(argv[++i], &end, 10);
            if (errno || !end || *end || parsed < 0 || parsed > 512) {
                jash_writes(jash_stderr_writer, 0,
                            "ls: --pretty-limit must be between 0 and 512\n");
                return 2;
            }
            pretty_limit = (int)parsed;
            show_pretty = 1;
        } else if (strcmp(argv[i], "--tree") == 0) {
            show_tree = 1;
        } else if (strcmp(argv[i], "--recent") == 0) {
            filter_recent_flag = 1;
        } else if (strcmp(argv[i], "--large") == 0) {
            filter_large_flag = 1;
        } else if (strcmp(argv[i], "--color=always") == 0 || strcmp(argv[i], "--color") == 0) {
            force_color = 1;
        } else if (strcmp(argv[i], "--columns") == 0 || strcmp(argv[i], "-C") == 0) {
            force_columns = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_flag = 1;
        } else if (strcmp(argv[i], "--data") == 0) {
            if (data_flag == 2) {
                jash_writes(jash_stderr_writer, 0,
                            "ls: --data and --data-v2 are exclusive\n");
                return 1;
            }
            data_flag = 1;
        } else if (strcmp(argv[i], "--data-v2") == 0) {
            if (data_flag == 1) {
                jash_writes(jash_stderr_writer, 0,
                            "ls: --data and --data-v2 are exclusive\n");
                return 1;
            }
            data_flag = 2;
        } else if (strncmp(argv[i], "--where-type=", 13) == 0) {
            where_type = argv[i] + 13;
        } else if (strcmp(argv[i], "--delta") == 0) {
            delta_flag = 1;
        } else if (strcmp(argv[i], "--ai") == 0 || strcmp(argv[i], "--describe") == 0) {
            ai_describe_flag = 1;
        } else if (strncmp(argv[i], "--semantic=", 11) == 0) {
            semantic_query = argv[i] + 11;
            semantic_search_flag = 1;
        } else if (strcmp(argv[i], "--semantic") == 0 && i + 1 < argc) {
            semantic_query = argv[++i];
            semantic_search_flag = 1;
        } else if (strcmp(argv[i], "--inspect") == 0) {
            inspect_flag = 1;
        } else if (strncmp(argv[i], "--relate=", 9) == 0) {
            relate_target = argv[i] + 9;
            relate_flag = 1;
        } else if (strcmp(argv[i], "--relate") == 0 && i + 1 < argc) {
            relate_target = argv[++i];
            relate_flag = 1;
        } else if (strncmp(argv[i], "--monitor=", 10) == 0) {
            monitor_path = argv[i] + 10;
            monitor_flag = 1;
        } else if (strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
            monitor_path = argv[++i];
            monitor_flag = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            /* Handle combined flags: -la, -al, -lah, etc. */
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'l': show_long = 1; break;
                    case 'a': show_all = 1; break;
                    case '1': show_one_per_line = 1; break;
                    case 'h': /* human-readable — currently same as default */ break;
                    case 'r': g_ls_reverse = 1; break;   /* ADR-0044: reverse (was no-op) */
                    case 'R': show_recursive = 1; break;
                    case 't': g_ls_sort = 1; break;      /* ADR-0044: by mtime (was no-op) */
                    case 'S': g_ls_sort = 2; break;      /* ADR-0044: by size (was no-op) */
                    case 'd': show_dir_itself = 1; break;/* ADR-0044: dir entry, not contents */
                    case 'C': force_columns = 1; break;
                    /* Compatibility no-ops for common BSD/GNU ls flags
                     * (`alias ls='ls -G'`, ls -F, etc.). Accept silently. */
                    case 'G': case 'F': case 'p': case 'A':
                    case 'f': case 'k': case 'n':
                    case 'o': case 'q': case 's': case 'u': case 'x':
                        break;
                    default:
                        jash_writef(jash_stderr_writer, 0, "ls: unknown flag '-%c'\n", argv[i][j]);
                        return 1;
                }
            }
        }
    }

    if (where_type && (!(data_flag || json_flag) || !*where_type ||
        (strcmp(where_type, "file") && strcmp(where_type, "dir") &&
         strcmp(where_type, "symlink") && strcmp(where_type, "fifo") &&
         strcmp(where_type, "socket") && strcmp(where_type, "block") &&
         strcmp(where_type, "char") && strcmp(where_type, "unknown")))) {
        jash_writes(jash_stderr_writer, 0,
            "ls: --where-type requires --json/--data and a known file type\n");
        return 1;
    }
    if (data_flag || json_flag) {
        if (data_flag && json_flag) {
            jash_writes(jash_stderr_writer, 0, "ls: --json and --data are exclusive\n");
            return 2;
        }
        if (monitor_flag || show_tree)
            return jash_structured_error(w, ctx, "ls", monitor_flag ? "monitor_unavailable" : "tree_unavailable", 2);
        const char* roots[LS_MAX_TARGETS];
        for (int i = 0; i < ntargets; i++) roots[i] = targets[i];
        if (!ntargets) roots[0] = ".";
        size_t root_count = (size_t)(ntargets ? ntargets : 1);
        if (delta_flag && !jash_gold_disabled()) {
            if (ntargets > 1 || show_long || show_recursive || show_dir_itself ||
                show_preview || show_pretty || filter_recent_flag || filter_large_flag ||
                g_ls_sort || g_ls_reverse || ai_describe_flag || semantic_search_flag ||
                inspect_flag || relate_flag || where_type)
                return jash_structured_error(w, ctx, "ls", "delta_requires_one_unfiltered_directory", 2);
            return ls_structured_delta(s, w, ctx, roots[0], show_all, data_flag);
        }
        if (relate_flag && (ntargets || show_recursive || show_dir_itself))
            return jash_structured_error(w, ctx, "ls", "relate_uses_its_target_directory", 2);
        int advanced = show_long || show_recursive || show_dir_itself || show_preview ||
            show_pretty || filter_recent_flag || filter_large_flag || g_ls_sort || g_ls_reverse ||
            ai_describe_flag || semantic_search_flag || inspect_flag || relate_flag || (json_flag && where_type);
        if (advanced) {
            lsx_options_t options = {.all = show_all, .recursive = show_recursive, .directory = show_dir_itself,
                .sort = g_ls_sort, .reverse = g_ls_reverse, .recent = filter_recent_flag, .large = filter_large_flag,
                .pretty = show_pretty, .pretty_limit = pretty_limit, .describe = ai_describe_flag, .inspect = inspect_flag,
                .long_format = show_long, .preview = show_preview, .version = data_flag,
                .semantic = semantic_query, .related = relate_target, .where_type = where_type};
            return ls_structured_query(s, w, ctx, roots, root_count, &options);
        }
        if (data_flag) return ls_data_emit(w, ctx, roots, root_count, show_all, where_type, data_flag);
        return ls_json(w, ctx, targets, ntargets, show_all);
    }

    /* AI-005: Monitor mode with JSON output — real-time file system monitoring.
     * This is a fully separate path that watches a directory for changes. */
    if (monitor_flag && json_flag) {
        if (!monitor_path) monitor_path = ".";
        return ls_monitor_json_impl(s, w, ctx, monitor_path);
    }
    
    /* ADR-0044 A1: --delta is a fully separate, session-aware path. Gated
     * behind the explicit flag; the default listing is untouched. JASH_GOLD=0
     * degrades it to a normal listing (doctrine: gold off ⇒ POSIX behavior). */
    if (delta_flag) {
        if (!jash_gold_disabled()) {
            const char* d = ntargets > 0 ? targets[0] : ".";
            return ls_delta(w, ctx, d, show_all, json_flag);
        }
        /* gold off → fall through to the normal (or --json) listing. */
    }

    /* Wave 7c: structured output is a fully separate path — the text listing
     * below (battery-tested) is untouched. */
    if (json_flag) return ls_json(w, ctx, targets, ntargets, show_all);

    /* Resolve targets:
     *   - If a target stats as a regular file, it's a file to print.
     *   - If a target stats as a directory, it becomes the listing path.
     *   - If a target contains glob chars and doesn't stat, error like bash.
     *   - Single dir target: behave as before (list its contents).
     *   - One or more file targets: print each (bash parity for `ls f1 f2`).
     */
    /* JINV-012: eram [32] fixos aqui também — o terceiro e quarto tetos da
     * mesma cadeia. Um `ls` com 40 alvos passava pelos dois primeiros e morria
     * calado aqui, listando 32. Mesmo slab, mesmo teto, mesma falha ALTA. */
    static char file_args[LS_MAX_TARGETS][1024];
    int nfile_args = 0;
    static char dir_args[LS_MAX_TARGETS][1024];
    int ndir_args = 0;
    int rc_targets = 0;

    /* -d with no operand lists the current directory entry itself (GNU). */
    if (show_dir_itself && ntargets == 0) {
        strlcpy(targets[0], ".", sizeof(targets[0]));
        ntargets = 1;
    }

    if (ntargets > 0) {
        int dir_count = 0;
        for (int i = 0; i < ntargets; i++) {
            struct stat tst;
            if (stat(targets[i], &tst) == 0) {
                /* -d: a directory operand is listed as an ENTRY (its own name/
                 * long line), not descended into. Route it to file_args. */
                if (S_ISDIR(tst.st_mode) && !show_dir_itself) {
                    if (ndir_args < LS_MAX_TARGETS) {   /* JINV-012 */
                        strlcpy(dir_args[ndir_args++], targets[i], sizeof(dir_args[0]));
                    }
                    strlcpy(path_buf, targets[i], sizeof(path_buf));
                    explicit_path = 1;
                    dir_count++;
                } else {
                    if (nfile_args < LS_MAX_TARGETS) {  /* JINV-012 */
                        strlcpy(file_args[nfile_args++], targets[i], sizeof(file_args[0]));
                    }
                }
            } else {
                /* No such target. If it looks like a glob, bash-style error. */
                if (strchr(targets[i], '*') || strchr(targets[i], '?') ||
                    strchr(targets[i], '[')) {
                    jash_writef(jash_stderr_writer, 0,
                                "ls: %s: No such file or directory\n", targets[i]);
                } else {
                    jash_writef(jash_stderr_writer, 0,
                                "ls: cannot access '%s': No such file or directory\n",
                                targets[i]);
                }
                /* Exit status for an unreadable operand is lineage-specific:
                 * GNU coreutils uses 2 ("serious trouble"), BSD/macOS uses 1.
                 * jash mirrors the ls the host ships, so a script's `|| exit`
                 * and `$?` checks behave the same either way. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
                rc_targets = 1;
#else
                rc_targets = 2;
#endif
            }
        }

        /* If we have file args and no dir args, just print the files. */
        if (nfile_args > 0 && dir_count == 0) {
            int piped_now = ls_is_piped();
            if (force_color || force_columns) piped_now = 0;
            for (int i = 0; i < nfile_args; i++) {
                if (show_long) {
                    /* ADR-0044: `ls -l file` / `ls -ld dir` — long format on a
                     * single operand (was ignored: only the name printed). */
                    ls_emit_long_line(w, ctx, file_args[i], file_args[i], piped_now);
                } else if (piped_now) {
                    jash_writef(w, ctx, "%s\n", file_args[i]);
                } else {
                    struct stat fst;
                    const char* color = "";
                    const char* icon = "";
                    if (stat(file_args[i], &fst) == 0) {
                        color = unix_get_file_color(file_args[i], fst.st_mode);
                        icon = unix_get_file_icon(file_args[i], fst.st_mode);
                    }
                    jash_writef(w, ctx, "%s%s%s\033[0m\n",
                                icon, color, file_args[i]);
                }
            }
            return rc_targets;
        }

        /* If there were only failed targets and no valid dir/file, exit. */
        if (dir_count == 0 && nfile_args == 0) {
            return rc_targets;
        }
        /* Otherwise: fall through to dir-listing for `path_buf`. */
        (void)explicit_path;
    }
    
    /* Check cache */
    int piped = ls_is_piped();
    /* Override pipe detection if user forced color/columns */
    if (force_color || force_columns) {
        piped = 0;
    }
    /* BUG-035: the cache key MUST include the output-affecting flags — keying on
     * the path alone made `ls` and `ls -la` (and `-a`/`-1`) in the same dir within
     * the 5s TTL return EACH OTHER's cached output (stale / wrong / empty grid).
     * Key on path + cwd + a compact flag signature so each view caches independently
     * and changing directories invalidates the "." cache correctly. */
    char ls_ckey[1024];
    /* Recursive mode — dedicated path, no cache */
    if (show_recursive) {
        int any_targets = 0;
        int multi_target = (ndir_args + nfile_args > 1);
        for (int i = 0; i < ndir_args; i++) {
            any_targets = 1;
            int d = 0;
            if (multi_target) {
                if (i > 0) d = 1;
                else {
                    char hdr[1032];
                    snprintf(hdr, sizeof(hdr), "%s:\n", dir_args[i]);
                    jash_writes(w, ctx, hdr);
                }
            }
            ls_walk_recursive(dir_args[i], show_long, show_all,
                              show_one_per_line, d, w, ctx);
        }
        for (int i = 0; i < nfile_args; i++) {
            any_targets = 1;
            struct stat fst;
            if (stat(file_args[i], &fst) == 0 && S_ISDIR(fst.st_mode)) {
                ls_walk_recursive(file_args[i], show_long, show_all,
                                  show_one_per_line, 0, w, ctx);
            } else {
                char line[1024];
                if (piped) snprintf(line, sizeof(line), "%s\n", file_args[i]);
                else {
                    const char* c = unix_get_file_color(file_args[i], fst.st_mode);
                    const char* ic= unix_get_file_icon(file_args[i], fst.st_mode);
                    snprintf(line, sizeof(line), "%s%s%s\033[0m\n", ic, c, file_args[i]);
                }
                jash_writes(w, ctx, line);
            }
        }
        if (!any_targets)
            ls_walk_recursive(path, show_long, show_all,
                              show_one_per_line, 0, w, ctx);
        return rc_targets;
    }

    /* Non-recursive multi-target: iterate with headers (POSIX: `ls dir1 dir2`, `ls f1 dir1`) */
    if (ndir_args > 1 || (ndir_args > 0 && nfile_args > 0)) {
        /* Print file args first (no headers) */
        for (int i = 0; i < nfile_args; i++) {
            struct stat fst;
            const char* color = "";
            const char* icon = "";
            if (stat(file_args[i], &fst) == 0) {
                color = unix_get_file_color(file_args[i], fst.st_mode);
                icon = unix_get_file_icon(file_args[i], fst.st_mode);
            }
            if (piped) jash_writef(w, ctx, "%s\n", file_args[i]);
            else jash_writef(w, ctx, "%s%s%s\033[0m\n", icon, color, file_args[i]);
        }
        /* Then dir args with headers */
        for (int i = 0; i < ndir_args; i++) {
            if (nfile_args > 0 || i > 0) jash_writef(w, ctx, "\n");
            jash_writef(w, ctx, "%s:\n", dir_args[i]);
            ls_file_info_t df[512];
            int ndf = 0;
            if (ls_collect_dir(dir_args[i], df, 512, &ndf, show_all) == 0) {
                char* out = ls_format_listing(df, ndf, show_long,
                                              show_one_per_line, piped, NULL);
                if (out) { jash_writes(w, ctx, out); free(out); }
            } else {
                jash_writef(jash_stderr_writer, 0, "ls: cannot access '%s'\n", dir_args[i]);
            }
        }
        return rc_targets;
    }

    /* Cache key + lookup */
    char cwd[1024];
    const char* cwdptr = getcwd(cwd, sizeof(cwd)) ? cwd : "";
    snprintf(ls_ckey, sizeof ls_ckey, "%s|%s|l%da%d1%dc%dC%dp%d",
             path, cwdptr, show_long, show_all, show_one_per_line,
             force_color, force_columns, show_pretty);
    ls_cache_entry_t* cached = NULL;
    if (!piped && !show_pretty && !show_preview && !show_tree && !pattern &&
        !filter_recent_flag && !filter_large_flag) {
        cached = ls_cache_find(ls_ckey);
        if (cached) {
            jash_writes(w, ctx, cached->output);
            return 0;
        }
    }

    /* Collect directory entries */
    ls_file_info_t files[512];
    int nfiles = 0;
    if (ls_collect_dir(path, files, 512, &nfiles, show_all) < 0) {
        jash_writef(jash_stderr_writer, 0, "ls: cannot access '%s'\n", path);
        return 1;
    }
    
    /* AI-001: AI description mode — show descriptions of files */
    if (ai_describe_flag) {
        for (int i = 0; i < nfiles; i++) {
            const char* color = unix_get_file_color(files[i].name, files[i].mode);
            const char* icon = unix_get_file_icon(files[i].name, files[i].mode);
            const char* desc = ls_get_ai_description(files[i].path);
            if (piped) {
                jash_writef(w, ctx, "%s %s\n", files[i].name, desc);
            } else {
                jash_writef(w, ctx, "%s%s%s %s\033[0m\n", icon, color, files[i].name, desc);
            }
        }
        return 0;
    }
    
    /* AI-002: Semantic search mode — filter by content/description */
    if (semantic_search_flag && semantic_query) {
        nfiles = ls_filter_semantic(files, nfiles, semantic_query);
    }
    
    /* AI-003: Inspect mode — show detailed file information */
    if (inspect_flag) {
        for (int i = 0; i < nfiles; i++) {
            const char* color = unix_get_file_color(files[i].name, files[i].mode);
            const char* icon = unix_get_file_icon(files[i].name, files[i].mode);
            if (piped) {
                jash_writef(w, ctx, "%s\n", files[i].name);
            } else {
                jash_writef(w, ctx, "%s%s%s\033[0m\n", icon, color, files[i].name);
                ls_format_inspect_details(w, ctx, files[i].path);
            }
        }
        return 0;
    }
    
    /* AI-004: Relate mode — show related files */
    if (relate_flag && relate_target) {
        jash_writef(w, ctx, "Files related to %s:\n", relate_target);
        ls_find_related_files(w, ctx, relate_target);
        return 0;
    }
    
    /* Sort alphabetically using qsort (O(n log n) vs O(n^2) bubble sort) */
    if (nfiles > 1 && !show_long) {
        /* ls_format_listing also sorts; avoid double-sort for long mode.
         * For filters/preview we sort here; for standard ls we let
         * ls_format_listing handle it. */
    }

    /* Apply filters */
    if (pattern) {
        nfiles = ls_filter_pattern(files, nfiles, pattern);
    }
    if (filter_recent_flag) {
        nfiles = ls_filter_recent(files, nfiles);
    }
    if (filter_large_flag) {
        nfiles = ls_filter_large(files, nfiles, 10);
    }

    /* Format listing via shared helper */
    char* output = show_pretty
        ? ls_format_pretty(files, nfiles, path, piped || unix_is_sensory_mode(),
                           pretty_limit)
        : ls_format_listing(files, nfiles, show_long,
                            show_one_per_line, piped, NULL);
    if (!output) {
        jash_writes(jash_stderr_writer, 0, "ls: out of memory\n");
        return 1;
    }

    /* Preview: show first 3 lines of each file (appended after listing) */
    if (show_preview) {
        for (int i = 0; i < nfiles; i++) {
            ls_file_info_t* f = &files[i];
            if (!f->is_dir) {
                FILE* fp = fopen(f->path, "r");
                if (fp) {
                    jash_writef(w, ctx, "\n%s%s%s:\n",
                                unix_get_file_icon(f->name, f->mode),
                                unix_get_file_color(f->name, f->mode),
                                f->name);
                    char preview_line[256];
                    for (int lc = 0; lc < 3 &&
                         fgets(preview_line, sizeof(preview_line), fp); lc++)
                        jash_writef(w, ctx, "  │ %s", preview_line);
                    fclose(fp);
                }
            }
        }
    }

    /* Update cache */
    if (!piped && !show_pretty && !show_preview && !show_tree && !pattern &&
        !filter_recent_flag && !filter_large_flag) {
        ls_cache_update(ls_ckey, output);   /* BUG-035: key includes flags */
    }

    /* Write output */
    jash_writes(w, ctx, output);
    free(output);

    return 0;
}

int jash_b_ls(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (args && strlen(args) >= LS_ARGS_MAX) {
        jash_writef(jash_stderr_writer, 0,
                    "ls: lista de argumentos longa demais (max %d bytes)\n",
                    LS_ARGS_MAX - 1);
        return 1;
    }
    jash_argv_t parsed = {0};
    if (jash_argv_build(args, &parsed) < 0) {
        jash_writes(jash_stderr_writer, 0, "ls: cannot parse arguments\n");
        return 1;
    }
    int result = jash_b_ls_argv(s, w, ctx, parsed.argc, parsed.argv);
    jash_argv_free(&parsed);
    return result;
}
