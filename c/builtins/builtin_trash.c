// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtin/trash — Trash management for Jash
//
// The first text-mode trash/recycle bin for a shell.
// Prevents accidental deletion. Files are recoverable for configurable TTL.
//
// Usage:
//   trash list           — list files in trash
//   trash restore <name> — restore a file from trash
//   trash restore --all  — restore all files
//   trash empty          — permanently delete all trash
//   trash info           — show trash stats
//   trash <files>        — move files to trash (same as rm --undo)

#include "builtin.h"
#include "recovery_output.h"
#include "osif/osinfo.h"
#include <core/timeline.h>
#include <core/guard.h>
#include <errno.h>
#include <fcntl.h>
#include <core/json_writer.h>
#include <core/data_json.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <signal.h>

/* fwd: transactional move (defined below) */
#include <core/env.h>
#include <theme/colors.h>
#include <sys/syscalls.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif

#define TRASH_MAX_AGE 172800  /* 48 hours default */

/* ADR-0050 W16: ONE TTL policy — before this, builtin_rm purged at 24h while
 * `trash list` displayed 48h (two sources of truth, JINV-class drift).
 * Policy: files >64MB cost disk → 24h; everything else 48h;
 * JASH_TRASH_TTL_H overrides the default class. */
long jash_trash_ttl_for(long size_bytes) {
    const char* e = getenv("JASH_TRASH_TTL_H");
    if (e && atoi(e) > 0) return (long)atoi(e) * 3600L;
    if (size_bytes > 64L * 1024L * 1024L) return 86400L;   /* big: 24h */
    return TRASH_MAX_AGE;                                 /* default: 48h */
}
#define TRASH_MAX_SIZE (1024L * 1024L * 1024L)  /* 1GB max trash size */

static int trash_get_dir(char* buf, size_t bufsz) {
    const char* home = getenv("HOME");
    if (!home) return -1;
    int length = snprintf(buf, bufsz, "%s/.jash/trash", home);
    if (length < 0 || (size_t)length >= bufsz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int trash_ensure_dir(void) {
    char trash_dir[512];
    if (trash_get_dir(trash_dir, sizeof(trash_dir)) < 0) return -1;
    struct stat st;
    if (stat(trash_dir, &st) != 0) {
        char jash_dir[512];
        snprintf(jash_dir, sizeof(jash_dir), "%s/.jash", getenv("HOME"));
        mkdir(jash_dir, 0700);
        mkdir(trash_dir, 0700);
    }
    return 0;
}

/* ── trash list ───────────────────────────────────────────────────────────── */

static int trash_list(jash_state_t* s, jash_writer_t w, void* ctx) {
    (void)s;
    char trash_dir[512];
    if (trash_get_dir(trash_dir, sizeof(trash_dir)) < 0) {
        jash_writes(w, ctx, "trash: cannot determine trash directory\n");
        return 1;
    }

    DIR* dir = opendir(trash_dir);
    if (!dir) {
        jash_writes(w, ctx, "trash: empty (no files)\n");
        return 0;
    }

    struct dirent* ent;
    int count = 0;
    long total_size = 0;
    time_t now = time(NULL);

    jash_writes(w, ctx, "\033[1;36mTrash contents:\033[0m\r\n");
    jash_writes(w, ctx, "\033[90m");
    jash_writef(w, ctx, "%-40s %-12s %s\r\n", "NAME", "SIZE", "EXPIRES IN");
    jash_writes(w, ctx, "\033[0m");

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strstr(ent->d_name, ".meta") != NULL) continue;

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", trash_dir, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) == 0) {
            /* Parse timestamp from filename: timestamp_originalname */
            long ts = 0;
            const char* underscore = strchr(ent->d_name, '_');
            if (underscore) {
                ts = atol(ent->d_name);
            }

            long remaining = jash_trash_ttl_for((long)st.st_size) - (now - ts);
            if (remaining < 0) remaining = 0;

            /* Get original name (after timestamp_) */
            const char* orig_name = underscore ? underscore + 1 : ent->d_name;

            /* Format size */
            char size_str[32];
            if (st.st_size < 1024) {
                snprintf(size_str, sizeof(size_str), "%ldB", (long)st.st_size);
            } else if (st.st_size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1fKB", st.st_size / 1024.0);
            } else {
                snprintf(size_str, sizeof(size_str), "%.1fMB", st.st_size / (1024.0 * 1024.0));
            }

            /* Format remaining time */
            char time_str[32];
            if (remaining > 3600) {
                snprintf(time_str, sizeof(time_str), "%ldh", remaining / 3600);
            } else if (remaining > 60) {
                snprintf(time_str, sizeof(time_str), "%ldm", remaining / 60);
            } else {
                snprintf(time_str, sizeof(time_str), "%lds", remaining);
            }

            /* Color: red if expiring soon */
            if (remaining < 3600) {
                jash_writes(w, ctx, "\033[31m");
            } else if (remaining < 86400) {
                jash_writes(w, ctx, "\033[33m");
            }

            jash_writef(w, ctx, "%-40s %-12s %s\r\n", orig_name, size_str, time_str);
            jash_writes(w, ctx, "\033[0m");

            count++;
            total_size += st.st_size;
        }
    }
    closedir(dir);

    jash_writes(w, ctx, "\r\n");
    jash_writef(w, ctx, "\033[90m%d file(s), %.1fMB total\033[0m\r\n",
                count, total_size / (1024.0 * 1024.0));

    return 0;
}

/* ── trash restore ────────────────────────────────────────────────────────── */

