// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/fschange — files_changed scanner (ADR-0038 W2).
// See fschange.h for the contract. F E R M I ∞ H A R T

#include "fschange.h"
#include <core/memo.h>   /* W3: read ledger */

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "osif/osif_stat.h"

#define FS_MAX_ENTRIES 30000
#define FS_MAX_DEPTH   8
#define FS_ARENA_CAP   (FS_MAX_ENTRIES * 48)
/* Amortized-growth starting points: snapshots of small trees must not pay
 * the fixed ~2.4MB (entries + arena) allocation per command. */
#define FS_INIT_ENTRIES 1024
#define FS_INIT_ARENA   (64 * 1024)

typedef struct {
    unsigned long h;       /* fnv1a of root-relative path */
    unsigned long size;
    unsigned long mt;      /* mtime ns */
    unsigned      path_off;
    unsigned char seen;    /* diff scratch */
} fsent_t;

struct jash_fssnap {
    fsent_t* e;
    int      n;
    int      cap;
    char*    arena;
    unsigned arena_used;
    unsigned arena_cap;
    int      overflow;
};

static unsigned long fs_fnv(unsigned long h, const char* s) {
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211UL; }
    return h;
}

static int fs_excluded(const char* name) {
    /* BUG-064: build/artifact dirs are BOTH a cost and a noise source — the
     * scanner walked 738 of this repo's 1961 files just in build/ +
     * build-speed/ (2x per command), and `cmake --build` would report
     * hundreds of changed .o the agent never cares about. Exclude the
     * conventional output dirs (git/ripgrep treat them the same). Regenerated
     * artifacts are never the agent's blast radius of interest. */
    static const char* const EX[] = {
        ".git", ".jash", ".cache", ".jash_sessions", "node_modules",
        "build", "target", "dist", "out", "_build", ".next", ".nuxt",
        ".venv", "venv", "__pycache__", ".tox", ".mypy_cache", ".pytest_cache",
        NULL
    };
    for (int i = 0; EX[i]; i++)
        if (strcmp(name, EX[i]) == 0) return 1;
    /* build-*, cmake-build-* (out-of-tree CMake, CLion) */
    if (strncmp(name, "build-", 6) == 0) return 1;
    if (strncmp(name, "cmake-build-", 12) == 0) return 1;
    return 0;
}

static int world_excluded(const char* name) {
    static const char* const EX[] = {
        ".git", "node_modules", ".jash", ".cache", ".svn", ".hg",
        "build", "build-speed", "__pycache__", ".DS_Store", NULL
    };
    for (int i = 0; EX[i]; i++)
        if (strcmp(name, EX[i]) == 0) return 1;
    return 0;
}

static unsigned long fs_ts(const struct timespec* t) {
    return (unsigned long)t->tv_sec * 1000000000UL + (unsigned long)t->tv_nsec;
}

static int fs_dirent_is_dir(const struct dirent* de) {
#if defined(_DIRENT_HAVE_D_TYPE) && defined(DT_DIR) && defined(O_NOFOLLOW)
    return de->d_type == DT_DIR;
#else
    (void)de;
    return 0;
#endif
}

static DIR* fs_open_child_dir(DIR* parent, const char* name, int follow) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    if (!follow) flags |= O_NOFOLLOW;
#else
    (void)follow;
#endif
    int fd = openat(dirfd(parent), name, flags);
    if (fd < 0) return NULL;
    DIR* child = fdopendir(fd);
    if (!child) close(fd);
    return child;
}

/* Double the entry vector, bounded by the FS_MAX_ENTRIES hard cap. */
static int fs_grow_entries(jash_fssnap_t* s) {
    int cap = s->cap ? s->cap * 2 : FS_INIT_ENTRIES;
    if (cap > FS_MAX_ENTRIES) cap = FS_MAX_ENTRIES;
    if (cap <= s->cap) return 0;
    fsent_t* e = (fsent_t*)realloc(s->e, sizeof(fsent_t) * (size_t)cap);
    if (!e) return 0;
    s->e = e;
    s->cap = cap;
    return 1;
}

/* Double the path arena until `need` more bytes fit, bounded by
 * FS_ARENA_CAP. */
static int fs_grow_arena(jash_fssnap_t* s, size_t need) {
    unsigned cap = s->arena_cap ? s->arena_cap : FS_INIT_ARENA;
    while (cap < s->arena_used + need && cap < FS_ARENA_CAP) cap *= 2;
    if (cap > FS_ARENA_CAP) cap = FS_ARENA_CAP;
    if (s->arena_used + need >= cap) return 0;
    char* a = (char*)realloc(s->arena, cap);
    if (!a) return 0;
    s->arena = a;
    s->arena_cap = cap;
    return 1;
}

