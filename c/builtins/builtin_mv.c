// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/mv — disruptive mv builtin with batch operations,
// atomic moves, and conflict resolution.
//
// Features:
//   mv file1 file2 ... dir/    — batch move (atomic)
//   mv --atomic src/ dst/      — all-or-nothing (rollback on failure)
//   mv --resolve=keep-new a b  — auto-resolve conflicts (keep-new, keep-old, skip)
//   mv --dry-run src dst       — show what would be moved
//
// Batch: moves multiple files in one operation (optimized)
// Atomic: if any move fails, rollback all previous moves
// Conflict Resolution: auto-handle destination exists scenarios

#include "builtin.h"
#include <core/timeline.h>
#include <core/env.h>
#include <core/guard.h>      /* ADR-0042 F9: mesh file intent-lock check */
#include <core/mutation_report.h>
#include <osif/osinfo.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

/* ── Conflict resolution modes ────────────────────────────────────────────── */

#define RESOLVE_ASK      0
#define RESOLVE_KEEP_NEW 1
#define RESOLVE_KEEP_OLD 2
#define RESOLVE_SKIP     3
#define RESOLVE_PROMPT   4   /* explicit -i: prompt even on a pipe/EOF */

#define MV_MAX_OPS 256

/* ── Conflict resolution ──────────────────────────────────────────────────── */

static int mv_resolve_conflict(const char* dst, int resolve_mode,
                                jash_writer_t w, void* ctx) {
    if (resolve_mode == RESOLVE_ASK || resolve_mode == RESOLVE_PROMPT) {
        /* BUG-061: the ask-by-default safety net is an INTERACTIVE
         * differential. Non-interactive (scripts, agents, pipes) must match
         * POSIX mv: overwrite silently. The old code fgets()'d a /dev/null
         * or pipe stdin, got NULL, and silently SKIPPED the move — scripts
         * kept the stale target and even consumed pipeline stdin when there
         * was one. Found by W1 memoization refusing to lie about it. */
        if (resolve_mode == RESOLVE_ASK && !isatty(STDIN_FILENO)) return 1;
        /* Interactive: ask user */
        jash_writef(jash_stderr_writer, NULL, "mv: '%s' exists. Overwrite? [y/N] ", dst);
        fflush(stderr);
        char response[16];
        if (fgets(response, sizeof(response), stdin)) {
            return (response[0] == 'y' || response[0] == 'Y') ? 1 : 0;
        }
        return 0;
    }
    
    if (resolve_mode == RESOLVE_KEEP_NEW) {
        /* Overwrite destination */
        return 1;
    }
    
    if (resolve_mode == RESOLVE_KEEP_OLD) {
        /* Keep destination, skip move */
        return 0;
    }
    
    if (resolve_mode == RESOLVE_SKIP) {
        /* Skip this file */
        return 0;
    }
    
    return 0;
}

/* ── Core move logic ──────────────────────────────────────────────────────── */