static int trash_open_restore_dir(const char* orig_dir) {
    const char* path = orig_dir;
    int fd;

    if (orig_dir[0] == '/') {
        const char* home = getenv("HOME");
        char root[512];
        if (!home || !home[0] || strlen(home) >= sizeof(root)) return -1;
        strlcpy(root, home, sizeof(root));
        size_t root_len = strlen(root);
        while (root_len > 1 && root[root_len - 1] == '/') root[--root_len] = '\0';
        if (strncmp(orig_dir, root, root_len) != 0 ||
            (root_len > 1 && orig_dir[root_len] != '/' && orig_dir[root_len] != '\0')) {
            /* The caller's cwd is already the authority for relative metadata.
             * Permit the equivalent absolute spelling, e.g. restoring from an
             * explicitly selected /dev/shm workdir after cross-device trash. */
            if (!getcwd(root, sizeof(root))) return -1;
            root_len = strlen(root);
            if (strncmp(orig_dir, root, root_len) != 0 ||
                (root_len > 1 && orig_dir[root_len] != '/' && orig_dir[root_len] != '\0'))
                return -1;
            fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        } else {
            fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        }
        path = orig_dir + root_len;
        while (*path == '/') path++;
    } else {
        fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    if (fd < 0) return -1;

    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char* end = strchr(path, '/');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char component[256];
        if (len == 0 || len >= sizeof(component)) { close(fd); return -1; }
        memcpy(component, path, len);
        component[len] = '\0';
        if (strcmp(component, "..") == 0) { close(fd); return -1; }
        if (strcmp(component, ".") != 0) {
            int next = openat(fd, component,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) { close(fd); return -1; }
            close(fd);
            fd = next;
        }
        path += len;
    }
    return fd;
}