/* Recursive walk. rel is the root-relative prefix ("" at top). */
static int world_entry_cmp(const void* a, const void* b) {
    return strcmp(((const jash_world_entry_t*)a)->path,
                  ((const jash_world_entry_t*)b)->path);
}

static int world_add(jash_world_scan_t* world, const char* rel,
                     const struct stat* st) {
    if (!world) return 1;
    if (world->count >= JASH_WORLD_MAX_ENTRIES || strlen(rel) >= JASH_WORLD_PATH_MAX) return 0;
    if (world->count == world->capacity) {
        int capacity = world->capacity ? world->capacity * 2 : 64;
        if (capacity > JASH_WORLD_MAX_ENTRIES) capacity = JASH_WORLD_MAX_ENTRIES;
        jash_world_entry_t* entries = (jash_world_entry_t*)realloc(
            world->entries, (size_t)capacity * sizeof(*entries));
        if (!entries) return 0;
        world->entries = entries;
        world->capacity = capacity;
    }
    jash_world_entry_t* entry = &world->entries[world->count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "%s", rel);
    entry->mtime = (long)st->st_mtime;
    entry->size = (long)st->st_size;
    entry->mode = (int)st->st_mode;
    return 1;
}

static void fs_walk(jash_fssnap_t* s, const char* root, size_t root_len, char* rel,
                     size_t rel_len, size_t rel_cap, unsigned long rel_hash,
                     int depth, int fs_active,
                     jash_world_scan_t* world, int world_active,
                     int world_depth, int* world_failed, DIR* opened) {
    if (s->overflow) { if (opened) closedir(opened); return; }
    if (fs_active && depth > FS_MAX_DEPTH) {
        s->overflow = 1;
        if (opened) closedir(opened);
        return;
    }
    if (world_active && depth > world_depth) world_active = 0;
    if (!fs_active && !world_active) { if (opened) closedir(opened); return; }
    /* Retain the exact root + '/' + rel limit even for already-opened dirs. */
    if (root_len + rel_len + 1 >= 1024) {
        s->overflow = 1;
        if (opened) closedir(opened);
        return;
    }
    DIR* d = opened ? opened : opendir(root);
    if (!d) return;
    if (rel_len) rel_hash = fs_fnv(rel_hash, "/");
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        const char* nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        int fs_child = fs_active && !fs_excluded(nm);
        int world_child = world_active && !world_excluded(nm);
        if (!fs_child && !world_child) continue;
        size_t nl = strlen(nm);
        if (rel_len + nl + 2 >= rel_cap) { s->overflow = 1; break; }
        size_t old = rel_len;
        if (rel_len) { rel[rel_len] = '/'; memcpy(rel + rel_len + 1, nm, nl + 1); rel_len += nl + 1; }
        else         { memcpy(rel, nm, nl + 1); rel_len = nl; }
        unsigned long child_hash = fs_fnv(rel_hash, nm);

        /* d_type is enough for a definite directory: neither scanner records
         * directory metadata. Unknown types and symlinks still use fstatat so
         * the existing fail-closed traversal policy remains unchanged. */
        if (fs_dirent_is_dir(de)) {
            DIR* child = fs_open_child_dir(d, nm, 0);
            if (child)
                fs_walk(s, root, root_len, rel, rel_len, rel_cap, child_hash,
                        depth + 1, fs_child,
                        world, world_child, world_depth, world_failed, child);
            rel_len = old;
            rel[rel_len] = '\0';
            if (s->overflow) break;
            continue;
        }

        /* fstatat on the directory fd: no full-path re-resolution per file
         * (the per-command syscall-storm lesson, BUG-053..055). */
        struct stat st;
        if (fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            struct stat world_st = st;
            if (world_child && S_ISLNK(st.st_mode) &&
                fstatat(dirfd(d), nm, &world_st, 0) != 0)
                world_child = 0;
            int fs_dir = fs_child && S_ISDIR(st.st_mode);
            int world_dir = world_child && S_ISDIR(world_st.st_mode);
            if (world_child && !world_dir && S_ISREG(world_st.st_mode) &&
                !*world_failed && !world_add(world, rel, &world_st))
                *world_failed = 1;
            if (fs_child && !fs_dir) {
                if (s->n >= FS_MAX_ENTRIES ||
                    s->arena_used + rel_len + 1 >= FS_ARENA_CAP) {
                    s->overflow = 1;
                } else if ((s->n == s->cap && !fs_grow_entries(s)) ||
                           (s->arena_used + rel_len + 1 >= s->arena_cap &&
                            !fs_grow_arena(s, rel_len + 1))) {
                    s->overflow = 1;
                } else {
                    fsent_t* e = &s->e[s->n++];
                    e->h = child_hash;
                    e->size = (unsigned long)st.st_size;
                    e->mt = fs_ts(&BEAR_ST_MTIM(st));
                    e->path_off = s->arena_used;
                    e->seen = 0;
                    memcpy(s->arena + s->arena_used, rel, rel_len + 1);
                    s->arena_used += (unsigned)rel_len + 1;
                }
            }
            if (fs_dir || world_dir) {
                DIR* child = fs_open_child_dir(d, nm,
                                               world_dir && S_ISLNK(st.st_mode));
                if (child)
                    fs_walk(s, root, root_len, rel, rel_len, rel_cap, child_hash,
                            depth + 1, fs_dir,
                            world, world_dir, world_depth, world_failed, child);
            }
        }
        rel_len = old;
        rel[rel_len] = '\0';
        if (s->overflow) break;
    }
    closedir(d);
}