static int mv_same_identity(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

#define MV_PATH_SIZE 4096

typedef struct {
    int parent_fd;
    int hold_fd;
    char name[MV_PATH_SIZE];
    char hold_name[64];
} mv_hold_t;

static unsigned long mv_hold_serial;

static int mv_link_noreplace_move(int old_fd, const char* old_name,
                                  int new_fd, const char* new_name) {
    struct stat held, published;
    if (fstatat(old_fd, old_name, &held, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (linkat(old_fd, old_name, new_fd, new_name, 0) != 0) return -1;
    if (unlinkat(old_fd, old_name, 0) == 0) return 0;

    int cleanup_errno = errno;
    if (fstatat(new_fd, new_name, &published, AT_SYMLINK_NOFOLLOW) == 0 &&
        mv_same_identity(&held, &published) &&
        unlinkat(new_fd, new_name, 0) == 0) {
        errno = cleanup_errno;
    } else {
        errno = ESTALE;
    }
    return -1;
}

static void mv_hold_close(mv_hold_t* h, int empty) {
    if (h->hold_fd >= 0) {
        if (empty) {
            struct stat opened, named;
            if (fstat(h->hold_fd, &opened) == 0 &&
                fstatat(h->parent_fd, h->hold_name, &named,
                        AT_SYMLINK_NOFOLLOW) == 0 &&
                mv_same_identity(&opened, &named))
                (void)unlinkat(h->parent_fd, h->hold_name, AT_REMOVEDIR);
        }
        close(h->hold_fd);
    }
    if (h->parent_fd >= 0) close(h->parent_fd);
}

static int mv_parent_open(const char* path, char* name, size_t name_cap) {
    char parent[MV_PATH_SIZE];
    size_t len = strlen(path);
    const char* slash;
    while (len > 1 && path[len - 1] == '/') len--;
    slash = NULL;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') slash = path + i;
    if (!slash) {
        strlcpy(parent, ".", sizeof(parent));
        if (len >= name_cap) { errno = ENAMETOOLONG; return -1; }
        memcpy(name, path, len);
        name[len] = '\0';
    } else {
        size_t parent_len = (slash == path) ? 1U : (size_t)(slash - path);
        size_t name_len = len - (size_t)(slash - path) - 1U;
        if (parent_len >= sizeof(parent) || name_len == 0 ||
            name_len >= name_cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
        memcpy(name, slash + 1, name_len);
        name[name_len] = '\0';
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    int parent_flags = O_RDONLY;
#ifdef O_DIRECTORY
    parent_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    parent_flags |= O_CLOEXEC;
#endif
    return open(parent, parent_flags);
}

static int mv_hold_open_at(int parent_fd, const char* name, mv_hold_t* h) {
    h->parent_fd = dup(parent_fd);
    h->hold_fd = -1;
    if (h->parent_fd < 0 || strlen(name) >= sizeof(h->name)) {
        if (h->parent_fd >= 0) close(h->parent_fd);
        h->parent_fd = -1;
        errno = strlen(name) >= sizeof(h->name) ? ENAMETOOLONG : errno;
        return -1;
    }
    strlcpy(h->name, name, sizeof(h->name));

    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        unsigned long serial = ++mv_hold_serial;
        snprintf(h->hold_name, sizeof(h->hold_name), ".jash-mv-hold-%ld-%lu",
                 (long)getpid(), serial);
        if (mkdirat(h->parent_fd, h->hold_name, 0700) == 0) break;
        if (errno != EEXIST || attempt == 127) {
            mv_hold_close(h, 0);
            return -1;
        }
    }

    int hold_flags = O_RDONLY;
#ifdef O_DIRECTORY
    hold_flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    hold_flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    hold_flags |= O_CLOEXEC;
#endif
    h->hold_fd = openat(h->parent_fd, h->hold_name, hold_flags);
    if (h->hold_fd < 0 || fchmod(h->hold_fd, 0700) != 0) {
        if (h->hold_fd >= 0) close(h->hold_fd);
        h->hold_fd = -1;
        (void)unlinkat(h->parent_fd, h->hold_name, AT_REMOVEDIR);
        mv_hold_close(h, 0);
        return -1;
    }
    return 0;
}

static int mv_hold_open(const char* path, mv_hold_t* h) {
    char name[MV_PATH_SIZE];
    int parent_fd = mv_parent_open(path, name, sizeof(name));
    if (parent_fd < 0) return -1;
    int rc = mv_hold_open_at(parent_fd, name, h);
    close(parent_fd);
    return rc;
}

static int mv_restore_noreplace(int old_fd, const char* old_name,
                                int new_fd, const char* new_name) {
#if defined(__APPLE__) && defined(RENAME_EXCL)
    if (renameatx_np(old_fd, old_name, new_fd, new_name, RENAME_EXCL) == 0)
        return 0;
    if (errno != ENOTSUP && errno != ENOSYS) return -1;
#endif
    struct stat held;
    if (fstatat(old_fd, old_name, &held, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (S_ISDIR(held.st_mode)) { errno = ENOTSUP; return -1; }
    /* linkat is an atomic no-clobber create. The held name is inside our
     * private directory, so removing it after the link cannot be redirected.
     * With flags=0 linkat links the inode itself, including symlinks, FIFOs,
     * sockets and devices, rather than following a symlink target. */
    return mv_link_noreplace_move(old_fd, old_name, new_fd, new_name);
}

typedef struct {
    mv_hold_t hold;
    char retained_path[MV_PATH_SIZE];
    int active;
} mv_capture_t;

static void mv_retained_path(const char* endpoint, const char* hold_name,
                             const char* item, char* out, size_t out_size) {
    char absolute[MV_PATH_SIZE];
    if (endpoint[0] != '/') {
        char cwd[MV_PATH_SIZE];
        if (getcwd(cwd, sizeof(cwd))) {
            int n = snprintf(absolute, sizeof(absolute), "%s/%s", cwd, endpoint);
            if (n < 0 || (size_t)n >= sizeof(absolute)) { out[0] = 0; return; }
        }
        else
            snprintf(absolute, sizeof(absolute), "%s", endpoint);
        endpoint = absolute;
    }
    char parent[MV_PATH_SIZE];
    const char* slash = strrchr(endpoint, '/');
    if (!slash) {
        if (!getcwd(parent, sizeof(parent))) strlcpy(parent, ".", sizeof(parent));
    } else if (slash == endpoint) {
        strlcpy(parent, "/", sizeof(parent));
    } else {
        size_t len = (size_t)(slash - endpoint);
        if (len >= sizeof(parent)) {
            out[0] = '\0';
            return;
        }
        memcpy(parent, endpoint, len);
        parent[len] = '\0';
    }
    int n = snprintf(out, out_size, "%s%s%s/%s", parent,
                     strcmp(parent, "/") ? "/" : "", hold_name, item);
    if (n < 0 || (size_t)n >= out_size) out[0] = 0;
}

static void mv_capture_set_path(mv_capture_t* capture, const char* endpoint) {
    mv_retained_path(endpoint, capture->hold.hold_name, "item",
                     capture->retained_path, sizeof(capture->retained_path));
}

static int mv_capture_identity_at(int parent_fd, const char* name,
                                  const struct stat* expected,
                                  mv_capture_t* capture);

static int mv_capture_identity(const char* path, const struct stat* expected,
                               mv_capture_t* capture) {
    char name[MV_PATH_SIZE];
    int parent_fd = mv_parent_open(path, name, sizeof(name));
    if (parent_fd < 0) return -1;
    int rc = mv_capture_identity_at(parent_fd, name, expected, capture);
    close(parent_fd);
    if (rc == 0 || capture->active) mv_capture_set_path(capture, path);
    return rc;
}

static int mv_capture_identity_at(int parent_fd, const char* name,
                                  const struct stat* expected,
                                  mv_capture_t* capture) {
    static const char item[] = "item";
    struct stat captured;
    capture->active = 0;
    if (mv_hold_open_at(parent_fd, name, &capture->hold) != 0) return -1;
    if (renameat(capture->hold.parent_fd, capture->hold.name,
                 capture->hold.hold_fd, item) != 0) {
        mv_hold_close(&capture->hold, 1);
        return -1;
    }
    if (fstatat(capture->hold.hold_fd, item, &captured,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !mv_same_identity(&captured, expected)) {
        int restored = mv_restore_noreplace(capture->hold.hold_fd, item,
                                             capture->hold.parent_fd,
                                             capture->hold.name) == 0;
        if (restored) mv_hold_close(&capture->hold, 1);
        else capture->active = 1;
        errno = ESTALE;
        return -1;
    }
    if (fsync(capture->hold.hold_fd) != 0 ||
        fsync(capture->hold.parent_fd) != 0) {
        int saved = errno;
        int restored = mv_restore_noreplace(capture->hold.hold_fd, item,
                                             capture->hold.parent_fd,
                                             capture->hold.name) == 0;
        if (restored) mv_hold_close(&capture->hold, 1);
        else capture->active = 1;
        errno = saved;
        return -1;
    }
    capture->active = 1;
    return 0;
}

static int mv_capture_restore(mv_capture_t* capture) {
    if (!capture->active) return 0;
    int restored = mv_restore_noreplace(capture->hold.hold_fd, "item",
                                         capture->hold.parent_fd,
                                         capture->hold.name) == 0;
    int durable = restored && fsync(capture->hold.parent_fd) == 0;
    mv_hold_close(&capture->hold, restored);
    capture->active = 0;
    if (!restored) return -1;  /* The item remains at retained_path. */
    return durable ? 0 : 1;
}

static int mv_capture_delete(mv_capture_t* capture) {
    if (!capture->active) return 0;
    if (unlinkat(capture->hold.hold_fd, "item", 0) != 0)
        return -1; /* Caller may still restore the retained capture. */
    int durable = fsync(capture->hold.hold_fd) == 0 &&
                  fsync(capture->hold.parent_fd) == 0;
    mv_hold_close(&capture->hold, 1);
    capture->active = 0;
    return durable ? 0 : 1;
}

static void mv_capture_abandon(mv_capture_t* capture) {
    if (!capture->active) return;
    mv_hold_close(&capture->hold, 0);
    capture->active = 0;
}

static int mv_capture_restore_report(mv_capture_t* capture, const char* what,
                                     jash_writer_t w, void* ctx) {
    int rc = mv_capture_restore(capture);
    if (rc < 0)
        jash_writef(w, ctx, "mv: %s retained at '%s'\n",
                    what, capture->retained_path);
    else if (rc > 0)
        jash_writef(w, ctx, "mv: %s restored but directory sync failed\n",
                    what);
    return rc;
}

/* ── Move operation tracking (for atomic rollback) ────────────────────────── */

typedef struct {
    char src[512];
    char dst[512];
    int src_parent_fd;
    int dst_parent_fd;
    char src_name[MV_PATH_SIZE];
    char dst_name[MV_PATH_SIZE];
    struct stat moved;
    mv_capture_t previous;
    int completed;
    jash_mutation_item_t* result;
} mv_operation_t;

static mv_operation_t *g_mv_ops;
static int g_mv_ops_count;

static int mv_publish_noreplace(int old_fd, const char* old_name,
                                int new_fd, const char* new_name);

static int mv_ops_ensure(void) {
    if (g_mv_ops) return 0;
    g_mv_ops = (mv_operation_t*)calloc(MV_MAX_OPS, sizeof(mv_operation_t));
    return g_mv_ops ? 0 : -1;
}

static void mv_item_recovery(jash_mutation_item_t* item, const char* path) {
    /* INV-JASH-TIER1-SOURCE-001: retain the recovery buffer's explicit bound. */
    if (item && item->recovery_path && path && strlen(path) < item->recovery_capacity)
        memcpy(item->recovery_path, path, strlen(path) + 1);
}

static int mv_journal_add(const char* src, const char* dst,
                          int src_parent_fd, const char* src_name,
                          int dst_parent_fd, const char* dst_name,
                          const struct stat* moved,
                          mv_capture_t* previous, jash_mutation_item_t* result) {
    if (g_mv_ops_count >= MV_MAX_OPS || strlen(src) >= sizeof(g_mv_ops[0].src) ||
        strlen(dst) >= sizeof(g_mv_ops[0].dst) ||
        strlen(src_name) >= sizeof(g_mv_ops[0].src_name) ||
        strlen(dst_name) >= sizeof(g_mv_ops[0].dst_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    mv_operation_t* op = &g_mv_ops[g_mv_ops_count++];
    strlcpy(op->src, src, sizeof(op->src));
    strlcpy(op->dst, dst, sizeof(op->dst));
    op->src_parent_fd = src_parent_fd;
    op->dst_parent_fd = dst_parent_fd;
    strlcpy(op->src_name, src_name, sizeof(op->src_name));
    strlcpy(op->dst_name, dst_name, sizeof(op->dst_name));
    op->moved = *moved;
    if (previous) op->previous = *previous;
    else memset(&op->previous, 0, sizeof(op->previous));
    op->completed = 1;
    op->result = result;
    return 0;
}

static void mv_result_note(mv_operation_t* op, const char* code, int retain) {
    if (!op->result) return;
    op->result->status = JASH_MUT_PARTIAL;
    op->result->effect = JASH_MUT_CHANGED;
    op->result->code = code;
    if (retain && op->result->recovery_path && op->previous.retained_path[0] &&
        strlen(op->previous.retained_path) < op->result->recovery_capacity)
        memcpy(op->result->recovery_path, op->previous.retained_path,
               strlen(op->previous.retained_path) + 1);
}

static int mv_rollback(jash_writer_t w, void* ctx) {
    int failed = 0;
    jash_writes(w, ctx, "mv: rolling back (atomic mode)...\n");
    for (int i = g_mv_ops_count - 1; i >= 0; i--) {
        mv_operation_t* op = &g_mv_ops[i];
        if (!op->completed) continue;
        struct stat current;
        int source_absent = fstatat(op->src_parent_fd, op->src_name, &current,
                                    AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
        int destination_owned =
            fstatat(op->dst_parent_fd, op->dst_name, &current,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            mv_same_identity(&current, &op->moved);
        if (!source_absent || !destination_owned ||
            mv_publish_noreplace(op->dst_parent_fd, op->dst_name,
                                 op->src_parent_fd, op->src_name) != 0 ||
            fstatat(op->src_parent_fd, op->src_name, &current,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !mv_same_identity(&current, &op->moved)) {
            jash_writef(w, ctx, "mv: rollback FAILED for '%s'\n", op->src);
            failed = 1; mv_result_note(op, "rollback_failed", 1);
            if (op->previous.active) {
                jash_writef(w, ctx,
                            "mv: prior destination retained for '%s' at '%s'\n",
                            op->dst, op->previous.retained_path);
                mv_capture_abandon(&op->previous);
            }
            close(op->src_parent_fd);
            close(op->dst_parent_fd);
            continue;
        }
        int restore_rc = 0;
        if (op->previous.active) {
            restore_rc = mv_capture_restore(&op->previous);
            if (restore_rc < 0)
                jash_writef(w, ctx,
                            "mv: rollback destination recovery retained for '%s' at '%s'\n",
                            op->dst, op->previous.retained_path);
            else if (restore_rc > 0)
                jash_writef(w, ctx,
                            "mv: rollback restored '%s' but directory sync failed\n",
                            op->dst);
        }
        int synced = fsync(op->src_parent_fd) == 0;
        if (op->dst_parent_fd != op->src_parent_fd)
            if (fsync(op->dst_parent_fd)) synced = 0;
        if (restore_rc || !synced) failed = 1;
        mv_result_note(op, restore_rc < 0 ? "rollback_failed" : restore_rc || !synced ? "rollback_sync_failed" : "rolled_back", restore_rc < 0);
        close(op->src_parent_fd);
        close(op->dst_parent_fd);
        op->completed = 0;
    }
    g_mv_ops_count = 0;
    return failed ? -1 : 0;
}

static int mv_commit(jash_writer_t w, void* ctx) {
    int failed = 0;
    for (int i = 0; i < g_mv_ops_count; i++) {
        int src_sync = fsync(g_mv_ops[i].src_parent_fd);
        int dst_sync = fsync(g_mv_ops[i].dst_parent_fd);
        if (src_sync != 0 || dst_sync != 0) {
            jash_writef(w, ctx,
                        "mv: committed but directory sync failed for '%s'\n",
                        g_mv_ops[i].dst);
            failed = 1;
        }
    }
    if (failed) {
        /* No rollback copy is disposable until every operation in the atomic
         * batch has crossed its durability barrier. */
        for (int i = 0; i < g_mv_ops_count; i++) {
            mv_result_note(&g_mv_ops[i], "commit_sync_failed", 1);
            if (g_mv_ops[i].previous.active) {
                jash_writef(w, ctx,
                            "mv: prior destination retained for '%s' at '%s'\n",
                            g_mv_ops[i].dst,
                            g_mv_ops[i].previous.retained_path);
                mv_capture_abandon(&g_mv_ops[i].previous);
            }
            close(g_mv_ops[i].src_parent_fd);
            close(g_mv_ops[i].dst_parent_fd);
        }
        g_mv_ops_count = 0;
        return -1;
    }
    for (int i = 0; i < g_mv_ops_count; i++) {
        if (g_mv_ops[i].previous.active) {
            int delete_rc = mv_capture_delete(&g_mv_ops[i].previous);
            if (delete_rc < 0) {
                mv_result_note(&g_mv_ops[i], "commit_cleanup_failed", 1);
                jash_writef(w, ctx,
                            "mv: committed but destination cleanup for '%s' is retained at '%s'\n",
                            g_mv_ops[i].dst,
                            g_mv_ops[i].previous.retained_path);
                mv_capture_abandon(&g_mv_ops[i].previous);
                failed = 1;
            } else if (delete_rc > 0) {
                mv_result_note(&g_mv_ops[i], "commit_cleanup_sync_failed", 0);
                jash_writef(w, ctx,
                            "mv: committed but destination cleanup sync failed for '%s'\n",
                            g_mv_ops[i].dst);
                failed = 1;
            }
        }
        close(g_mv_ops[i].src_parent_fd);
        close(g_mv_ops[i].dst_parent_fd);
    }
    g_mv_ops_count = 0;
    return failed ? -1 : 0;
}

static int mv_exchange_at(int left_fd, const char* left,
                          int right_fd, const char* right) {
#if defined(__linux__)
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif
    return jash_osif_renameat2(left_fd, left, right_fd, right, RENAME_EXCHANGE);
#elif defined(__APPLE__)
    return renameatx_np(left_fd, left, right_fd, right, RENAME_SWAP);
#else
    (void)left_fd; (void)left; (void)right_fd; (void)right;
    errno = ENOTSUP;
    return -1;
#endif
}

static int mv_publish_noreplace(int old_fd, const char* old_name,
                                int new_fd, const char* new_name) {
#if defined(__linux__)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
    return jash_osif_renameat2(old_fd, old_name, new_fd, new_name, RENAME_NOREPLACE);
#elif defined(__APPLE__)
    return renameatx_np(old_fd, old_name, new_fd, new_name, RENAME_EXCL);
#else
    return mv_link_noreplace_move(old_fd, old_name, new_fd, new_name);
#endif
}

static int mv_keeps_existing(int resolve_mode) {
    return resolve_mode == RESOLVE_KEEP_OLD || resolve_mode == RESOLVE_SKIP;
}

static int mv_rollback_publish(mv_hold_t* destination, int had_previous,
                               const struct stat* copied) {
    struct stat captured;
    if (had_previous) {
        if (mv_exchange_at(destination->hold_fd, "copy",
                           destination->parent_fd, destination->name) != 0)
            return -1;
        if (fstatat(destination->hold_fd, "copy", &captured,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
            !mv_same_identity(&captured, copied))
            return -1;
        return unlinkat(destination->hold_fd, "copy", 0);
    }

    if (renameat(destination->parent_fd, destination->name,
                 destination->hold_fd, "copy") != 0)
        return -1;
    if (fstatat(destination->hold_fd, "copy", &captured,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !mv_same_identity(&captured, copied)) {
        (void)mv_restore_noreplace(destination->hold_fd, "copy",
                                   destination->parent_fd, destination->name);
        errno = ESTALE;
        return -1;
    }
    return unlinkat(destination->hold_fd, "copy", 0);
}

static void mv_possible(jash_mutation_item_t* item) {
    if (item && item->effect == JASH_MUT_NONE) item->effect = JASH_MUT_POSSIBLE;
}

#define MV_COPY_ERROR   (-1)
#define MV_COPY_DONE      0
#define MV_COPY_SKIPPED   1

/* Copy a regular file src→dst preserving mode + timestamps, then remove src
 * through an inode-verified private hold. Used only for EXDEV fallback. */
static int mv_copy_regular(const char* src, const char* dst, int no_replace,
                           jash_writer_t w, void* ctx, jash_mutation_item_t* item) {
    int in_flags = O_RDONLY;
#ifdef O_NOFOLLOW
    in_flags |= O_NOFOLLOW;
#endif
    int in = open(src, in_flags);
    if (in < 0) return -1;
    struct stat source;
    if (fstat(in, &source) != 0 || !S_ISREG(source.st_mode)) {
        close(in);
        errno = EINVAL;
        return -1;
    }
    mv_hold_t destination;
    if (mv_hold_open(dst, &destination) != 0) { close(in); return -1; }

    struct stat previous;
    int had_previous = fstatat(destination.parent_fd, destination.name,
                               &previous, AT_SYMLINK_NOFOLLOW) == 0;
    if (!had_previous && errno != ENOENT) {
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_ERROR;
    }
    if (had_previous && no_replace) {
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_SKIPPED;
    }

    int out_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    out_flags |= O_NOFOLLOW;
#endif
    int out = openat(destination.hold_fd, "copy", out_flags, 0600);
    if (out < 0) {
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_ERROR;
    }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    struct stat copied;
    int have_copied_identity = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (jash_mutation_cancel_status()) { rc = -1; break; }
        ssize_t off = 0;
        while (off < n) {
            ssize_t wn = write(out, buf + off, (size_t)(n - off));
            if (wn <= 0) { rc = -1; break; }
            off += wn;
            if (item) item->bytes_written += (uint64_t)wn;
        }
        if (rc) break;
    }
    if (n < 0 || jash_mutation_cancel_status()) rc = -1;
    if (rc == 0) {
        struct timespec tv[2];
#ifdef __APPLE__
        tv[0] = source.st_atimespec; tv[1] = source.st_mtimespec;
#else
        tv[0] = source.st_atim; tv[1] = source.st_mtim;
#endif
        /* Descriptor operations cannot be redirected to a replacement path.
         * Set-ID bits are safe only while their user/group meaning is intact. */
        if (futimens(out, tv) != 0 || fstat(out, &copied) != 0) {
            rc = -1;
        } else {
            have_copied_identity = 1;
            mode_t copied_mode = (source.st_mode & 01777);
            if (copied.st_uid == source.st_uid) copied_mode |= source.st_mode & S_ISUID;
            if (copied.st_gid == source.st_gid) copied_mode |= source.st_mode & S_ISGID;
            if (fchmod(out, copied_mode) != 0 || fsync(out) != 0) rc = -1;
        }
    }
    if (!have_copied_identity && fstat(out, &copied) == 0)
        have_copied_identity = 1;
    if (close(out) != 0) rc = -1;
    if (rc != 0 || !have_copied_identity || jash_mutation_cancel_status()) {
        (void)unlinkat(destination.hold_fd, "copy", 0);
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_ERROR;
    }

    mv_capture_t source_capture;
    mv_possible(item);
    if (mv_capture_identity(src, &source, &source_capture) != 0) {
        if (source_capture.active) {
            jash_writef(w, ctx,
                        "mv: source recovery retained at '%s'\n",
                        source_capture.retained_path);
            mv_capture_abandon(&source_capture);
        }
        (void)unlinkat(destination.hold_fd, "copy", 0);
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_ERROR;
    }

    int published;
    int copy_in_hold = 1;
    if (had_previous) {
        published = mv_exchange_at(destination.hold_fd, "copy",
                                   destination.parent_fd, destination.name);
        if (published == 0) {
            copy_in_hold = 0;
            struct stat exchanged;
            if (fstatat(destination.hold_fd, "copy", &exchanged,
                        AT_SYMLINK_NOFOLLOW) != 0 ||
                !mv_same_identity(&exchanged, &previous)) {
                if (mv_exchange_at(destination.hold_fd, "copy",
                                   destination.parent_fd,
                                   destination.name) == 0)
                    copy_in_hold = 1;
                published = -1;
                errno = ESTALE;
            }
        }
    } else {
        published = mv_publish_noreplace(destination.hold_fd, "copy",
                                         destination.parent_fd,
                                         destination.name);
        if (published == 0) copy_in_hold = 0;
    }
    if (published != 0) {
        int publish_errno = errno;
        int source_restored = mv_capture_restore_report(
            &source_capture, "source recovery", w, ctx) == 0;
        if (copy_in_hold) (void)unlinkat(destination.hold_fd, "copy", 0);
        mv_hold_close(&destination, copy_in_hold);
        close(in);
        errno = publish_errno;
        if (source_restored && no_replace &&
            (errno == EEXIST || errno == ENOTEMPTY))
            return MV_COPY_SKIPPED;
        return MV_COPY_ERROR;
    }

    struct stat named_dst;
    if (fstatat(destination.parent_fd, destination.name, &named_dst,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !mv_same_identity(&named_dst, &copied)) {
        (void)mv_rollback_publish(&destination, had_previous, &copied);
        (void)mv_capture_restore_report(&source_capture, "source recovery",
                                        w, ctx);
        mv_hold_close(&destination, 1);
        close(in);
        errno = ESTALE;
        return MV_COPY_ERROR;
    }

    /* The destination name and, for overwrite, the exchanged prior name must
     * survive a crash before the only source name is removed. */
    if (fsync(destination.parent_fd) != 0 ||
        (had_previous && fsync(destination.hold_fd) != 0)) {
        int saved = errno;
        (void)mv_rollback_publish(&destination, had_previous, &copied);
        (void)mv_capture_restore_report(&source_capture, "source recovery",
                                        w, ctx);
        (void)fsync(destination.parent_fd);
        (void)fsync(destination.hold_fd);
        mv_hold_close(&destination, 1);
        close(in);
        errno = saved;
        return MV_COPY_ERROR;
    }
    int source_delete_rc = mv_capture_delete(&source_capture);
    if (source_delete_rc < 0) {
        (void)mv_rollback_publish(&destination, had_previous, &copied);
        (void)mv_capture_restore_report(&source_capture, "source recovery",
                                        w, ctx);
        mv_hold_close(&destination, 1);
        close(in);
        errno = ESTALE;
        return MV_COPY_ERROR;
    }
    if (source_delete_rc > 0) {
        /* unlink succeeded but its directory sync failed. The already-durable
         * destination is now the only reliable data name, so never roll it
         * back; report failure and retain any prior destination exactly. */
        if (had_previous) {
            char retained[MV_PATH_SIZE];
            mv_retained_path(dst, destination.hold_name, "copy",
                             retained, sizeof(retained));
            jash_writef(w, ctx,
                        "mv: source deletion durability failed; prior destination retained at '%s'\n",
                        retained);
            mv_hold_close(&destination, 0);
        } else {
            mv_hold_close(&destination, 1);
        }
        close(in);
        errno = EIO;
        return MV_COPY_ERROR;
    }

    if (had_previous && unlinkat(destination.hold_fd, "copy", 0) != 0) {
        char retained[MV_PATH_SIZE];
        mv_retained_path(dst, destination.hold_name, "copy",
                         retained, sizeof(retained));
        jash_writef(w, ctx,
                    "mv: prior destination cleanup retained at '%s'\n",
                    retained);
        mv_hold_close(&destination, 0);
        close(in);
        return MV_COPY_ERROR;
    }
    if (had_previous && fsync(destination.hold_fd) != 0) {
        mv_hold_close(&destination, 1);
        close(in);
        return MV_COPY_ERROR;
    }
    mv_hold_close(&destination, 1);
    close(in);
    return MV_COPY_DONE;
}

static int mv_move_file(const char* src, const char* dst, int resolve_mode,
                         int dry_run, int atomic, int atomic_multi,
                         jash_writer_t w, void* ctx, jash_mutation_item_t* item) {
    if (dry_run) {
        struct stat existing;
        if (mv_keeps_existing(resolve_mode) && lstat(dst, &existing) == 0) return 2;
        jash_writef(w, ctx, "  Would move: %s -> %s\n", src, dst);
        return 0;
    }
    if (atomic && (strlen(src) >= sizeof(g_mv_ops[0].src) ||
                   strlen(dst) >= sizeof(g_mv_ops[0].dst))) {
        jash_writef(w, ctx, "mv: path too long for atomic rollback: '%s'\n", src);
        return 1;
    }
    
    /* Check if destination exists */
    struct stat st_dst;
    if (lstat(dst, &st_dst) == 0) {
        if (S_ISDIR(st_dst.st_mode)) {
            if (mv_keeps_existing(resolve_mode)) {
                jash_writef(jash_stderr_writer, 0,
                            "mv: skipped '%s' (conflict)\n", src);
                return 2;
            }
            jash_writef(w, ctx,
                        "mv: cannot overwrite directory '%s' with non-directory '%s'\n",
                        dst, src);
            return 1;
        }
        /* Destination exists: resolve conflict */
        if (!mv_resolve_conflict(dst, resolve_mode, w, ctx)) {
            jash_writef(jash_stderr_writer, 0, "mv: skipped '%s' (conflict)\n", src);
            /* BUG-047: this used to return 0 — the SAME code as success —
             * so the caller's `moved++` counted a skipped file as moved,
             * printing the contradictory "skipped 'x'" followed by
             * "moved N file(s)" (N including x). 2 = skipped: neither an
             * error nor a move. */
            return 2;
        }
        struct stat source;
        if (lstat(src, &source) == 0 && mv_same_identity(&source, &st_dst)) {
            if (item) item->code = "same_file";
            jash_writef(w, ctx, "mv: '%s' and '%s' are the same file\n", src, dst);
            return 1;
        }
        /* rename(2) replaces the destination atomically. Removing it first
         * creates a data-loss window and loses the destination entirely when
         * the subsequent rename fails. */
    }
    
    /* Keep-old/skip must be enforced by the publication syscall, not only by
     * the destination snapshot above; another process can create dst between
     * the check and rename. */
    int no_replace = mv_keeps_existing(resolve_mode);
    mv_capture_t previous;
    memset(&previous, 0, sizeof(previous));
    int atomic_src_fd = -1, atomic_dst_fd = -1;
    char atomic_src_name[MV_PATH_SIZE] = "";
    char atomic_dst_name[MV_PATH_SIZE] = "";
    struct stat source_expected, moved_identity;
    if (atomic) {
        atomic_src_fd = mv_parent_open(src, atomic_src_name,
                                       sizeof(atomic_src_name));
        atomic_dst_fd = mv_parent_open(dst, atomic_dst_name,
                                       sizeof(atomic_dst_name));
        if (atomic_src_fd < 0 || atomic_dst_fd < 0 ||
            fstatat(atomic_src_fd, atomic_src_name, &source_expected,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            if (atomic_src_fd >= 0) close(atomic_src_fd);
            if (atomic_dst_fd >= 0) close(atomic_dst_fd);
            jash_writef(w, ctx,
                        "mv: cannot pin atomic move endpoints for '%s'\n", src);
            return 1;
        }
    }
    if (atomic && !no_replace) {
        mv_possible(item);
        struct stat expected;
        if (fstatat(atomic_dst_fd, atomic_dst_name, &expected,
                    AT_SYMLINK_NOFOLLOW) == 0) {
            if (mv_capture_identity_at(atomic_dst_fd, atomic_dst_name,
                                       &expected, &previous) != 0) {
                if (previous.active) {
                    mv_capture_set_path(&previous, dst);
                    jash_writef(w, ctx,
                                "mv: destination recovery retained at '%s'\n",
                                previous.retained_path);
                    mv_item_recovery(item, previous.retained_path);
                    mv_capture_abandon(&previous);
                }
                close(atomic_src_fd);
                close(atomic_dst_fd);
                jash_writef(w, ctx, "mv: cannot preserve destination '%s' for rollback\n", dst);
                return 1;
            }
            mv_capture_set_path(&previous, dst);
        } else if (errno != ENOENT) {
            close(atomic_src_fd);
            close(atomic_dst_fd);
            jash_writef(w, ctx, "mv: cannot inspect destination '%s'\n", dst);
            return 1;
        }
    }
    int rename_rc;
    jash_mutation_effect_t before = item ? item->effect : JASH_MUT_NONE;
    if (no_replace) mv_possible(item);
    if (atomic) {
        /* Capture leaves dst absent. Publishing no-replace prevents a raced
         * inode from being silently destroyed in that window. */
        rename_rc = mv_publish_noreplace(atomic_src_fd, atomic_src_name,
                                         atomic_dst_fd, atomic_dst_name);
    } else {
        rename_rc = no_replace
            ? mv_publish_noreplace(AT_FDCWD, src, AT_FDCWD, dst)
            : rename(src, dst);
    }
    if (rename_rc != 0) {
        int rename_errno = errno;
        if (previous.active) {
            int restore_rc = mv_capture_restore(&previous);
            if (restore_rc != 0) {
                if (atomic_src_fd >= 0) close(atomic_src_fd);
                if (atomic_dst_fd >= 0) close(atomic_dst_fd);
                if (restore_rc < 0)
                    mv_item_recovery(item, previous.retained_path);
                if (restore_rc < 0)
                    jash_writef(w, ctx,
                                "mv: cannot restore destination '%s' after failed move; recovery retained at '%s'\n",
                                dst, previous.retained_path);
                else
                    jash_writef(w, ctx,
                                "mv: restored destination '%s' after failed move but directory sync failed\n",
                                dst);
                return 1;
            }
        }
        if (atomic_src_fd >= 0) close(atomic_src_fd);
        if (atomic_dst_fd >= 0) close(atomic_dst_fd);
        errno = rename_errno;
        if (no_replace && (errno == EEXIST || errno == ENOTEMPTY)) {
            if (item) item->effect = before;
            jash_writef(jash_stderr_writer, 0,
                        "mv: skipped '%s' (conflict)\n", src);
            return 2;
        }
        /* ADR-0044: cross-device (EXDEV) — rename can't span filesystems, so
         * fall back to copy + unlink for a regular file (GNU mv does this).
         * Was a hard failure before: `mv /tmp/x .` across mounts just errored. */
        if (errno == EXDEV) {
            if (atomic_multi) {
                jash_writef(w, ctx,
                            "mv: multi-source --atomic does not support cross-device move '%s'\n",
                            src);
                return 1;
            }
            if (item) item->effect = before;
            int copy_rc = mv_copy_regular(src, dst, no_replace, w, ctx, item);
            if (copy_rc == MV_COPY_DONE) {
                /* mv_copy_regular removed the verified source pathname. */
            } else if (copy_rc == MV_COPY_SKIPPED) {
                if (item) item->effect = before;
                jash_writef(jash_stderr_writer, 0,
                            "mv: skipped '%s' (conflict)\n", src);
                return 2;
            } else {
                jash_writef(w, ctx, "mv: cannot move '%s' to '%s' across devices\n", src, dst);
                return 1;
            }
        } else {
            jash_writef(w, ctx, "mv: cannot move '%s' to '%s'\n", src, dst);
            return 1;
        }
    }

    if (item) item->effect = JASH_MUT_CHANGED;
    if (atomic && rename_rc == 0) {
        struct stat source_now;
        int source_absent =
            fstatat(atomic_src_fd, atomic_src_name, &source_now,
                    AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
        int moved_matches =
            fstatat(atomic_dst_fd, atomic_dst_name, &moved_identity,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            mv_same_identity(&moved_identity, &source_expected);
        if (!source_absent || !moved_matches ||
            mv_journal_add(src, dst, atomic_src_fd, atomic_src_name,
                           atomic_dst_fd, atomic_dst_name, &moved_identity,
                           &previous, item) != 0) {
            if (source_absent && moved_matches)
                (void)mv_publish_noreplace(atomic_dst_fd, atomic_dst_name,
                                           atomic_src_fd, atomic_src_name);
            if (previous.active) {
                struct stat restored_source;
                if (fstatat(atomic_src_fd, atomic_src_name, &restored_source,
                            AT_SYMLINK_NOFOLLOW) == 0 &&
                    mv_same_identity(&restored_source, &source_expected)) {
                    if (mv_capture_restore_report(&previous, "prior destination recovery", w, ctx) < 0)
                        mv_item_recovery(item, previous.retained_path);
                }
                else {
                    jash_writef(w, ctx,
                                "mv: prior destination retained at '%s'\n",
                                previous.retained_path);
                    mv_item_recovery(item, previous.retained_path);
                    mv_capture_abandon(&previous);
                }
            }
            close(atomic_src_fd);
            close(atomic_dst_fd);
            jash_writef(w, ctx, "mv: cannot journal atomic move '%s'\n", src);
            return 1;
        }
    }
    {
        char desc[200];
        snprintf(desc, sizeof(desc), "mv %s -> %s", src, dst);
        if (atomic && item) { /* JSON atomic records are journaled after commit. */ }
        else if (item && no_replace && !atomic && !rename_rc && !strcmp(item->source, src))
            jash_timeline_record_ref(JASH_UNDO_MOVE, desc, src, dst, &item->undo);
        else jash_timeline_record(JASH_UNDO_MOVE, desc, src, dst);
    }
    
    return 0;
}

/* ── Recursive move ───────────────────────────────────────────────────────── */

static int mv_recursive(const char* src, const char* dst, int resolve_mode,
                         int dry_run, int atomic, int atomic_multi,
                         jash_writer_t w, void* ctx, jash_mutation_item_t* item) {
    if (jash_mutation_cancel_status()) return jash_mutation_cancel_status();
    struct stat st;
    if (atomic && (strlen(src) >= sizeof(g_mv_ops[0].src) ||
                   strlen(dst) >= sizeof(g_mv_ops[0].dst))) {
        jash_writef(w, ctx, "mv: path too long for atomic rollback: '%s'\n", src);
        return 1;
    }
    if (lstat(src, &st) != 0) {
        jash_writef(w, ctx, "mv: cannot stat '%s'\n", src);
        return 1;
    }
    
    if (!S_ISDIR(st.st_mode)) {
        /* Regular file */
        return mv_move_file(src, dst, resolve_mode, dry_run, atomic,
                            atomic_multi, w, ctx, item);
    }
    if (dry_run) {
        struct stat existing;
        if (mv_keeps_existing(resolve_mode) && lstat(dst, &existing) == 0) return 2;
        jash_writef(w, ctx, "  Would move directory: %s -> %s\n", src, dst);
        return 0;
    }
    
    struct stat conflict;
    if (lstat(dst, &conflict) == 0) {
        if (!mv_resolve_conflict(dst, resolve_mode, w, ctx)) return 2;
        if (mv_same_identity(&st, &conflict)) {
            if (item) item->code = "same_file";
            jash_writef(w, ctx, "mv: '%s' and '%s' are the same directory\n", src, dst);
            return 1;
        }
    }
    /* Directory: try simple rename first. Keep-old/skip needs no-replace here
     * too because this fast path bypasses mv_move_file. */
    int no_replace = mv_keeps_existing(resolve_mode);
    int atomic_src_fd = -1, atomic_dst_fd = -1;
    char atomic_src_name[MV_PATH_SIZE] = "";
    char atomic_dst_name[MV_PATH_SIZE] = "";
    struct stat source_expected, moved_identity;
    if (atomic) {
        struct stat existing;
        atomic_src_fd = mv_parent_open(src, atomic_src_name,
                                       sizeof(atomic_src_name));
        atomic_dst_fd = mv_parent_open(dst, atomic_dst_name,
                                       sizeof(atomic_dst_name));
        if (atomic_src_fd < 0 || atomic_dst_fd < 0 ||
            fstatat(atomic_src_fd, atomic_src_name, &source_expected,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            if (atomic_src_fd >= 0) close(atomic_src_fd);
            if (atomic_dst_fd >= 0) close(atomic_dst_fd);
            return 1;
        }
        if (fstatat(atomic_dst_fd, atomic_dst_name, &existing,
                    AT_SYMLINK_NOFOLLOW) == 0) {
            close(atomic_src_fd);
            close(atomic_dst_fd);
            jash_writef(w, ctx,
                        "mv: --atomic directory overwrite is unsupported for '%s'\n",
                        dst);
            return 1;
        }
        if (errno != ENOENT) {
            close(atomic_src_fd);
            close(atomic_dst_fd);
            return 1;
        }
    }
    int rename_rc;
    if (atomic) {
        rename_rc = mv_publish_noreplace(atomic_src_fd, atomic_src_name,
                                         atomic_dst_fd, atomic_dst_name);
    } else {
        rename_rc = no_replace
            ? mv_publish_noreplace(AT_FDCWD, src, AT_FDCWD, dst)
            : rename(src, dst);
    }
    if (rename_rc == 0) {
        if (item) item->effect = JASH_MUT_CHANGED;
        if (atomic) {
            struct stat source_now;
            int source_absent =
                fstatat(atomic_src_fd, atomic_src_name, &source_now,
                        AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
            int moved_matches =
                fstatat(atomic_dst_fd, atomic_dst_name, &moved_identity,
                        AT_SYMLINK_NOFOLLOW) == 0 &&
                mv_same_identity(&moved_identity, &source_expected);
            if (!source_absent || !moved_matches ||
                mv_journal_add(src, dst, atomic_src_fd, atomic_src_name,
                                atomic_dst_fd, atomic_dst_name,
                                &moved_identity, NULL, item) != 0) {
                if (source_absent && moved_matches)
                    (void)mv_publish_noreplace(atomic_dst_fd, atomic_dst_name,
                                               atomic_src_fd, atomic_src_name);
                close(atomic_src_fd);
                close(atomic_dst_fd);
                return 1;
            }
        }
        if (item && no_replace && !atomic && !strcmp(item->source, src)) {
            char description[256]; snprintf(description, sizeof(description), "mv directory %s", src);
            jash_timeline_record_ref(JASH_UNDO_MOVE, description, src, dst, &item->undo);
        }
        return 0;
    }
    if (atomic_src_fd >= 0) close(atomic_src_fd);
    if (atomic_dst_fd >= 0) close(atomic_dst_fd);
    if (no_replace && (errno == EEXIST || errno == ENOTEMPTY)) {
        jash_writef(jash_stderr_writer, 0,
                    "mv: skipped '%s' (conflict)\n", src);
        return 2;
    }
    if (atomic) {
        jash_writef(w, ctx,
                    "mv: --atomic recursive cross-device move is unsupported for '%s'\n",
                    src);
        return 1;
    }
    if (errno != EXDEV) {
        jash_writef(w, ctx, "mv: cannot move directory '%s' to '%s'\n", src, dst);
        return 1;
    }
    
    /* Fallback: recursive copy + delete */
    if (mkdir(dst, st.st_mode) != 0) {
        jash_writef(w, ctx, "mv: cannot create directory '%s'\n", dst);
        return 1;
    }
    if (item) item->effect = JASH_MUT_CHANGED;
    
    DIR* dir = opendir(src);
    if (!dir) {
        jash_writef(w, ctx, "mv: cannot open directory '%s'\n", src);
        return 1;
    }
    
    /* BUG-047: track hard errors and skips separately rather than summing
     * child return codes — once mv_move_file could return a third value
     * (2 = skipped), a raw sum stopped being a meaningful 0/nonzero
     * error signal (e.g. one skip + one real error summed to 3). */
    int had_error = 0, had_skip = 0;
    struct dirent* ent;

    for (;;) {
        if (jash_mutation_cancel_status()) { had_error = 1; break; }
        errno = 0;
        ent = readdir(dir);
        if (!ent) { if (errno) had_error = 1; break; }
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }

        char src_child[1024];
        char dst_child[1024];
        int sn = snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
        int dn = snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, ent->d_name);
        if (sn < 0 || dn < 0 || (size_t)sn >= sizeof(src_child) || (size_t)dn >= sizeof(dst_child)) {
            had_error = 1;
            continue;
        }

        int rc = mv_recursive(src_child, dst_child, resolve_mode, dry_run,
                              atomic, atomic_multi, w, ctx, item);
        if (rc == 1) had_error = 1;
        else if (rc == 2) had_skip = 1;
    }

    closedir(dir);

    if (!had_error && !had_skip && !jash_mutation_cancel_status()) {
        if (rmdir(src) != 0) had_error = 1;
    }

    if (item && (had_error || had_skip)) item->code = "subtree_incomplete";
    if (had_error) return 1;
    if (had_skip) return 2;
    return 0;
}

/* ── Main mv builtin ──────────────────────────────────────────────────────── */

typedef struct {
    jash_mutation_item_t item;
    char destination[512];
    char recovery[MV_PATH_SIZE];
    int directory;
} mv_report_entry_t;

static int mv_atomic_report(jash_state_t* s, jash_writer_t w, void* ctx,
                             int count, char** sources, const char* destination,
                             int destination_is_dir, int resolve_mode, int dry,
                             jash_mutation_report_t* report) {
    (void)s;
    report->transaction = "not_started";
    if (count < 1 || count > MV_MAX_OPS) { report->error_code = "atomic_item_limit"; return 1; }
    mv_report_entry_t* entries = calloc((size_t)count, sizeof(*entries));
    if (!entries) { report->error_code = "out_of_memory"; return 1; }
    int rc = 0;
    for (int i = 0; i < count; i++) {
        mv_report_entry_t* e = &entries[i];
        e->item = (jash_mutation_item_t){.source = sources[i], .destination = destination, .action = "move",
            .status = JASH_MUT_SKIPPED, .code = "not_attempted", .bytes_known = 1,
            .recovery_path = e->recovery, .recovery_capacity = sizeof(e->recovery)};
        size_t end = strlen(sources[i]), begin = end;
        while (end > 1 && sources[i][end - 1] == '/') end--;
        begin = end; while (begin && sources[i][begin - 1] != '/') begin--;
        int n = destination_is_dir ? snprintf(e->destination, sizeof(e->destination), "%s/%.*s", destination, (int)(end - begin), sources[i] + begin)
                                   : snprintf(e->destination, sizeof(e->destination), "%s", destination);
        struct stat source_st, parent_st;
        char leaf[MV_PATH_SIZE];
        const char* error = NULL;
        if (strlen(sources[i]) >= sizeof(g_mv_ops[0].src) || n < 0 || (size_t)n >= sizeof(e->destination)) error = "path_too_long";
        else if (lstat(sources[i], &source_st)) error = "inspect_source";
        else {
            e->item.destination = e->destination;
            e->directory = S_ISDIR(source_st.st_mode);
            int parent = mv_parent_open(e->destination, leaf, sizeof(leaf));
            if (parent < 0 || fstat(parent, &parent_st)) error = "inspect_destination_parent";
            else if (source_st.st_dev != parent_st.st_dev) error = "atomic_cross_device_unsupported";
            if (parent >= 0) close(parent);
            for (int prior = 0; prior < i && !error; prior++)
                if (!strcmp(entries[prior].destination, e->destination)) error = "duplicate_destination";
        }
        if (error) { rc = 1; e->item.status = JASH_MUT_FAILED; e->item.code = error; }
    }
    if (rc) goto emit;
    if (dry) {
        report->transaction = "preview";
        for (int i = 0; i < count; i++) {
            struct stat existing;
            int skipped = mv_keeps_existing(resolve_mode) && !lstat(entries[i].item.destination, &existing);
            entries[i].item.status = skipped ? JASH_MUT_SKIPPED : JASH_MUT_PLANNED;
            entries[i].item.code = skipped ? "conflict" : NULL;
        }
        goto emit;
    }
    if (mv_ops_ensure()) { rc = 1; report->error_code = "out_of_memory"; goto emit; }
    g_mv_ops_count = 0;
    for (int i = 0; i < count; i++) {
        if (jash_mutation_report_stop(report)) { rc = jash_mutation_report_stop(report); break; }
        jash_mutation_item_t* item = &entries[i].item;
        item->status = JASH_MUT_FAILED; item->code = "move_failed";
        int result;
        if (jash_guard_claim(item->source, w, ctx)) { item->code = "claim_conflict"; result = 1; }
        else if (entries[i].directory)
            result = mv_recursive(item->source, item->destination, resolve_mode, 0, 1, 1, w, ctx, item);
        else result = mv_move_file(item->source, item->destination, resolve_mode, 0, 1, 1, w, ctx, item);
        if (!result) { item->status = JASH_MUT_COMPLETED; item->effect = JASH_MUT_CHANGED; item->code = NULL; }
        else if (result == 2) { item->status = JASH_MUT_SKIPPED; item->effect = JASH_MUT_NONE; item->code = "conflict"; }
        else { rc = result; break; }
        /* A delivery heartbeat detects result-channel failure without
         * prematurely claiming that a tentative move has committed. */
        if (jash_mutation_report_heartbeat(report)) { rc = 125; break; }
    }
    if (!rc && jash_mutation_report_stop(report)) rc = jash_mutation_report_stop(report);
    if (rc) {
        int tentative = g_mv_ops_count, uncertain = 0;
        for (int i = 0; i < count; i++)
            if (entries[i].item.status == JASH_MUT_FAILED && entries[i].item.effect != JASH_MUT_NONE) uncertain = 1;
        int rollback_failed = mv_rollback(w, ctx);
        report->transaction = rollback_failed || uncertain ? "partial" : tentative ? "rolled_back" : "not_started";
    }
    else if (mv_commit(w, ctx)) { rc = 1; report->transaction = "partial"; }
    else {
        report->transaction = "committed";
        for (int i = 0; i < count; i++) if (entries[i].item.status == JASH_MUT_COMPLETED) {
            char description[256]; snprintf(description, sizeof(description), "mv atomic %s", entries[i].item.source);
            jash_timeline_record(JASH_UNDO_MOVE, description, entries[i].item.source, entries[i].item.destination);
        }
    }
emit:
    for (int i = 0; i < count; i++) if (jash_mutation_report_item(report, &entries[i].item)) { rc = 125; break; }
    free(entries);
    return rc;
}

static int mv_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                   jash_mutation_report_t* report) {
    (void)s;

    /* ADR-0036 W2 / BUG-165 root-cause fix: argv_fn builtin — the dispatcher
     * hands us a quote-aware argv (`argv[argc]` is NULL). No more strtok(" \t")
     * re-split that would shred a quoted path with a space (`mv 'a b' dst`) into
     * separate words. Word boundaries survive; the last arg is always the dst. */
    if (argc == 0) {
        jash_writes(w, ctx, "Usage: mv [--atomic] [--dry-run] [--resolve=keep-new|keep-old|skip] <src> [src2...] <dst>\n");
        return 1;
    }

    /* Parse arguments */
    int atomic = 0;
    int dry_run = 0;
    int resolve_mode = RESOLVE_ASK;
    int verbose = 0;
    const char* target_dir = NULL;
    int no_target_dir = 0;

    /* Extract flags */
    int file_start = 0;
    int end_opts = 0;
    for (int i = 0; i < argc; i++) {
        if (end_opts) { file_start = i; break; }
        if (strcmp(argv[i], "--") == 0) { end_opts = 1; file_start = i + 1; continue; }
        if (strcmp(argv[i], "--atomic") == 0) {
            atomic = 1;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0) {
            /* -f: overwrite without prompting (GNU default is already silent
             * overwrite for agents; -f also forces it in interactive mode). */
            resolve_mode = RESOLVE_KEEP_NEW;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-clobber") == 0) {
            resolve_mode = RESOLVE_KEEP_OLD;   /* never overwrite */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
            resolve_mode = RESOLVE_PROMPT;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
            file_start = i + 1;
        } else if (!strcmp(argv[i], "--no-target-directory")) {
            no_target_dir = 1; file_start = i + 1;
        } else if (!strcmp(argv[i], "--target-directory") || !strncmp(argv[i], "--target-directory=", 19)) {
            if (target_dir) { jash_writes(w, ctx, "mv: duplicate target directory\n"); return 1; }
            if (argv[i][18] == '=') target_dir = argv[i] + 19;
            else if (++i < argc) target_dir = argv[i];
            else { jash_writes(w, ctx, "mv: missing target directory\n"); return 1; }
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '-' && argv[i][1] != '\0') {
            /* short-flag cluster: -fv, -nv, ... */
            int ok = 1;
            const char* option = argv[i] + 1;
            while (*option) {
                switch (*option++) {
                    case 'f': resolve_mode = RESOLVE_KEEP_NEW; break;
                    case 'n': resolve_mode = RESOLVE_KEEP_OLD; break;
                    case 'i': resolve_mode = RESOLVE_PROMPT; break;
                    case 'v': verbose = 1; break;
                    case 'T': no_target_dir = 1; break;
                    case 't':
                        if (target_dir) { jash_writes(w, ctx, "mv: duplicate target directory\n"); return 1; }
                        if (*option) target_dir = option;
                        else if (++i < argc) target_dir = argv[i];
                        else { jash_writes(w, ctx, "mv: missing target directory\n"); return 1; }
                        option = "";
                        break;
                    default: ok = 0; break;
                }
                if (!ok) break;
            }
            if (!ok) { jash_writef(w, ctx, "mv: unknown option '%s'\n", argv[i]); return 1; }
            file_start = i + 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            if (report) report->preview = 1;
            file_start = i + 1;
        } else if (strncmp(argv[i], "--resolve=", 10) == 0) {
            const char* mode = argv[i] + 10;
            if (strcmp(mode, "keep-new") == 0) {
                resolve_mode = RESOLVE_KEEP_NEW;
            } else if (strcmp(mode, "keep-old") == 0) {
                resolve_mode = RESOLVE_KEEP_OLD;
            } else if (strcmp(mode, "skip") == 0) {
                resolve_mode = RESOLVE_SKIP;
            } else {
                jash_writef(w, ctx, "mv: unknown resolve mode '%s'\n", mode);
                return 1;
            }
            file_start = i + 1;
        } else if (argv[i][0] == '-') {
            jash_writef(w, ctx, "mv: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            file_start = i;
            break;
        }
    }
    
    if (target_dir && no_target_dir) {
        jash_writes(w, ctx, "mv: -t and -T are incompatible\n");
        return 1;
    }
    if (argc - file_start < (target_dir ? 1 : 2)) {
        jash_writes(w, ctx, "mv: missing source or destination\n");
        return 1;
    }
    
    const char* dst = target_dir ? target_dir : argv[argc - 1];
    int source_end = target_dir ? argc : argc - 1;
    int num_sources = source_end - file_start;
    
    /* Check if destination is directory (for batch moves) */
    struct stat st_dst;
    int dst_is_dir = (stat(dst, &st_dst) == 0 && S_ISDIR(st_dst.st_mode));
    if (target_dir && !dst_is_dir) {
        jash_writes(w, ctx, "mv: target must be a directory\n");
        return 1;
    }
    if (no_target_dir) dst_is_dir = 0;
    
    if (num_sources > 1 && !dst_is_dir) {
        jash_writes(w, ctx, "mv: destination must be directory for multiple sources\n");
        return 1;
    }
    if (report && atomic)
        return mv_atomic_report(s, w, ctx, num_sources, argv + file_start, dst, dst_is_dir, resolve_mode, dry_run, report);

    if (atomic && !dry_run) {
        if (num_sources > MV_MAX_OPS || mv_ops_ensure() != 0) {
            jash_writes(w, ctx, "mv: cannot allocate atomic rollback journal\n");
            return 1;
        }
        /* Validate every operand before the first mutation. Cross-device batch
         * copy+delete is deliberately unsupported because it cannot be rolled
         * back by rename(2). */
        for (int i = file_start; i < source_end; i++) {
            struct stat preflight;
            if (lstat(argv[i], &preflight) != 0) {
                jash_writef(w, ctx, "mv: cannot access '%s'\n", argv[i]);
                return 1;
            }
            if (num_sources > 1 && preflight.st_dev != st_dst.st_dev) {
                jash_writef(w, ctx,
                            "mv: multi-source --atomic does not support cross-device source '%s'\n",
                            argv[i]);
                return 1;
            }
        }
    }
    
    /* Reset operation tracking */
    g_mv_ops_count = 0;
    
    if (dry_run) {
        jash_writes(w, ctx, "Dry run (no files will be moved):\n");
    }
    
    /* Move files */
    int errors = 0;
    int moved = 0;
    
    for (int i = file_start; i < source_end; i++) {
        if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
        const char* src = argv[i];
        char final_dst[MV_PATH_SIZE];
        jash_mutation_item_t item = {.source = src, .destination = dst, .action = "move",
                                    .code = "move_failed", .bytes_known = 1};
        int rc = 1;

        /* ADR-0042 F9: another live agent's intent-lock on this source. In
         * strict mode this blocks the move; advisory just warns (stderr).
         * Skip during dry-run so preview stays a pure no-op. */
        if (!dry_run && jash_guard_claim(src, w, ctx)) {
            item.code = "claim_conflict";
            goto item_done;
        }

        /* Build final destination path */
        if (dst_is_dir) {
            size_t end = strlen(src);
            while (end > 1 && src[end - 1] == '/') end--;
            size_t begin = end;
            while (begin && src[begin - 1] != '/') begin--;
            int n = snprintf(final_dst, sizeof(final_dst), "%s/%.*s", dst, (int)(end - begin), src + begin);
            if (n < 0 || (size_t)n >= sizeof(final_dst)) { item.code = "path_too_long"; goto item_done; }
        } else {
            if (strlen(dst) >= sizeof(final_dst)) { item.code = "path_too_long"; goto item_done; }
            strlcpy(final_dst, dst, sizeof(final_dst));
        }
        item.destination = final_dst;
        
        /* Check if source is directory */
        struct stat st_src;
        if (lstat(src, &st_src) != 0) {
            jash_writef(w, ctx, "mv: cannot access '%s'\n", src);
            item.code = "inspect_source";
            goto item_done;
        }
        
        /* BUG-047: distinguish this ITERATION's outcome (moved / error /
         * skipped) instead of comparing the cumulative `errors` total —
         * the old `if (errors == 0) moved++` also meant a successful move
         * after an earlier failure was never counted, since `errors` had
         * already gone nonzero. */
        if (S_ISDIR(st_src.st_mode)) {
            rc = mv_recursive(src, final_dst, resolve_mode, dry_run, atomic,
                              num_sources > 1, w, ctx, report ? &item : NULL);
        } else {
            rc = mv_move_file(src, final_dst, resolve_mode, dry_run, atomic,
                              num_sources > 1, w, ctx, report ? &item : NULL);
        }
item_done:
        if (!rc) { item.status = dry_run ? JASH_MUT_PLANNED : JASH_MUT_COMPLETED; item.code = NULL; }
        else if (rc == 2) {
            item.status = item.effect == JASH_MUT_NONE ? JASH_MUT_SKIPPED : JASH_MUT_PARTIAL;
            item.code = item.effect == JASH_MUT_NONE ? "conflict" : "subtree_incomplete";
        }
        if (jash_mutation_report_item(report, &item) != 0) return 125;
        if (rc == 1) {
            errors++;
            if (atomic) { mv_rollback(w, ctx); break; }
        }
        else if (rc == 0) {
            moved++;
            if (verbose && !dry_run)
                jash_writef(w, ctx, "'%s' -> '%s'\n", src, final_dst);
        }
        /* rc == 2: skipped — neither an error nor a move. */
    }

    if (atomic && !dry_run && errors == 0 && mv_commit(w, ctx) != 0)
        errors++;
    
    /* Summary → STDERR (was stdout). Keeps the human-facing confirmation but
     * leaves stdout POSIX-clean so `x=$(mv a b)` captures nothing — resolving
     * the documented conformance exception (mv.exceptions). -v's per-file
     * 'src' -> 'dst' still goes to stdout, matching GNU mv -v. */
    if (!dry_run && moved > 0 && verbose) {
        jash_writef(jash_stderr_writer, 0, "mv: moved %d file(s)\n", moved);
    }

    return errors ? 1 : 0;
}

int jash_b_mv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    static const char* const values[] = {"-t", "--target-directory", NULL};
    int filtered_count;
    char** filtered;
    jash_mutation_format_t format;
    int json = jash_mutation_args_mode(argc, argv, values, &filtered_count, &filtered, &format);
    int args_error = errno;
    if (!json) return mv_run(s, w, ctx, argc, argv, NULL);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, "mv", "operand_tree", format, w, ctx) != 0) { free(filtered); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    int rc = mv_run(s, jash_stderr_writer, NULL, filtered_count, filtered, &report);
    free(filtered);
    return jash_mutation_report_end(&report, rc);
}