static int trash_same_inode(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static int trash_owned_name_exists(int dirfd, const char* name,
                                   const struct stat* owned) {
    struct stat current;
    return name && name[0] && owned->st_ino != 0 &&
           fstatat(dirfd, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
           trash_same_inode(&current, owned);
}

static void trash_source_sibling_path(const char* source, const char* sibling,
                                      char* out, size_t out_size) {
    char absolute[1024];
    if (source[0] != '/') {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
            snprintf(absolute, sizeof(absolute), "%s/%s", cwd, source);
        else
            snprintf(absolute, sizeof(absolute), "%s", source);
        source = absolute;
    }
    const char* slash = strrchr(source, '/');
    if (!slash) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
            snprintf(out, out_size, "%s/%s", cwd, sibling);
        else
            snprintf(out, out_size, "./%s", sibling);
    } else if (slash == source) {
        snprintf(out, out_size, "/%s", sibling);
    } else {
        snprintf(out, out_size, "%.*s/%s", (int)(slash - source), source,
                 sibling);
    }
}

static void trash_report_retained(const char* kind, const char* path) {
    fprintf(stderr, "trash: retained %s at '%s'\n", kind, path);
}

static void trash_report_owned_in_dir(const char* dirpath,
                                      const struct stat* owned,
                                      const char* kind) {
    if (owned->st_ino == 0) return;
    DIR* dir = opendir(dirpath);
    if (!dir) return;
    int owned_dirfd = dirfd(dir);
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        struct stat current;
        if (fstatat(owned_dirfd, ent->d_name, &current,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            trash_same_inode(&current, owned)) {
            char retained[1024];
            snprintf(retained, sizeof(retained), "%s/%s", dirpath, ent->d_name);
            trash_report_retained(kind, retained);
        }
    }
    closedir(dir);
}

static int trash_rename_noreplace(int src_dirfd, const char* src,
                                  int dst_dirfd, const char* dst) {
    int rename_rc;
#if defined(__linux__) && defined(SYS_renameat2)
    rename_rc = jash_osif_renameat2(src_dirfd, src, dst_dirfd, dst, RENAME_NOREPLACE);
    if (rename_rc != 0 && (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
        if (linkat(src_dirfd, src, dst_dirfd, dst, 0) == 0) {
            if (unlinkat(src_dirfd, src, 0) == 0) return 0;
            return -1;
        }
        rename_rc = -1;
    }
#elif defined(__APPLE__)
    rename_rc = renameatx_np(src_dirfd, src, dst_dirfd, dst, RENAME_EXCL);
#else
    rename_rc = linkat(src_dirfd, src, dst_dirfd, dst, 0);
    if (rename_rc == 0) {
        if (unlinkat(src_dirfd, src, 0) == 0) return 0;
        return -1;
    }
#endif
    return rename_rc;
}

static void trash_unlink_owned(int dirfd, const char* name,
                               const struct stat* owned) {
    struct stat current;
    if (fstatat(dirfd, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        trash_same_inode(&current, owned))
        unlinkat(dirfd, name, 0);
}

static int trash_unique_name(int dirfd, const char* prefix,
                             char* name, size_t name_size) {
    static unsigned int sequence;
    struct stat st;
    for (int attempt = 0; attempt < 64; attempt++) {
        snprintf(name, name_size, "%s%ld.%u", prefix,
                 (long)getpid(), ++sequence);
        if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT)
            return 0;
    }
    errno = EEXIST;
    return -1;
}

static int trash_rollback_hold(int dirfd, const char* hold, const char* source) {
    if (trash_rename_noreplace(dirfd, hold, dirfd, source) != 0)
        return -1; /* The owned inode remains recoverable under the hold name. */
    return 0;
}

static int trash_move_file_at(int src_dirfd, const char* src,
                              int dst_dirfd, const char* dst) {
    struct stat same_owned, current;
    if (fstatat(src_dirfd, src, &same_owned, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (trash_rename_noreplace(src_dirfd, src, dst_dirfd, dst) == 0) {
        int failed = fstatat(dst_dirfd, dst, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
                     !trash_same_inode(&same_owned, &current) ||
                     fsync(dst_dirfd) != 0 || fsync(src_dirfd) != 0;
        if (!failed) return 0;
        int saved = errno ? errno : EIO;
        if (fstatat(dst_dirfd, dst, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            trash_same_inode(&same_owned, &current))
            (void)trash_rename_noreplace(dst_dirfd, dst, src_dirfd, src);
        (void)fsync(dst_dirfd);
        (void)fsync(src_dirfd);
        errno = saved;
        return -1;
    }
    if (errno != EXDEV) return -1;

    int sfd = openat(src_dirfd, src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (sfd < 0) return -1;
    struct stat source_owned;
    if (fstat(sfd, &source_owned) != 0 || !S_ISREG(source_owned.st_mode)) {
        close(sfd);
        errno = ENOTSUP;
        return -1;
    }

    char temp_name[64];
    if (trash_unique_name(dst_dirfd, ".jtrash.", temp_name, sizeof(temp_name)) != 0) {
        close(sfd);
        return -1;
    }
    int dfd = -1;
    for (int attempt = 0; attempt < 32; attempt++) {
        dfd = openat(dst_dirfd, temp_name,
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                     source_owned.st_mode & 0777);
        if (dfd >= 0 || errno != EEXIST) break;
        if (trash_unique_name(dst_dirfd, ".jtrash.", temp_name,
                              sizeof(temp_name)) != 0) break;
    }
    if (dfd < 0) { close(sfd); return -1; }
    struct stat dest_owned;
    if (fstat(dfd, &dest_owned) != 0) {
        close(sfd);
        close(dfd);
        unlinkat(dst_dirfd, temp_name, 0);
        return -1;
    }

    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t wn = write(dfd, buf + off, (size_t)(n - off));
            if (wn <= 0) { ok = 0; break; }
            off += wn;
        }
        if (!ok) break;
    }
    if (n < 0 || fsync(dfd) != 0) ok = 0;
    struct stat source_now;
    if (!ok || fstatat(src_dirfd, src, &source_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&source_owned, &source_now)) {
        int saved = ok ? ESTALE : errno;
        close(sfd);
        close(dfd);
        trash_unlink_owned(dst_dirfd, temp_name, &dest_owned);
        errno = saved;
        return -1;
    }

    char hold_name[64];
    if (trash_unique_name(src_dirfd, ".jhold.", hold_name, sizeof(hold_name)) != 0 ||
        trash_rename_noreplace(src_dirfd, src, src_dirfd, hold_name) != 0) {
        int saved = errno;
        close(sfd);
        close(dfd);
        trash_unlink_owned(dst_dirfd, temp_name, &dest_owned);
        errno = saved;
        return -1;
    }
    if (fstatat(src_dirfd, hold_name, &source_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&source_owned, &source_now)) {
        trash_rollback_hold(src_dirfd, hold_name, src);
        close(sfd);
        close(dfd);
        trash_unlink_owned(dst_dirfd, temp_name, &dest_owned);
        errno = ESTALE;
        return -1;
    }
    if (fsync(src_dirfd) != 0) {
        int saved = errno;
        trash_rollback_hold(src_dirfd, hold_name, src);
        (void)fsync(src_dirfd);
        close(sfd);
        close(dfd);
        trash_unlink_owned(dst_dirfd, temp_name, &dest_owned);
        errno = saved;
        return -1;
    }

    if (trash_rename_noreplace(dst_dirfd, temp_name, dst_dirfd, dst) != 0) {
        int saved = errno;
        trash_rollback_hold(src_dirfd, hold_name, src);
        close(sfd);
        close(dfd);
        trash_unlink_owned(dst_dirfd, temp_name, &dest_owned);
        errno = saved;
        return -1;
    }
    if (fsync(dst_dirfd) != 0) {
        int saved = errno;
        trash_unlink_owned(dst_dirfd, dst, &dest_owned);
        (void)fsync(dst_dirfd);
        trash_rollback_hold(src_dirfd, hold_name, src);
        (void)fsync(src_dirfd);
        close(sfd);
        close(dfd);
        errno = saved;
        return -1;
    }

    /* This post-publication sync is also the last safe injection point before
     * ownership checks and source deletion. */
    if (fsync(dfd) != 0 ||
        fstatat(src_dirfd, hold_name, &source_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&source_owned, &source_now)) {
        trash_unlink_owned(dst_dirfd, dst, &dest_owned);
        trash_rollback_hold(src_dirfd, hold_name, src);
        close(sfd);
        close(dfd);
        errno = ESTALE;
        return -1;
    }
    struct stat dest_now;
    if (fstatat(dst_dirfd, dst, &dest_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&dest_owned, &dest_now)) {
        trash_rollback_hold(src_dirfd, hold_name, src);
        close(sfd);
        close(dfd);
        errno = ESTALE;
        return -1;
    }
    int close_failed = 0;
    if (close(dfd) != 0) close_failed = 1;
    if (close(sfd) != 0) close_failed = 1;
    if (close_failed) return -1;

    if (fstatat(src_dirfd, hold_name, &source_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&source_owned, &source_now) ||
        fstatat(dst_dirfd, dst, &dest_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&dest_owned, &dest_now)) {
        errno = ESTALE;
        return -1;
    }
    if (unlinkat(src_dirfd, hold_name, 0) != 0) {
        int saved = errno;
        trash_unlink_owned(dst_dirfd, dst, &dest_owned);
        trash_rollback_hold(src_dirfd, hold_name, src);
        errno = saved;
        return -1;
    }
    if (fsync(src_dirfd) != 0) return -1;
    return 0;
}

#include "trash_metadata.inc"

static int trash_restore_file(const char* trash_dir, const char* pattern,
                              jash_writer_t w, void* ctx,
                              int* matched, int* failures) {
    int trash_fd = open(trash_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (trash_fd < 0) return -1;
    DIR* dir = fdopendir(trash_fd);
    if (!dir) { close(trash_fd); return -1; }

    struct dirent* ent;
    int restored = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strstr(ent->d_name, ".meta") != NULL) continue;

        /* Read original dir from .meta file */
        char meta_name[512];
        if (snprintf(meta_name, sizeof(meta_name), "%s.meta", ent->d_name) >=
            (int)sizeof(meta_name)) continue;
        trash_metadata_t metadata;
        if (trash_metadata_read(trash_fd, ent->d_name, &metadata)) {
            jash_writef(jash_stderr_writer, NULL, "trash: %s: %s\n", ent->d_name, metadata.error);
            (*failures)++;
            continue;
        }
        const char* orig_dir = metadata.directory;
        const char* orig_name = metadata.name;

        /* Build full restore path */
        char restore_path[1024];
        if (orig_dir[0]) {
            snprintf(restore_path, sizeof(restore_path), "%s/%s", orig_dir, orig_name);
        } else {
            snprintf(restore_path, sizeof(restore_path), "%s", orig_name);
        }

        /* ADR-0050 W5: exact matching — never substring. A pattern with '/'
         * is a full original path (this is what `undo` passes, killing the
         * d1/x vs d2/x collision); a bare name matches the original basename
         * EXACTLY. Substring matching restored the WRONG file whenever two
         * trashed paths shared a suffix. */
        if (pattern[0] != '\0') {
            if (strchr(pattern, '/')) {
                if (strcmp(restore_path, pattern) != 0) continue;
            } else {
                if (strcmp(orig_name, pattern) != 0) continue;
            }
        }
        (*matched)++;

        struct stat restore_data_owned = {0}, restore_meta_owned = {0};
        (void)fstatat(trash_fd, ent->d_name, &restore_data_owned,
                      AT_SYMLINK_NOFOLLOW);
        (void)fstatat(trash_fd, meta_name, &restore_meta_owned,
                      AT_SYMLINK_NOFOLLOW);

        /* Metadata is untrusted. Relative directories are anchored at cwd;
         * absolute directories are anchored at HOME or cwd. Each component is opened
         * without following symlinks, and renameat pins both endpoints. */
        int restore_fd = trash_open_restore_dir(orig_dir);
        if (restore_fd < 0) {
            jash_writef(jash_stderr_writer, 0,
                "trash: refusing meta with untrusted orig_dir: %s\n", orig_dir);
            (*failures)++;
            continue;
        }
        int move_rc = trash_move_file_at(trash_fd, ent->d_name,
                                         restore_fd, orig_name);
        int move_errno = errno;
        close(restore_fd);
        if (move_rc == 0) {
            if (unlinkat(trash_fd, meta_name, 0) == 0 && fsync(trash_fd) == 0) {
                restored++;
                jash_writef(w, ctx, "restored: %s\r\n", restore_path);
            } else {
                (*failures)++;
                jash_writef(jash_stderr_writer, 0,
                            "trash: restored data but metadata cleanup failed: %s\n",
                            restore_path);
                trash_report_owned_in_dir(trash_dir, &restore_meta_owned,
                                          "trash metadata");
            }
        } else {
            (*failures)++;
            jash_writef(jash_stderr_writer, 0,
                        "trash: cannot restore %s: %s\n", restore_path,
                        strerror(move_errno));
            trash_report_owned_in_dir(trash_dir, &restore_data_owned,
                                      "trash data recovery");
            trash_report_owned_in_dir(trash_dir, &restore_meta_owned,
                                      "trash metadata");
        }
    }
    closedir(dir);
    return restored;
}

static int trash_restore(jash_state_t* s, jash_writer_t w, void* ctx,
                          int argc, char** argv) {
    (void)s;
    char trash_dir[512];
    if (trash_get_dir(trash_dir, sizeof(trash_dir)) < 0) {
        jash_writes(w, ctx, "trash: cannot determine trash directory\n");
        return 1;
    }

    if (argc < 1) {
        jash_writes(w, ctx, "Usage: trash restore <filename> or trash restore --all\r\n");
        return 1;
    }

    int total = 0, matched = 0, failures = 0;
    if (strcmp(argv[0], "--all") == 0 || strcmp(argv[0], "-a") == 0) {
        int restored = trash_restore_file(trash_dir, "", w, ctx,
                                           &matched, &failures);
        if (restored < 0) failures++;
        else total = restored;
    } else {
        for (int i = 0; i < argc; i++) {
            int restored = trash_restore_file(trash_dir, argv[i], w, ctx,
                                               &matched, &failures);
            if (restored < 0) failures++;
            else total += restored;
        }
    }

    if (matched == 0) {
        jash_writes(w, ctx, "trash: no matching files found\r\n");
    } else if (total > 0) {
        jash_writef(w, ctx, "trash: restored %d file(s)\r\n", total);
    }

    return failures > 0 || matched == 0 ? 1 : 0;
}

/* ── trash empty ──────────────────────────────────────────────────────────── */

static int trash_gc_at(int parent, const char* name, unsigned depth) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(parent, name, 0);
    if (depth >= 64) { errno = ELOOP; return -1; }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR* directory = fdopendir(fd);
    if (!directory) { close(fd); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) rc = -1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (trash_gc_at(fd, entry->d_name, depth + 1)) rc = -1;
    }
    closedir(directory);
    return rc ? rc : unlinkat(parent, name, AT_REMOVEDIR);
}

static int trash_native_open(int create);

static int trash_gc_item(jash_discovery_rows_t* rows, jash_json_writer_t* output,
                          int mode, const char* path, const char* action, const char* error,
                          int applied, int dry) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "action")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, "gc")},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("action"), jash_discovery_string(rows, action)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
        {JDV_LITERAL("applied"), applied < 0 ? jdv_value_nothing() : jdv_value_bool(applied)},
        {JDV_LITERAL("dry_run"), jdv_value_bool(dry)}
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    if (mode == JASH_DISCOVERY_DATA) return jash_discovery_effects_flush(rows);
    if (rows->count > 1) jash_jsonw_raw(output, ",");
    if (jdv_json_write(output, &rows->items[rows->count - 1u], &JDV_DEFAULT_LIMITS) || jash_jsonw_finish(output)) return 125;
    return 0;
}