static int fs_cmp(const void* a, const void* b) {
    unsigned long x = ((const fsent_t*)a)->h, y = ((const fsent_t*)b)->h;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static fsent_t* fs_find(jash_fssnap_t* s, unsigned long h) {
    int lo = 0, hi = s->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s->e[mid].h == h) return &s->e[mid];
        if (s->e[mid].h < h) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static jash_fssnap_t* fssnap_take(const char* root, int world_depth,
                                  jash_world_scan_t** world_out) {
    if (!root || !*root) return NULL;
    if (world_out) *world_out = NULL;
    jash_fssnap_t* s = (jash_fssnap_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->e = (fsent_t*)malloc(sizeof(fsent_t) * FS_INIT_ENTRIES);
    s->arena = (char*)malloc(FS_INIT_ARENA);
    if (!s->e || !s->arena) {
        free(s->e); free(s->arena); free(s);
        return NULL;
    }
    s->cap = FS_INIT_ENTRIES;
    s->arena_cap = FS_INIT_ARENA;
    jash_world_scan_t* world = NULL;
    if (world_out) world = (jash_world_scan_t*)calloc(1, sizeof(*world));
    int world_failed = world_out && !world;
    char rel[1024] = "";
    fs_walk(s, root, strlen(root), rel, 0, sizeof(rel), 1469598103934665603UL,
            0, 1, world, world != NULL, world_depth, &world_failed, NULL);
    if (s->overflow) {
        jash_world_scan_free(world);
        free(s->e); free(s->arena); free(s);
        return NULL;
    }
    qsort(s->e, (size_t)s->n, sizeof(fsent_t), fs_cmp);
    if (world_failed) {
        jash_world_scan_free(world);
    } else if (world) {
        qsort(world->entries, (size_t)world->count, sizeof(*world->entries),
              world_entry_cmp);
        *world_out = world;
    }
    return s;
}

jash_fssnap_t* jash_fssnap_take(const char* root) {
    return fssnap_take(root, 0, NULL);
}

jash_fssnap_t* jash_fssnap_take_with_world(const char* root, int world_depth,
                                           jash_world_scan_t** world_out) {
    if (!world_out) return jash_fssnap_take(root);
    if (world_depth < 1) world_depth = 1;
    if (world_depth > 5) world_depth = 5;
    return fssnap_take(root, world_depth, world_out);
}

/* W3: was the PRE-command version of root/rel different from the version
 * the agent last read? (pre_mt == 0 means "no pre version" — creations.) */
static void stale_check(jash_fsdiff_t* out, const char* root, const char* rel,
                        unsigned long pre_mt) {
    static int guard_off = -1;
    if (guard_off < 0) {
        const char* g = getenv("JASH_STALE_GUARD");
        guard_off = (g && g[0] == '0') ? 1 : 0;
    }
    if (guard_off || pre_mt == 0 || out->n == 0) return;
    char abs[1024];
    if (snprintf(abs, sizeof(abs), "%s/%s", root, rel) >= (int)sizeof(abs))
        return;
    unsigned long read_mt = 0, read_ts = 0;
    if (!jash_memo_ledger_lookup(abs, &read_mt, &read_ts)) return;
    if (read_mt != pre_mt) {
        out->stale_total++;
        for (int i = 0; i < out->n; i++) {
            if (strcmp(out->paths[i], rel) == 0) {
                out->stale[i] = 1;
                out->stale_read_ts[i] = read_ts;
                break;
            }
        }
    }
}

static void diff_add(jash_fsdiff_t* out, char kind, const char* path) {
    out->total++;
    char kept[JASH_FSDIFF_PATH_MAX];
    snprintf(kept, sizeof kept, "%s", path);
    int pos = 0;
    while (pos < out->n && strcmp(out->paths[pos], kept) < 0) pos++;
    if (out->n == JASH_FSDIFF_MAX_REPORT && pos == out->n) return;
    int last = out->n < JASH_FSDIFF_MAX_REPORT ? out->n++ : out->n - 1;
    for (int i = last; i > pos; i--) {
        memcpy(out->paths[i], out->paths[i - 1], JASH_FSDIFF_PATH_MAX);
        out->kind[i] = out->kind[i - 1];
        out->stale[i] = out->stale[i - 1];
        out->stale_read_ts[i] = out->stale_read_ts[i - 1];
    }
    snprintf(out->paths[pos], JASH_FSDIFF_PATH_MAX, "%s", kept);
    out->kind[pos] = kind;
    out->stale[pos] = 0;
    out->stale_read_ts[pos] = 0;
}

/* Second walk for the diff: compare each present file to the snapshot. */
static void fs_diff_walk(jash_fssnap_t* snap, const char* root, size_t root_len,
                         char* rel, size_t rel_len, size_t rel_cap,
                         unsigned long rel_hash, int depth,
                         jash_fsdiff_t* out, int* overflow, DIR* opened) {
    if (*overflow || depth > FS_MAX_DEPTH) {
        *overflow = 1;
        if (opened) closedir(opened);
        return;
    }
    /* Same absolute-path guard as the snapshot walk, without formatting it. */
    if (root_len + rel_len + 1 >= 1024) {
        *overflow = 1;
        if (opened) closedir(opened);
        return;
    }
    DIR* d = opened ? opened : opendir(root);
    if (!d) return;
    if (rel_len) rel_hash = fs_fnv(rel_hash, "/");
    struct dirent* de;
    long walked = 0;
    while ((de = readdir(d)) != NULL) {
        const char* nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (fs_excluded(nm)) continue;
        size_t nl = strlen(nm);
        if (rel_len + nl + 2 >= rel_cap) { *overflow = 1; break; }
        size_t old = rel_len;
        if (rel_len) { rel[rel_len] = '/'; memcpy(rel + rel_len + 1, nm, nl + 1); rel_len += nl + 1; }
        else         { memcpy(rel, nm, nl + 1); rel_len = nl; }
        unsigned long child_hash = fs_fnv(rel_hash, nm);

        if (fs_dirent_is_dir(de)) {
            DIR* child = fs_open_child_dir(d, nm, 0);
            if (child)
                fs_diff_walk(snap, root, root_len, rel, rel_len, rel_cap,
                             child_hash, depth + 1, out, overflow, child);
            rel_len = old;
            rel[rel_len] = '\0';
            if (*overflow) break;
            continue;
        }

        struct stat st;
        if (fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISDIR(st.st_mode)) {
                DIR* child = fs_open_child_dir(d, nm, 0);
                if (child)
                    fs_diff_walk(snap, root, root_len, rel, rel_len, rel_cap,
                                 child_hash, depth + 1, out, overflow, child);
            } else {
                walked++;
                if (walked > FS_MAX_ENTRIES) { *overflow = 1; }
                else {
                    fsent_t* e = fs_find(snap, child_hash);
                    if (!e) diff_add(out, '+', rel);
                    else {
                        e->seen = 1;
                        if (e->size != (unsigned long)st.st_size ||
                            e->mt != fs_ts(&BEAR_ST_MTIM(st))) {
                            diff_add(out, '~', rel);
                            stale_check(out, root, rel, e->mt);
                        }
                    }
                }
            }
        }
        rel_len = old;
        rel[rel_len] = '\0';
        if (*overflow) break;
    }
    closedir(d);
}

void jash_fssnap_diff(jash_fssnap_t* snap, const char* root, jash_fsdiff_t* out) {
    memset(out, 0, sizeof(*out));
    if (!snap) return;
    if (!root || !*root) { free(snap->e); free(snap->arena); free(snap); return; }

    int overflow = 0;
    char rel[1024] = "";
    fs_diff_walk(snap, root, strlen(root), rel, 0, sizeof(rel),
                 1469598103934665603UL, 0, out, &overflow, NULL);
    if (!overflow) {
        for (int i = 0; i < snap->n; i++)
            if (!snap->e[i].seen) {
                diff_add(out, '-', snap->arena + snap->e[i].path_off);
                stale_check(out, root, snap->arena + snap->e[i].path_off,
                            snap->e[i].mt);
            }
        out->valid = 1;
    }
    free(snap->e);
    free(snap->arena);
    free(snap);
}

/* ── W5: persist the delta for the impact map ───────────────────────────── */

#define FSJOURNAL_TRIM_AT   6000
#define FSJOURNAL_KEEP      4000

void jash_fsdiff_persist(const jash_fsdiff_t* fsd) {
    if (!fsd || !fsd->valid || fsd->n == 0) return;
    const char* home = getenv("HOME");
    if (!home || !*home) return;
    const char* sid = getenv("CLAUDE_CODE_SESSION_ID");
    if (!sid || !*sid) sid = getenv("JASH_SESSION");
    if (!sid || !*sid) sid = "-";

    char path[600];
    snprintf(path, sizeof(path), "%s/.jash/fsjournal.tsv", home);
    FILE* f = fopen(path, "a");
    if (!f) return;
    unsigned long now = (unsigned long)time(NULL);
    for (int i = 0; i < fsd->n; i++)
        fprintf(f, "%lu\t%s\t%c\t%s\n", now, sid, fsd->kind[i], fsd->paths[i]);
    long sz = ftell(f);
    fclose(f);

    /* Trim: keep the newest FSJOURNAL_KEEP lines when the file grows past
     * the threshold (~line = 64B; check cheaply by size). */
    if (sz > FSJOURNAL_TRIM_AT * 64) {
        FILE* in = fopen(path, "r");
        if (!in) return;
        char** lines = (char**)malloc(sizeof(char*) * FSJOURNAL_TRIM_AT);
        if (!lines) { fclose(in); return; }
        int n = 0;
        char buf[1024];
        while (fgets(buf, sizeof(buf), in)) {
            if (n == FSJOURNAL_TRIM_AT) {
                free(lines[0]);
                memmove(lines, lines + 1, sizeof(char*) * (FSJOURNAL_TRIM_AT - 1));
                n--;
            }
            lines[n] = strdup(buf);
            if (lines[n]) n++;
        }
        fclose(in);
        char tmp[640];
        snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
        FILE* o = fopen(tmp, "w");
        if (o) {
            int start = n > FSJOURNAL_KEEP ? n - FSJOURNAL_KEEP : 0;
            for (int i = start; i < n; i++) fputs(lines[i], o);
            fclose(o);
            rename(tmp, path);
        }
        for (int i = 0; i < n; i++) free(lines[i]);
        free(lines);
    }
}

/* ── pure-command gate ──────────────────────────────────────────────────── */

/* Names whose SIMPLE invocation cannot change the filesystem. Read-only
 * externals share the W1 memo whitelist philosophy; pure builtins added. */
static const char* const FS_PURE[] = {
    "cat", "head", "tail", "wc", "nl", "stat", "file", "cmp", "diff",
    "grep", "egrep", "fgrep", "rg", "ls", "echo", "printf", "pwd", "true",
    "false", "test", "[", "type", "which", "recall", "state", "expect",
    "help", "history", "alias", "env", "date", "whoami", "id", "uname",
    "basename", "dirname", "realpath", "readlink", "sortby", "uniqby",
    "where", "pick", "count", "sum", "avg", "describe", "agent-guide",
    NULL
};

int jash_fschange_command_is_pure(const char* command_str) {
    if (!command_str) return 1;
    const char* p = command_str;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 1;
    /* Any compound/redirect/expansion machinery -> assume impure. */
    for (const char* q = p; *q; q++) {
        char c = *q;
        if (c == ';' || c == '|' || c == '&' || c == '>' || c == '<' ||
            c == '`' || c == '\n')
            return 0;
        if (c == '$' && q[1] == '(') return 0;
    }
    char name[64];
    unsigned i = 0;
    while (*p && *p != ' ' && *p != '\t' && i + 1 < sizeof(name))
        name[i++] = *p++;
    name[i] = '\0';
    for (int k = 0; FS_PURE[k]; k++)
        if (strcmp(FS_PURE[k], name) == 0) return 1;
    return 0;
}