static int trash_empty(jash_state_t* s, jash_writer_t w, void* ctx, int mode, int dry) {
    (void)s;
    char trash_dir[512];
    if (trash_get_dir(trash_dir, sizeof trash_dir) < 0)
        return jash_discovery_error(w, ctx, "trash", mode, "trash_path_unavailable", 1);
    jash_json_writer_t output;
    jash_discovery_rows_t rows = {0};
    struct sigaction old_pipe, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (mode) {
        if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "trash", mode, "allocation_failed", 125);
        if (sigaction(SIGPIPE, &ignore, &old_pipe)) { jash_discovery_rows_free(&rows); return 125; }
        jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
        jash_jsonw_init(&output, w, ctx);
        int admitted = 0;
        if (mode == JASH_DISCOVERY_DATA) admitted = jash_discovery_effects_begin(&rows, w, ctx, "trash", mode, "jash.trash.gc.v1");
        else {
            jash_jsonw_raw(&output, "{\"schema\":\"jash.trash.gc.v1\",\"items\":[");
            if (jash_jsonw_finish(&output)) admitted = 125;
        }
        if (admitted) { jash_discovery_rows_free(&rows); (void)sigaction(SIGPIPE, &old_pipe, NULL); return admitted; }
    }
    int rc = 0, removed = 0, retained = 0, failures = 0, pins_complete = 1, truncated = 0;
    char recovery[1024] = "", hold[64] = "";
    int parent = mode ? trash_native_open(0) : open(trash_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int h = -1;
    jash_recovery_pins_t* pins = NULL;
    DIR* directory = NULL;
    if (parent < 0) { if (errno != ENOENT) failures++; goto done; }
    pins = jash_recovery_pins_load(); pins_complete = jash_recovery_pins_complete(pins);
    int walk = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    directory = walk >= 0 ? fdopendir(walk) : NULL;
    if (!directory) { if (walk >= 0) close(walk); failures++; goto done; }
    if (!dry && pins_complete) {
        snprintf(hold, sizeof hold, ".jash-trash-gc-%ld", (long)getpid());
        mode_t mask = umask(0); int made = mkdirat(parent, hold, 0700); umask(mask);
        if (made) { failures++; goto done; }
        snprintf(recovery, sizeof recovery, "%s/%s", trash_dir, hold);
        h = openat(parent, hold, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (h < 0) { failures++; goto done; }
        struct stat owned;
        if (fstat(h, &owned) || owned.st_uid != geteuid() || (owned.st_mode & 0077)) { close(h); h = -1; failures++; goto done; }
    }
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) failures++; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !strcmp(entry->d_name, hold)) continue;
        if (mode && rows.count >= 4094u) { truncated = 1; rc = 75; break; }
        char full[1024]; int n = snprintf(full, sizeof full, "%s/%s", trash_dir, entry->d_name);
        const char *action = "retained", *error = NULL;
        int applied = 0;
        if (!strncmp(entry->d_name, ".jash-", 6)) { retained++; error = "retained_recovery"; }
        else if (n < 0 || (size_t)n >= sizeof full) { retained++; error = "path_limit"; }
        else if (jash_recovery_pins_contains(pins, full)) { retained++; error = pins_complete ? "recovery_pin" : "pin_catalog_incomplete"; }
        else if (dry) { removed++; action = "planned"; }
        else {
            struct stat expected, captured;
            if (fstatat(parent, entry->d_name, &expected, AT_SYMLINK_NOFOLLOW) ||
                jash_osif_renameat2(parent, entry->d_name, h, entry->d_name, RENAME_NOREPLACE)) {
                failures++; error = "capture_failed"; action = "failed";
            } else if (fstatat(h, entry->d_name, &captured, AT_SYMLINK_NOFOLLOW) ||
                expected.st_dev != captured.st_dev || expected.st_ino != captured.st_ino) {
                (void)jash_osif_renameat2(h, entry->d_name, parent, entry->d_name, RENAME_NOREPLACE);
                failures++; error = "identity_changed"; action = "failed"; applied = -1;
            } else if (trash_gc_at(h, entry->d_name, 0)) {
                failures++; error = "remove_failed"; action = "failed"; applied = -1;
            } else { removed++; action = "removed"; applied = 1; }
        }
        if (mode) {
            int delivered = trash_gc_item(&rows, &output, mode, full, action, error, applied, dry);
            if (delivered) { rc = delivered; truncated = delivered == 75; break; }
        }
    }
done:
    if (directory && closedir(directory)) failures++;
    if (h >= 0) {
        struct stat held, named;
        int owned = !fstat(h, &held) && !fstatat(parent, hold, &named, AT_SYMLINK_NOFOLLOW) && held.st_dev == named.st_dev && held.st_ino == named.st_ino;
        if (close(h)) failures++;
        if (owned && !unlinkat(parent, hold, AT_REMOVEDIR)) recovery[0] = 0;
        if (fsync(parent)) failures++;
    }
    if (parent >= 0 && close(parent)) failures++;
    jash_recovery_pins_free(pins);
    if (!rc) rc = failures || retained || !pins_complete ? 1 : 0;
    if (mode == JASH_DISCOVERY_DATA) {
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("record"), jash_discovery_string(&rows, "summary")},
            {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.trash.gc.v1")},
            {JDV_LITERAL("dry_run"), jdv_value_bool(dry)},
            {JDV_LITERAL("removed"), jdv_value_u64(dry ? 0 : (uint64_t)removed)},
            {JDV_LITERAL("planned"), jdv_value_u64(dry ? (uint64_t)removed : 0)},
            {JDV_LITERAL("retained"), jdv_value_u64((uint64_t)retained)},
            {JDV_LITERAL("failed"), jdv_value_u64((uint64_t)failures)},
            {JDV_LITERAL("pins_complete"), jdv_value_bool(pins_complete)},
            {JDV_LITERAL("recovery_path"), jash_discovery_path(&rows, recovery[0] ? recovery : NULL)},
            {JDV_LITERAL("truncated"), jdv_value_bool(truncated)},
            {JDV_LITERAL("exit_code"), jdv_value_u64((uint64_t)rc)}
        };
        (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
        rc = jash_discovery_effects_end_outcome(&rows, rc,
            truncated && rc == 75 ? JDV_STREAM_INCOMPLETE : rc ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE,
            truncated, truncated ? "entry_limit" : NULL);
    } else if (mode) {
        jash_jsonw_raw(&output, "],\"dry_run\":"); jash_jsonw_raw(&output, dry ? "true" : "false");
        jash_jsonw_raw(&output, ",\"removed\":"); jash_jsonw_i64(&output, dry ? 0 : removed);
        jash_jsonw_raw(&output, ",\"planned\":"); jash_jsonw_i64(&output, dry ? removed : 0);
        jash_jsonw_raw(&output, ",\"retained\":"); jash_jsonw_i64(&output, retained);
        jash_jsonw_raw(&output, ",\"failed\":"); jash_jsonw_i64(&output, failures);
        jash_jsonw_raw(&output, ",\"pins_complete\":"); jash_jsonw_raw(&output, pins_complete ? "true" : "false");
        jash_jsonw_raw(&output, ",\"recovery_path\":"); jash_jsonw_string_or_bytes(&output, recovery[0] ? recovery : NULL);
        jash_jsonw_raw(&output, ",\"truncated\":"); jash_jsonw_raw(&output, truncated ? "true" : "false");
        jash_jsonw_raw(&output, ",\"exit_code\":"); jash_jsonw_i64(&output, rc); jash_jsonw_raw(&output, "}\n");
        if (jash_jsonw_finish(&output)) rc = 125;
    } else if (!dry && !rc) jash_writes(w, ctx, "trash: emptied\r\n");
    else jash_writef(w, ctx, "trash: %s %d entries, retained %d, failed %d%s%s\n",
                       dry ? "would remove" : "removed", removed, retained, failures,
                       recovery[0] ? "; recovery: " : "", recovery);
    jash_discovery_rows_free(&rows);
    if (mode && sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
    return rc;
}

/* ── trash info ───────────────────────────────────────────────────────────── */

static int trash_info(jash_state_t* s, jash_writer_t w, void* ctx) {
    (void)s;
    char trash_dir[512];
    if (trash_get_dir(trash_dir, sizeof(trash_dir)) < 0) {
        jash_writes(w, ctx, "trash: cannot determine trash directory\n");
        return 1;
    }

    DIR* dir = opendir(trash_dir);
    if (!dir) {
        jash_writes(w, ctx, "trash: empty\r\n");
        return 0;
    }

    int count = 0;
    long total_size = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", trash_dir, ent->d_name);
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            count++;
            total_size += st.st_size;
        }
    }
    closedir(dir);

    jash_writes(w, ctx, "\033[1;36mTrash Info:\033[0m\r\n");
    jash_writef(w, ctx, "  Location: %s\r\n", trash_dir);
    jash_writef(w, ctx, "  Files: %d\r\n", count);
    jash_writef(w, ctx, "  Size: %.1fMB\r\n", total_size / (1024.0 * 1024.0));
    jash_writef(w, ctx, "  Retention: 48 hours\r\n");
    jash_writef(w, ctx, "  Max size: 1GB\r\n");

    return 0;
}

/* ── trash (move files to trash) ──────────────────────────────────────────── */

static int trash_open_parent(const char* path, char* name, size_t name_size) {
    char parent[1024];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strlcpy(parent, path, sizeof(parent));
    char* slash = strrchr(parent, '/');
    const char* base = parent;
    if (slash) {
        base = slash + 1;
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    } else {
        strlcpy(parent, ".", sizeof(parent));
        base = path;
    }
    if (!base[0] || strlen(base) >= name_size) { errno = EINVAL; return -1; }
    strlcpy(name, base, name_size);
    return open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int trash_prepare_metadata(int trash_fd, const char* orig_dir,
                                  const char* original_name,
                                  char* temp_name, size_t temp_size,
                                  struct stat* owned) {
    if (!orig_dir || strlen(orig_dir) >= 512 || strpbrk(orig_dir, "\r\n") ||
        !original_name || !*original_name || strlen(original_name) >= 256 || strchr(original_name, '/') ||
        !strcmp(original_name, ".") || !strcmp(original_name, "..")) { errno = EINVAL; return -1; }
    if (trash_unique_name(trash_fd, ".jmeta.", temp_name, temp_size) != 0)
        return -1;
    int fd = openat(trash_fd, temp_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, owned) != 0) { close(fd); unlinkat(trash_fd, temp_name, 0); return -1; }
    FILE* file = fdopen(fd, "w");
    if (!file) { close(fd); trash_unlink_owned(trash_fd, temp_name, owned); return -1; }
    int failed = fputs(orig_dir, file) == EOF || fputc('\n', file) == EOF;
    if (!failed && fputs(TRASH_NAME_MARKER, file) == EOF) failed = 1;
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char* p = (const unsigned char*)original_name; !failed && *p; p++)
        if (fputc(hex[*p >> 4], file) == EOF || fputc(hex[*p & 15], file) == EOF) failed = 1;
    if (!failed && fputc('\n', file) == EOF) failed = 1;
    if (ferror(file) || fflush(file) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (failed) {
        trash_unlink_owned(trash_fd, temp_name, owned);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int trash_copy_temp(int source_fd, int trash_fd, mode_t mode,
                           char* temp_name, size_t temp_size,
                           struct stat* owned) {
    if (trash_unique_name(trash_fd, ".jtrash.", temp_name, temp_size) != 0)
        return -1;
    int fd = openat(trash_fd, temp_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    mode & 0777);
    if (fd < 0) return -1;
    if (fstat(fd, owned) != 0) { close(fd); unlinkat(trash_fd, temp_name, 0); return -1; }
    char buf[65536];
    ssize_t n;
    int failed = 0;
    while ((n = read(source_fd, buf, sizeof(buf))) > 0) {
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(fd, buf + offset, (size_t)(n - offset));
            if (written <= 0) { failed = 1; break; }
            offset += written;
        }
        if (failed) break;
    }
    if (n < 0 || fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (failed) {
        trash_unlink_owned(trash_fd, temp_name, owned);
        errno = EIO;
        return -1;
    }
    return 0;
}

int jash_trash_move_file(const char* src, const char* trash_dir,
                         const char* trash_name, const char* orig_dir) {
    char source_name[256], hold_name[64] = "", data_temp[64] = "";
    char meta_temp[64] = "", meta_name[512] = "";
    int source_dirfd = trash_open_parent(src, source_name, sizeof(source_name));
    if (source_dirfd < 0) return -1;
    int trash_fd = open(trash_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (trash_fd < 0) { close(source_dirfd); return -1; }
    int source_fd = openat(source_dirfd, source_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (source_fd < 0) { close(source_dirfd); close(trash_fd); return -1; }
    struct stat source_owned = {0}, data_owned = {0}, meta_owned = {0}, current;
    if (fstat(source_fd, &source_owned) != 0 ||
        snprintf(meta_name, sizeof(meta_name), "%s.meta", trash_name) >=
            (int)sizeof(meta_name) ||
        trash_prepare_metadata(trash_fd, orig_dir, source_name, meta_temp, sizeof(meta_temp),
                               &meta_owned) != 0)
        goto fail;

    /* The same-filesystem path renames the original inode directly. Make its
     * current contents durable before syncing the namespace transition. */
    if (S_ISREG(source_owned.st_mode) && fsync(source_fd) != 0) goto fail;

    if (trash_unique_name(source_dirfd, ".jhold.", hold_name, sizeof(hold_name)) != 0 ||
        trash_rename_noreplace(source_dirfd, source_name,
                               source_dirfd, hold_name) != 0)
        goto fail;
    if (fstatat(source_dirfd, hold_name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&source_owned, &current)) {
        errno = ESTALE;
        goto fail;
    }
    if (fsync(source_dirfd) != 0) goto fail;

    int cross_device = 0;
    if (trash_rename_noreplace(source_dirfd, hold_name, trash_fd, trash_name) == 0) {
        data_owned = source_owned;
        hold_name[0] = '\0';
    } else if (errno == EXDEV && S_ISREG(source_owned.st_mode)) {
        cross_device = 1;
        if (lseek(source_fd, 0, SEEK_SET) < 0 ||
            trash_copy_temp(source_fd, trash_fd, source_owned.st_mode,
                            data_temp, sizeof(data_temp), &data_owned) != 0 ||
            trash_rename_noreplace(trash_fd, data_temp, trash_fd, trash_name) != 0)
            goto fail;
        data_temp[0] = '\0';
    } else {
        if (errno == EXDEV) errno = ENOTSUP;
        goto fail;
    }

    if (trash_rename_noreplace(trash_fd, meta_temp, trash_fd, meta_name) != 0)
        goto fail;
    meta_temp[0] = '\0';
    if (fsync(trash_fd) != 0) goto fail;

    if (fstatat(trash_fd, trash_name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !trash_same_inode(&data_owned, &current)) {
        errno = ESTALE;
        goto fail;
    }
    if (cross_device) {
        if (fstatat(source_dirfd, hold_name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !trash_same_inode(&source_owned, &current) ||
            unlinkat(source_dirfd, hold_name, 0) != 0)
            goto fail;
        hold_name[0] = '\0';
        if (fsync(source_dirfd) != 0) {
            char retained[1024];
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, trash_name);
            trash_report_retained("trash data", retained);
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, meta_name);
            trash_report_retained("trash metadata", retained);
            close(source_fd);
            close(source_dirfd);
            close(trash_fd);
            return -1; /* Durable data+metadata remain as the recovery copy. */
        }
    } else if (fsync(source_dirfd) != 0) {
        char retained[1024];
        snprintf(retained, sizeof(retained), "%s/%s", trash_dir, trash_name);
        trash_report_retained("trash data", retained);
        snprintf(retained, sizeof(retained), "%s/%s", trash_dir, meta_name);
        trash_report_retained("trash metadata", retained);
        close(source_fd);
        close(source_dirfd);
        close(trash_fd);
        return -1;
    }

    close(source_fd);
    close(source_dirfd);
    close(trash_fd);
    return 0;

fail: {
        int saved = errno ? errno : EIO;
        if (data_temp[0]) trash_unlink_owned(trash_fd, data_temp, &data_owned);
        struct stat published;
        int data_retained = 0;
        int source_restored = 0;
        if (hold_name[0]) {
            if (trash_rollback_hold(source_dirfd, hold_name, source_name) == 0) {
                hold_name[0] = '\0';
                source_restored = 1;
            }
        }
        if (fstatat(trash_fd, trash_name, &published, AT_SYMLINK_NOFOLLOW) == 0 &&
            trash_same_inode(&data_owned, &published)) {
            if (hold_name[0]) data_retained = 1;
            else if (source_restored)
                trash_unlink_owned(trash_fd, trash_name, &data_owned);
            else if (trash_rename_noreplace(trash_fd, trash_name,
                                             source_dirfd, source_name) == 0)
                source_owned = published;
            else
                data_retained = 1;
        }
        if (data_retained) {
            /* A replacement at the original path prevented rollback. Keep the
             * metadata beside the published recovery copy instead of orphaning it. */
            if (fstatat(trash_fd, meta_name, &published, AT_SYMLINK_NOFOLLOW) != 0 &&
                meta_temp[0] &&
                trash_rename_noreplace(trash_fd, meta_temp,
                                        trash_fd, meta_name) == 0)
                meta_temp[0] = '\0';
        } else {
            if (meta_temp[0]) trash_unlink_owned(trash_fd, meta_temp, &meta_owned);
            if (fstatat(trash_fd, meta_name, &published, AT_SYMLINK_NOFOLLOW) == 0 &&
                trash_same_inode(&meta_owned, &published))
                unlinkat(trash_fd, meta_name, 0);
        }
        (void)fsync(source_dirfd);
        (void)fsync(trash_fd);

        char retained[1024];
        if (trash_owned_name_exists(source_dirfd, hold_name, &source_owned)) {
            trash_source_sibling_path(src, hold_name, retained, sizeof(retained));
            trash_report_retained("source recovery", retained);
        }
        if (trash_owned_name_exists(trash_fd, data_temp, &data_owned)) {
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, data_temp);
            trash_report_retained("temporary trash data", retained);
        }
        if (trash_owned_name_exists(trash_fd, trash_name, &data_owned)) {
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, trash_name);
            trash_report_retained("trash data", retained);
        }
        if (trash_owned_name_exists(trash_fd, meta_temp, &meta_owned)) {
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, meta_temp);
            trash_report_retained("temporary trash metadata", retained);
        }
        if (trash_owned_name_exists(trash_fd, meta_name, &meta_owned)) {
            snprintf(retained, sizeof(retained), "%s/%s", trash_dir, meta_name);
            trash_report_retained("trash metadata", retained);
        }
        close(source_fd);
        close(source_dirfd);
        close(trash_fd);
        errno = saved;
        return -1;
    }
}

static int trash_move(int argc, char** argv, jash_state_t* s,
                       jash_writer_t w, void* ctx) {
    (void)s;
    if (argc < 1) {
        jash_writes(w, ctx, "Usage: trash <file1> [file2] ...\r\n");
        return 1;
    }

    if (trash_ensure_dir() < 0) {
        jash_writes(w, ctx, "trash: cannot create trash directory\r\n");
        return 1;
    }

    char trash_dir[512];
    trash_get_dir(trash_dir, sizeof(trash_dir));

    int moved = 0, failures = 0;
    time_t now = time(NULL);

    for (int i = 0; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            jash_writef(w, ctx, "trash: '%s' not found\r\n", argv[i]);
            failures++;
            continue;
        }

        /* Get original directory */
        char orig_dir[512] = "";
        char *last_slash = strrchr(argv[i], '/');
        if (last_slash) {
            int dir_len = last_slash - argv[i];
            if (dir_len < (int)sizeof(orig_dir)) {
                strncpy(orig_dir, argv[i], dir_len);
                orig_dir[dir_len] = '\0';
            }
        }
        if (orig_dir[0] == '\0') {
            if (getcwd(orig_dir, sizeof(orig_dir)) == NULL) {
                strlcpy(orig_dir, ".", sizeof(orig_dir));
            }
        }

        /* Create trash path with timestamp */
        const char* basename = strrchr(argv[i], '/');
        basename = basename ? basename + 1 : argv[i];
        char trash_path[1024];
        snprintf(trash_path, sizeof(trash_path), "%s/%ld_%s",
                 trash_dir, (long)now, basename);

        /* Handle collision */
        if (stat(trash_path, &st) == 0) {
            struct timespec ts;
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                jash_writef(w, ctx, "trash: cannot generate a unique name for '%s'\r\n", argv[i]);
                failures++;
                continue;
            }
            snprintf(trash_path, sizeof(trash_path), "%s/%ld_%ld_%s",
                     trash_dir, (long)now, ts.tv_nsec, basename);
        }

        const char* trash_name = strrchr(trash_path, '/');
        trash_name = trash_name ? trash_name + 1 : trash_path;
        if (jash_trash_move_file(argv[i], trash_dir, trash_name, orig_dir) == 0) {
            moved++;
            {
                char desc[200];
                snprintf(desc, sizeof(desc), "trash %s", argv[i]);
                jash_timeline_record(JASH_UNDO_TRASH, desc, argv[i], trash_path);
            }
        } else {
            failures++;
            jash_writef(w, ctx, "trash: cannot move '%s': %s\r\n",
                        argv[i], strerror(errno));
        }
    }

    if (moved > 0) {
        jash_writef(w, ctx, "trash: moved %d file(s) (recoverable for 48h)\r\n", moved);
    }

    return failures > 0 ? 1 : 0;
}

/* ── Main entry point ─────────────────────────────────────────────────────── */

int jash_b_trash(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* Parse arguments */
    int argc = 0;
    char* argv[64];
    char args_buf[4096];
    strlcpy(args_buf, args ? args : "", sizeof(args_buf));
    args_buf[sizeof(args_buf) - 1] = '\0';

    char* token = strtok(args_buf, " \t");
    while (token && argc < 64) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }

    /* Dispatch subcommands */
    if (argc == 0 || strcmp(argv[0], "list") == 0 || strcmp(argv[0], "ls") == 0) {
        return trash_list(s, w, ctx);
    }
    if (strcmp(argv[0], "restore") == 0 || strcmp(argv[0], "undel") == 0) {
        return trash_restore(s, w, ctx, argc - 1, argv + 1);
    }
    if (strcmp(argv[0], "empty") == 0 || strcmp(argv[0], "clear") == 0 || !strcmp(argv[0], "gc")) {
        int json = 0, dry = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--json")) json = 1;
            else if (!strcmp(argv[i], "--dry-run")) dry = 1;
            else return 2;
        }
        return trash_empty(s, w, ctx, json, dry);
    }
    if (strcmp(argv[0], "info") == 0) {
        return trash_info(s, w, ctx);
    }
    /* ADR-0023 E2 / BUG-027: protection toggle for destructive ops (rm). */
    if (strcmp(argv[0], "on") == 0 || strcmp(argv[0], "protect") == 0) {
        jash_trash_set(1);
        jash_writes(w, ctx,
            "\033[1;32mtrash protection ON\033[0m — `rm` is now recoverable "
            "(moved to trash + undo/rewind cover it).\r\n");
        return 0;
    }
    if (strcmp(argv[0], "off") == 0) {
        jash_trash_set(0);
        jash_writes(w, ctx,
            "\033[1;33mtrash protection OFF\033[0m — `rm` permanently deletes "
            "(bash parity). `rm --undo` still trashes per-call.\r\n");
        return 0;
    }
    if (strcmp(argv[0], "auto") == 0 || strcmp(argv[0], "default") == 0) {
        jash_trash_set(-1);   /* clear explicit → revert to default policy */
        jash_writes(w, ctx,
            "trash protection AUTO — follows recording mode "
            "(on when JASH_SIGIL>=1).\r\n");
        return 0;
    }
    if (strcmp(argv[0], "status") == 0) {
        int on = jash_trash_protected();
        int src = jash_trash_source();
        const char* srcs = src == 0 ? "explicit `trash on/off`"
                         : src == 1 ? "JASH_TRASH env"
                                    : "default (JASH_SIGIL>=1)";
        jash_writef(w, ctx,
            "trash protection: %s\r\n  source: %s\r\n  effect: `rm` %s\r\n",
            on ? "\033[1;32mON\033[0m" : "\033[1;33mOFF\033[0m", srcs,
            on ? "→ trash (recoverable, undo/rewind cover it)"
               : "→ permanent delete (bash parity)");
        return 0;
    }
    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        jash_writes(w, ctx, "\033[1;36mTrash — recycle bin for the terminal\033[0m\r\n\r\n");
        jash_writes(w, ctx, "  trash <files>      Move files to trash (recoverable)\r\n");
        jash_writes(w, ctx, "  trash list         List files in trash\r\n");
        jash_writes(w, ctx, "  trash restore <f>  Restore a file from trash\r\n");
        jash_writes(w, ctx, "  trash restore --all Restore all files\r\n");
        jash_writes(w, ctx, "  trash empty        Permanently delete all trash\r\n");
        jash_writes(w, ctx, "  trash info         Show trash statistics\r\n");
        jash_writes(w, ctx, "\r\n");
        jash_writes(w, ctx, "\033[1mProtection (makes `rm` recoverable via undo/rewind):\033[0m\r\n");
        jash_writes(w, ctx, "  trash on           Protect: `rm` → trash + journaled (undoable)\r\n");
        jash_writes(w, ctx, "  trash off          Off: `rm` permanently deletes (bash parity)\r\n");
        jash_writes(w, ctx, "  trash auto         Follow recording mode (default; on if JASH_SIGIL>=1)\r\n");
        jash_writes(w, ctx, "  trash status       Show current protection + source\r\n");
        jash_writes(w, ctx, "\r\n");
        jash_writes(w, ctx, "\033[90mFiles are kept for 48 hours. Auto-cleaned on rm.\033[0m\r\n");
        return 0;
    }

    /* Default: move files to trash */
    return trash_move(argc, argv, s, w, ctx);
}

#include "trash_native.inc"
