// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/cp — disruptive cp builtin with progress bar,
// resume capability, deduplication, and verification.
//
// Features:
//   cp src dst              — copy file with progress bar
//   cp --resume partial dst — resume interrupted copy
//   cp --dedup src/ dst/    — use hardlinks for identical files
//   cp --verify src dst     — checksum verification after copy
//   cp -r src/ dst/         — recursive copy
//
// Progress: shows percentage, speed, ETA for large files
// Resume: continues from last byte offset if destination exists
// Dedup: creates hardlinks instead of copying identical files (saves space)
// Verify: compares BLAKE3 digests of the opened source and destination.

#include "builtin.h"
#include <core/env.h>
#include <core/timeline.h>
#include <module/blake3.h>
#include <core/mutation_report.h>
#include <sys/syscalls.h>
#include <sys/time.h>   /* utimes (-p preserve mtime) */
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>

#define CP_F_PROGRESS    0x01
#define CP_F_RESUME      0x02
#define CP_F_DEDUP       0x04
#define CP_F_VERIFY      0x08
#define CP_F_NO_PROGRESS 0x10
#define CP_F_NOCLOBBER   0x20   /* -n: never overwrite an existing dest */
#define CP_F_VERBOSE     0x40   /* -v: print 'src' -> 'dst' per file */
#define CP_F_PRESERVE    0x80   /* -p/-a: preserve mode + mtime */
#define CP_F_UPDATE      0x100
#define CP_F_DRY_RUN     0x200

#define CP_AUTO_PROGRESS_BYTES (8LL * 1024LL * 1024LL)
#define CP_AUTO_PROGRESS_FILES 256
#define CP_RENDER_INTERVAL_NS 100000000ULL
#define CP_PATH_SIZE 4096

static volatile sig_atomic_t cp_interrupted = 0;

static void cp_sigint_handler(int sig) {
    (void)sig;
    cp_interrupted = 1;
}

static int cp_install_sigint(struct sigaction* old_sa) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = cp_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART: wake blocking read/write on Ctrl+C */
    cp_interrupted = 0;
    return sigaction(SIGINT, &sa, old_sa);
}

static void cp_restore_sigint(const struct sigaction* old_sa) {
    if (old_sa) sigaction(SIGINT, old_sa, NULL);
}

/* Last path component, trailing slashes ignored (so "dir/" -> "dir",
 * matching coreutils basename semantics). Copies into a caller buffer —
 * unlike POSIX basename(3), never mutates or aliases the input. */
static void cp_basename(const char* path, char* out, size_t out_cap) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') start = i + 1;
    size_t blen = len - start;
    if (blen >= out_cap) blen = out_cap - 1;
    memcpy(out, path + start, blen);
    out[blen] = '\0';
}

/* ── Progress bar ─────────────────────────────────────────────────────────── */

#define PROGRESS_BAR_WIDTH 24

typedef struct {
    off_t total_bytes;
    off_t copied_bytes;
    unsigned long total_files;
    unsigned long copied_files;
    unsigned long scanned_files;
    int enabled;
    int utf8;
    int scanning;
    uint64_t start_ns;
    uint64_t last_render_ns;
    char current[256];
} cp_progress_t;

static uint64_t cp_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cp_env_truthy(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

static int cp_utf8_ok(void) {
    const char* v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_CTYPE");
    if (!v || !*v) v = getenv("LANG");
    return v && (strstr(v, "UTF-8") || strstr(v, "utf8") || strstr(v, "UTF8"));
}

static int cp_progress_allowed(int flags) {
    const char* mode = getenv("JASH_CP_PROGRESS");
    if ((flags & CP_F_NO_PROGRESS) || (mode && strcmp(mode, "never") == 0)) return 0;
    if (!isatty(STDERR_FILENO)) return 0;
    if (cp_env_truthy("JASH_QUIET") || cp_env_truthy("JASH_AGENT")) return 0;
    return 1;
}

static int cp_progress_should_enable(off_t bytes, unsigned long files, int flags) {
    const char* mode = getenv("JASH_CP_PROGRESS");
    if (!cp_progress_allowed(flags)) return 0;
    if ((flags & CP_F_PROGRESS) || (mode && strcmp(mode, "always") == 0)) return 1;
    return bytes >= CP_AUTO_PROGRESS_BYTES || files >= CP_AUTO_PROGRESS_FILES;
}

static void cp_format_bytes(double n, char* out, size_t outsz) {
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    while (n >= 1024.0 && u < 4) { n /= 1024.0; u++; }
    if (u == 0) snprintf(out, outsz, "%.0f %s", n, units[u]);
    else snprintf(out, outsz, "%.1f %s", n, units[u]);
}

static void cp_format_eta(double seconds, char* out, size_t outsz) {
    if (seconds < 0 || seconds > 86400.0 * 365.0) {
        snprintf(out, outsz, "ETA --");
    } else if (seconds >= 3600.0) {
        snprintf(out, outsz, "ETA %.0fh", seconds / 3600.0);
    } else if (seconds >= 60.0) {
        snprintf(out, outsz, "ETA %.0fm", seconds / 60.0);
    } else {
        snprintf(out, outsz, "ETA %.0fs", seconds);
    }
}

static void cp_progress_render(cp_progress_t* p, int force) {
    if (!p || !p->enabled) return;
    uint64_t now = cp_now_ns();
    if (!force && now - p->last_render_ns < CP_RENDER_INTERVAL_NS) return;
    p->last_render_ns = now;

    const char* spin_utf8[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
    const char* spin_ascii[] = { "-", "\\", "|", "/" };
    int phase = (int)((now - p->start_ns) / CP_RENDER_INTERVAL_NS);
    const char* spin = p->utf8 ? spin_utf8[phase % 10] : spin_ascii[phase % 4];

    if (p->scanning) {
        fprintf(stderr, "\r\033[K%s cp scanning... %lu files", spin, p->scanned_files);
        fflush(stderr);
        return;
    }

    double fraction = 1.0;
    if (p->total_bytes > 0) fraction = (double)p->copied_bytes / (double)p->total_bytes;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    int filled = (int)(fraction * PROGRESS_BAR_WIDTH);
    int pct = (int)(fraction * 100.0);
    double elapsed = (double)(now - p->start_ns) / 1000000000.0;
    double speed = elapsed > 0.0 ? (double)p->copied_bytes / elapsed : 0.0;
    double eta = speed > 0.0 ? ((double)(p->total_bytes - p->copied_bytes) / speed) : -1.0;

    char copied[32], total[32], speed_s[32], eta_s[32];
    cp_format_bytes((double)p->copied_bytes, copied, sizeof(copied));
    cp_format_bytes((double)p->total_bytes, total, sizeof(total));
    cp_format_bytes(speed, speed_s, sizeof(speed_s));
    cp_format_eta(eta, eta_s, sizeof(eta_s));

    fprintf(stderr, "\r\033[K%s cp [", spin);
    for (int i = 0; i < PROGRESS_BAR_WIDTH; i++) {
        if (i < filled) fputs(p->utf8 ? "█" : "=", stderr);
        else if (i == filled && pct < 100) fputs(p->utf8 ? "▌" : ">", stderr);
        else fputs(p->utf8 ? "░" : " ", stderr);
    }
    fprintf(stderr, "] %3d%% %s/%s %s/s %s %lu/%lu files",
            pct, copied, total, speed_s, eta_s, p->copied_files, p->total_files);
    if (p->current[0]) fprintf(stderr, "  %s", p->current);
    fflush(stderr);
}

static void cp_progress_finish(cp_progress_t* p) {
    if (!p || !p->enabled) return;
    cp_progress_render(p, 1);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Errors are distinct from digest values; pread preserves the copy offset. */
static int cp_checksum_fd(int fd, unsigned char out[BLAKE3_OUT_LEN]) {
    unsigned char buf[8192];
    struct blake3 hash;
    blake3_init(&hash);
    off_t offset = 0;
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof(buf), offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        blake3_update(&hash, buf, (size_t)n);
        offset += n;
        if (cp_interrupted) { errno = EINTR; return -1; }
    }
    blake3_final(&hash, out, BLAKE3_OUT_LEN);
    return 0;
}

/* ── Deduplication check ──────────────────────────────────────────────────── */

static int cp_files_identical_fd(int src_fd, const struct stat* st_src,
                                 const char* dst) {
    int dst_fd = open(dst, O_RDONLY);
    if (dst_fd < 0) return 0;
    struct stat st_dst;
    unsigned char src_hash[BLAKE3_OUT_LEN], dst_hash[BLAKE3_OUT_LEN];
    int identical = fstat(dst_fd, &st_dst) == 0 && S_ISREG(st_src->st_mode) &&
                    S_ISREG(st_dst.st_mode) && st_src->st_size == st_dst.st_size &&
                    cp_checksum_fd(src_fd, src_hash) == 0 &&
                    cp_checksum_fd(dst_fd, dst_hash) == 0 &&
                    memcmp(src_hash, dst_hash, sizeof(src_hash)) == 0;
    close(dst_fd);
    return identical;
}

static unsigned long cp_link_serial;

typedef struct {
    int parent_fd;
    int hold_fd;
    char name[CP_PATH_SIZE];
    char hold_name[96];
} cp_temp_t;

static void cp_temp_close(cp_temp_t* temp, int discard_item) {
    if (temp->hold_fd >= 0) {
        if (discard_item) (void)unlinkat(temp->hold_fd, "item", 0);
        close(temp->hold_fd);
    }
    if (temp->parent_fd >= 0) {
        (void)unlinkat(temp->parent_fd, temp->hold_name, AT_REMOVEDIR);
        close(temp->parent_fd);
    }
    temp->hold_fd = -1;
    temp->parent_fd = -1;
}

static int cp_temp_begin(const char* dst, cp_temp_t* temp) {
    char parent[CP_PATH_SIZE];
    size_t len = strlen(dst);
    const char* slash = NULL;
    temp->parent_fd = -1;
    temp->hold_fd = -1;
    while (len > 1 && dst[len - 1] == '/') len--;
    for (size_t i = 0; i < len; i++) if (dst[i] == '/') slash = dst + i;
    if (!slash) {
        strlcpy(parent, ".", sizeof(parent));
        if (len == 0 || len >= sizeof(temp->name)) { errno = ENAMETOOLONG; return -1; }
        memcpy(temp->name, dst, len); temp->name[len] = '\0';
    } else {
        size_t parent_len = slash == dst ? 1U : (size_t)(slash - dst);
        size_t name_len = len - (size_t)(slash - dst) - 1U;
        if (parent_len >= sizeof(parent) || name_len == 0 ||
            name_len >= sizeof(temp->name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(parent, dst, parent_len); parent[parent_len] = '\0';
        memcpy(temp->name, slash + 1, name_len); temp->name[name_len] = '\0';
    }

    int parent_flags = O_RDONLY;
#ifdef O_DIRECTORY
    parent_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    parent_flags |= O_CLOEXEC;
#endif
    temp->parent_fd = open(parent, parent_flags);
    if (temp->parent_fd < 0) return -1;
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        snprintf(temp->hold_name, sizeof(temp->hold_name),
                 ".jash-cp-copy-%ld-%lu", (long)getpid(), ++cp_link_serial);
        if (mkdirat(temp->parent_fd, temp->hold_name, 0700) == 0) break;
        if (errno != EEXIST || attempt == 127) {
            cp_temp_close(temp, 0);
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
    temp->hold_fd = openat(temp->parent_fd, temp->hold_name, hold_flags);
    if (temp->hold_fd < 0 || fchmod(temp->hold_fd, 0700) != 0) {
        if (temp->hold_fd >= 0) close(temp->hold_fd);
        temp->hold_fd = -1;
        cp_temp_close(temp, 0);
        return -1;
    }
    return 0;
}

static int cp_publish_noreplace_link(cp_temp_t* temp) {
    struct stat held, published;
    if (fstatat(temp->hold_fd, "item", &held, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (linkat(temp->hold_fd, "item", temp->parent_fd, temp->name, 0) != 0)
        return -1;
    if (unlinkat(temp->hold_fd, "item", 0) == 0) return 0;

    /* The destination is already public. If consuming the private link fails,
     * undo publication rather than report success with two live names. */
    int cleanup_errno = errno;
    if (fstatat(temp->parent_fd, temp->name, &published,
                AT_SYMLINK_NOFOLLOW) == 0 &&
        held.st_dev == published.st_dev && held.st_ino == published.st_ino &&
        unlinkat(temp->parent_fd, temp->name, 0) == 0) {
        errno = cleanup_errno;
    } else {
        errno = ESTALE;
    }
    return -1;
}

static int cp_publish_noreplace(cp_temp_t* temp) {
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
    int rc = (int)syscall(SYS_renameat2, temp->hold_fd, "item",
                          temp->parent_fd, temp->name, RENAME_NOREPLACE);
    if (rc == 0 || (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP))
        return rc;
#elif defined(__APPLE__)
    return renameatx_np(temp->hold_fd, "item", temp->parent_fd, temp->name,
                        RENAME_EXCL);
#endif
    return cp_publish_noreplace_link(temp);
}

#define CP_LINK_ERROR    (-1)
#define CP_LINK_DONE       0
#define CP_LINK_SKIPPED    1
#define CP_LINK_FALLBACK   2

/* Link the already-open source inode to a temporary name, then atomically
 * replace dst. If descriptor linking is unavailable, leave dst untouched and
 * let the caller copy from src_fd instead of consulting src again. */
static int cp_link_fd_replace(int src_fd, const char* dst, int no_clobber) {
#ifdef __linux__
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
    cp_temp_t temp;
    if (cp_temp_begin(dst, &temp) != 0) return CP_LINK_FALLBACK;
    int linked = linkat(src_fd, "", temp.hold_fd, "item", AT_EMPTY_PATH) == 0;
    if (!linked) {
        char proc_fd[64];
        snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", src_fd);
        linked = linkat(AT_FDCWD, proc_fd, temp.hold_fd, "item",
                        AT_SYMLINK_FOLLOW) == 0;
    }
    if (!linked) {
        cp_temp_close(&temp, 1);
        return CP_LINK_FALLBACK;
    }
    int rc = no_clobber ? cp_publish_noreplace(&temp)
                        : renameat(temp.hold_fd, "item", temp.parent_fd,
                                   temp.name);
    int saved_errno = errno;
    cp_temp_close(&temp, rc != 0);
    errno = saved_errno;
    if (rc == 0) return CP_LINK_DONE;
    if (no_clobber && errno == EEXIST) return CP_LINK_SKIPPED;
    return CP_LINK_ERROR;
#else
    (void)src_fd; (void)dst; (void)no_clobber;
    errno = ENOTSUP;
    return CP_LINK_FALLBACK;
#endif
}

static int cp_scan_tree(const char* path, cp_progress_t* p) {
    if (cp_interrupted) return 130;

    struct stat st;
    if (lstat(path, &st) != 0) return 1;

    if (!S_ISDIR(st.st_mode)) {
        if (S_ISREG(st.st_mode)) p->total_bytes += st.st_size;
        p->total_files++;
        p->scanned_files++;
        cp_progress_render(p, 0);
        return 0;
    }

    DIR* dir = opendir(path);
    if (!dir) return 1;

    int errors = 0;
    struct dirent* ent;
    while (!cp_interrupted && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }

        char child[CP_PATH_SIZE];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            errors++;
            continue;
        }
        int rc = cp_scan_tree(child, p);
        if (rc == 130) {
            errors = 130;
            break;
        }
        errors += rc;
    }

    closedir(dir);
    if (cp_interrupted) return 130;
    return errors;
}

/* ── Core copy logic ──────────────────────────────────────────────────────── */

#define COPY_BUFFER_SIZE (64 * 1024)  /* 64KB buffer */

static int cp_newer(const struct stat* src, const struct stat* dst) {
    if (src->st_mtime != dst->st_mtime) return src->st_mtime > dst->st_mtime;
#ifdef __APPLE__
    return src->st_mtimespec.tv_nsec > dst->st_mtimespec.tv_nsec;
#else
    return src->st_mtim.tv_nsec > dst->st_mtim.tv_nsec;
#endif
}

static int cp_item_fail(jash_mutation_item_t* item, const char* code) {
    if (item) item->code = code;
    return 1;
}

static int cp_copy_file_impl(const char* src, const char* dst, int flags,
                        jash_writer_t w, void* ctx, cp_progress_t* progress,
                        jash_mutation_item_t* item) {
    int resume_mode = flags & CP_F_RESUME;
    int dedup_mode = flags & CP_F_DEDUP;
    int verify_mode = flags & CP_F_VERIFY;
    int no_clobber = flags & CP_F_NOCLOBBER;

    int preview = flags & CP_F_DRY_RUN;
    int src_fd = preview ? -1 : open(src, O_RDONLY);
    if (!preview && src_fd < 0) {
        if (cp_interrupted || errno == EINTR) return 130;
        jash_writef(w, ctx, "cp: cannot open '%s'\n", src);
        return cp_item_fail(item, "open_source");
    }
    struct stat st_src;
    if ((preview ? stat(src, &st_src) : fstat(src_fd, &st_src)) != 0) {
        if (src_fd >= 0) close(src_fd);
        jash_writef(w, ctx, "cp: cannot inspect '%s'\n", src);
        return cp_item_fail(item, "inspect_source");
    }
    if (progress && progress->total_bytes == 0 && S_ISREG(st_src.st_mode)) {
        progress->total_bytes = st_src.st_size;
        progress->total_files = 1;
    }

    /* ADR-0042 F8: snapshot destination before overwrite so `tread` and
     * `undo` can recover the previous content. Capture once for dedup+normal. */
    struct stat st_dst;
    int dst_seen = stat(dst, &st_dst) == 0;
    if ((flags & CP_F_UPDATE) && dst_seen && !cp_newer(&st_src, &st_dst)) {
        if (item) { item->status = JASH_MUT_SKIPPED; item->code = "not_newer"; }
        if (src_fd >= 0) close(src_fd);
        return 0;
    }
    if (dst_seen && st_src.st_dev == st_dst.st_dev &&
        st_src.st_ino == st_dst.st_ino) {
        if (src_fd >= 0) close(src_fd);
        if (no_clobber) {
            if (item) { item->status = JASH_MUT_SKIPPED; item->code = "destination_exists"; }
            return 0;
        }
        jash_writef(w, ctx, "cp: '%s' and '%s' are the same file\n", src, dst);
        return cp_item_fail(item, "same_file");
    }
    if (flags & CP_F_DRY_RUN) {
        struct stat entry;
        int skip = no_clobber && lstat(dst, &entry) == 0;
        if (item) {
            item->status = skip ? JASH_MUT_SKIPPED : JASH_MUT_PLANNED;
            item->code = skip ? "destination_exists" : NULL;
        }
        jash_writef(w, ctx, "cp: would %s '%s' -> '%s'\n", skip ? "skip" : "copy", src, dst);
        return 0;
    }
    int dst_existed = !no_clobber && dst_seen && S_ISREG(st_dst.st_mode);
    char backup[1024];
    int has_backup = 0;
    if (dst_existed && !resume_mode) {
        if (jash_timeline_backup_file(dst, backup, sizeof(backup)) == 0)
            has_backup = 1;
    }

    /* Dedup: check if files are identical */
    if (dedup_mode) {
        if (cp_interrupted) { close(src_fd); return 130; }
        if (cp_files_identical_fd(src_fd, &st_src, dst)) {
            int link_rc = cp_link_fd_replace(src_fd, dst, no_clobber);
            if (link_rc == CP_LINK_DONE) {
                if (item) { item->action = "hardlink"; item->effect = JASH_MUT_CHANGED;
                    item->verification = verify_mode ? 1 : 0; }
                if (progress) {
                    progress->copied_bytes += st_src.st_size;
                    progress->copied_files++;
                    cp_progress_render(progress, 0);
                }
                jash_writef(w, ctx, "cp: hardlinked '%s' → '%s' (shares data, edits to one affect both)\n",
                           src, dst);
                if (dst_existed) {
                    char desc[256];
                    snprintf(desc, sizeof(desc), "cp overwrite (hardlink) %s", dst);
                    jash_timeline_record_ref(JASH_UNDO_FILE_OVERWRITE, desc, dst,
                                         has_backup ? backup : "", item ? &item->undo : NULL);
                } else {
                    char desc[256];
                    snprintf(desc, sizeof(desc), "cp create (hardlink) %s", dst);
                    jash_timeline_record_ref(JASH_UNDO_FILE_CREATE, desc, dst, NULL, item ? &item->undo : NULL);
                }
                close(src_fd);
                return 0;
            }
            if (link_rc == CP_LINK_SKIPPED) {
                if (item) { item->status = JASH_MUT_SKIPPED; item->code = "destination_exists"; }
                close(src_fd);
                return 0;
            }
            if (link_rc == CP_LINK_ERROR && no_clobber) {
                close(src_fd);
                jash_writef(w, ctx, "cp: cannot publish '%s' without clobbering\n", dst);
                return cp_item_fail(item, "publish_destination");
            }
            /* Fallback to normal copy if hardlink fails */
        }
    }
    
    int dst_fd;
    off_t start_offset = 0;
    cp_temp_t output_temp;
    int temp_output = 0;

    if (no_clobber) {
        if (cp_temp_begin(dst, &output_temp) != 0) {
            close(src_fd);
            jash_writef(w, ctx, "cp: cannot prepare destination '%s'\n", dst);
            return cp_item_fail(item, "prepare_destination");
        }
        int out_flags = (verify_mode ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        out_flags |= O_NOFOLLOW;
#endif
        dst_fd = openat(output_temp.hold_fd, "item", out_flags, 0600);
        if (dst_fd < 0) {
            cp_temp_close(&output_temp, 1);
            close(src_fd);
            jash_writef(w, ctx, "cp: cannot prepare destination '%s'\n", dst);
            return cp_item_fail(item, "prepare_destination");
        }
        temp_output = 1;
    } else if (resume_mode) {
        /* Resume: open for append, get current size */
        dst_fd = open(dst, (verify_mode ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND, 0644);
        if (dst_fd >= 0) {
            if (item) item->effect = JASH_MUT_POSSIBLE;
            struct stat st_resume;
            if (fstat(dst_fd, &st_resume) == 0) {
                start_offset = st_resume.st_size;
                lseek(src_fd, start_offset, SEEK_SET);
            }
        }
    } else {
        /* Check the opened inode BEFORE truncating (including hardlink aliases
         * and destinations replaced since the pathname probe). */
        dst_fd = open(dst, (verify_mode ? O_RDWR : O_WRONLY) | O_CREAT, 0644);
        if (dst_fd >= 0) {
            if (item) item->effect = JASH_MUT_POSSIBLE;
            struct stat opened;
            if (fstat(dst_fd, &opened) != 0 ||
                (opened.st_dev == st_src.st_dev && opened.st_ino == st_src.st_ino) ||
                (S_ISREG(opened.st_mode) && ftruncate(dst_fd, 0) != 0)) {
                close(dst_fd);
                close(src_fd);
                jash_writef(w, ctx, "cp: cannot safely overwrite '%s'\n", dst);
                return cp_item_fail(item, "overwrite_destination");
            }
            if (item && S_ISREG(opened.st_mode)) item->effect = JASH_MUT_CHANGED;
        }
    }
    
    if (dst_fd < 0) {
        close(src_fd);
        if (cp_interrupted || errno == EINTR) return 130;
        jash_writef(w, ctx, "cp: cannot create '%s'\n", dst);
        return cp_item_fail(item, "open_destination");
    }
    
    char buffer[COPY_BUFFER_SIZE];
    ssize_t bytes_read;
    if (progress && progress->copied_bytes < start_offset) {
        progress->copied_bytes = start_offset;
    }
    if (progress) {
        const char* slash = strrchr(src, '/');
        snprintf(progress->current, sizeof(progress->current), "%s", slash ? slash + 1 : src);
        cp_progress_render(progress, 0);
    }
    
    while (!cp_interrupted && !jash_mutation_cancel_status()) {
        bytes_read = read(src_fd, buffer, sizeof(buffer));
        if (bytes_read < 0 && errno == EINTR && cp_interrupted) break;
        if (bytes_read <= 0) break;

        ssize_t written_total = 0;
        while (written_total < bytes_read && !cp_interrupted && !jash_mutation_cancel_status()) {
            ssize_t bytes_written = write(dst_fd, buffer + written_total, (size_t)(bytes_read - written_total));
            if (bytes_written < 0 && errno == EINTR && cp_interrupted) break;
            if (bytes_written <= 0) {
                jash_writef(w, ctx, "cp: write error on '%s'\n", dst);
                close(src_fd);
                close(dst_fd);
                if (temp_output) cp_temp_close(&output_temp, 1);
                return cp_item_fail(item, "write_destination");
            }

            written_total += bytes_written;
            if (item) item->bytes_written += (uint64_t)bytes_written;
            if (item && !temp_output) item->effect = JASH_MUT_CHANGED;
            if (progress) {
                progress->copied_bytes += bytes_written;
                cp_progress_render(progress, 0);
            }
        }
    }

    if (cp_interrupted || jash_mutation_cancel_status()) {
        close(src_fd);
        close(dst_fd);
        if (temp_output) cp_temp_close(&output_temp, 1);
        return jash_mutation_cancel_status() ? jash_mutation_cancel_status() : 130;
    }

    if (bytes_read < 0) {
        jash_writef(w, ctx, "cp: read error on '%s'\n", src);
        close(src_fd);
        close(dst_fd);
        if (temp_output) cp_temp_close(&output_temp, 1);
        return cp_item_fail(item, "read_source");
    }
    
    if (verify_mode) {
        unsigned char src_hash[BLAKE3_OUT_LEN], dst_hash[BLAKE3_OUT_LEN];
        int verified = cp_checksum_fd(src_fd, src_hash) == 0 &&
                       cp_checksum_fd(dst_fd, dst_hash) == 0 &&
                       memcmp(src_hash, dst_hash, sizeof(src_hash)) == 0;
        if (!verified) {
            if (item) { item->verification = -1; item->code = "verification_failed"; }
            close(src_fd);
            close(dst_fd);
            if (temp_output) cp_temp_close(&output_temp, 1);
            jash_writef(w, ctx, "cp: BLAKE3 verification FAILED for '%s'\n", dst);
            return cp_interrupted ? 130 : 1;
        }
        if (item) item->verification = 1;
    }
    close(src_fd);

    /* Apply metadata while the descriptor still identifies the file we wrote.
     * A pathname chmod/utimes here could modify an unrelated file if dst were
     * replaced between close and metadata restoration. */
    if (flags & CP_F_PRESERVE) {
        struct timespec tv[2];
#ifdef __APPLE__
        tv[0] = st_src.st_atimespec; tv[1] = st_src.st_mtimespec;
#else
        tv[0] = st_src.st_atim; tv[1] = st_src.st_mtim;
#endif
        if (futimens(dst_fd, tv) != 0) {
            jash_writef(w, ctx, "cp: cannot preserve times for '%s'\n", dst);
            close(dst_fd);
            if (temp_output) cp_temp_close(&output_temp, 1);
            return cp_item_fail(item, "preserve_times");
        }
    }
    /* SECURITY: strip set-ID bits unless -p/-a, and even then retain them only
     * when they still refer to the same user/group. Changing ownership while
     * blindly restoring set-ID would change the privilege the bit confers. */
    mode_t dst_mode = st_src.st_mode & 0777;
    if (flags & CP_F_PRESERVE) {
        struct stat st_copied;
        if (fstat(dst_fd, &st_copied) != 0) {
            jash_writef(w, ctx, "cp: cannot inspect copied file '%s'\n", dst);
            close(dst_fd);
            if (temp_output) cp_temp_close(&output_temp, 1);
            return cp_item_fail(item, "inspect_destination");
        }
        dst_mode |= st_src.st_mode & S_ISVTX;
        if (st_copied.st_uid == st_src.st_uid) dst_mode |= st_src.st_mode & S_ISUID;
        if (st_copied.st_gid == st_src.st_gid) dst_mode |= st_src.st_mode & S_ISGID;
    }
    /* Restore mode last because data writes may clear special bits. */
    if (fchmod(dst_fd, dst_mode) != 0) {
        jash_writef(w, ctx, "cp: cannot preserve mode for '%s'\n", dst);
        close(dst_fd);
        if (temp_output) cp_temp_close(&output_temp, 1);
        return cp_item_fail(item, "preserve_mode");
    }
    if (close(dst_fd) != 0) {
        jash_writef(w, ctx, "cp: write error on '%s'\n", dst);
        if (temp_output) cp_temp_close(&output_temp, 1);
        return cp_item_fail(item, "close_destination");
    }
    if (temp_output) {
        int publish_rc = cp_publish_noreplace(&output_temp);
        int publish_errno = errno;
        cp_temp_close(&output_temp, publish_rc != 0);
        errno = publish_errno;
        if (publish_rc != 0) {
            if (errno == EEXIST) {
                if (item) { item->status = JASH_MUT_SKIPPED; item->code = "destination_exists"; }
                return 0;
            }
            jash_writef(w, ctx, "cp: cannot publish '%s' without clobbering\n", dst);
            if (item) item->effect = JASH_MUT_POSSIBLE;
            return cp_item_fail(item, "publish_destination");
        }
    }
    /* -v / --verbose: one line per copied file (GNU format). */
    if (flags & CP_F_VERBOSE) {
        jash_writef(w, ctx, "'%s' -> '%s'\n", src, dst);
    }
    if (progress) {
        progress->copied_files++;
        cp_progress_render(progress, 0);
    }
    
    /* The comparison used pinned descriptors before publication/close. */
    if (verify_mode) {
        jash_writef(w, ctx, "cp: verified '%s' (BLAKE3 OK)\n", dst);
    }

    /* ADR-0042 F8: journal overwrite/create for `tread`/`undo`. */
    if (dst_existed) {
        char desc[256];
        snprintf(desc, sizeof(desc), "cp overwrite %s", dst);
        jash_timeline_record_ref(JASH_UNDO_FILE_OVERWRITE, desc, dst,
                             has_backup ? backup : "", item ? &item->undo : NULL);
    } else {
        char desc[256];
        snprintf(desc, sizeof(desc), "cp create %s", dst);
        jash_timeline_record_ref(JASH_UNDO_FILE_CREATE, desc, dst, NULL, item ? &item->undo : NULL);
    }

    return 0;
}

static int cp_copy_file(const char* src, const char* dst, int flags,
                        jash_writer_t w, void* ctx, cp_progress_t* progress,
                        jash_mutation_report_t* report) {
    if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
    jash_mutation_item_t item = {.source = src, .destination = dst, .action = "copy",
                                .code = "copy_failed", .bytes_known = 1};
    int rc = cp_copy_file_impl(src, dst, flags, w, ctx, progress, report ? &item : NULL);
    if (!rc && item.status == JASH_MUT_FAILED) {
        item.status = JASH_MUT_COMPLETED; item.code = NULL; item.effect = JASH_MUT_CHANGED;
    }
    if (rc == 130 || rc == 143) item.code = "interrupted";
    if (jash_mutation_report_item(report, &item) != 0) return 125;
    return rc;
}

static int cp_report(jash_mutation_report_t* report, const char* src, const char* dst,
                     const char* action, jash_mutation_status_t status,
                     jash_mutation_effect_t effect, const char* code) {
    jash_mutation_item_t item = {.source = src, .destination = dst, .action = action,
                                .status = status, .effect = effect, .code = code};
    return jash_mutation_report_item(report, &item) != 0 ? 125 : 0;
}

/* ── Recursive copy ───────────────────────────────────────────────────────── */

/* Reject copying a directory into itself BEFORE creating any output. Walking
 * parent inodes also handles symlink aliases of the destination parent. */
static int cp_tree_destination_ok(const char* src, const char* dst) {
    struct stat source, current, parent;
    if (stat(src, &source) != 0) return 0;
    char path[CP_PATH_SIZE];
    if (strlen(dst) >= sizeof(path)) return 0;
    strlcpy(path, dst, sizeof(path));
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        char* slash = strrchr(path, '/');
        if (!slash) strlcpy(path, ".", sizeof(path));
        else if (slash == path) slash[1] = '\0';
        else *slash = '\0';
        fd = open(path, O_RDONLY | O_DIRECTORY);
    }
    if (fd < 0) return 0;
    int ok = 0;
    for (;;) {
        if (fstat(fd, &current) != 0) break;
        if (source.st_dev == current.st_dev && source.st_ino == current.st_ino) break;
        int up = openat(fd, "..", O_RDONLY | O_DIRECTORY);
        if (up < 0) break;
        if (fstat(up, &parent) != 0) { close(up); break; }
        if (parent.st_dev == current.st_dev && parent.st_ino == current.st_ino) {
            close(up); ok = 1; break;
        }
        close(fd); fd = up;
    }
    close(fd);
    return ok;
}

static int cp_copy_symlink(const char* src, const char* dst, int flags,
                           jash_writer_t w, void* ctx, jash_mutation_report_t* report) {
    if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
    struct stat previous;
    int existed = lstat(dst, &previous) == 0;
    struct stat source;
    if (existed && ((flags & CP_F_NOCLOBBER) || ((flags & CP_F_UPDATE) &&
        lstat(src, &source) == 0 && !cp_newer(&source, &previous)))) {
        return cp_report(report, src, dst, "symlink", JASH_MUT_SKIPPED, JASH_MUT_NONE,
                         flags & CP_F_NOCLOBBER ? "destination_exists" : "not_newer");
    }
    if (flags & CP_F_DRY_RUN) {
        jash_writef(w, ctx, "cp: would copy symlink '%s' -> '%s'\n", src, dst);
        return cp_report(report, src, dst, "symlink", JASH_MUT_PLANNED, JASH_MUT_NONE, NULL);
    }
    char target[CP_PATH_SIZE];
    ssize_t n = readlink(src, target, sizeof(target));
    if (n < 0 || (size_t)n >= sizeof(target)) {
        cp_report(report, src, dst, "symlink", JASH_MUT_FAILED, JASH_MUT_NONE, "read_symlink");
        return 1;
    }
    target[n] = '\0';
    cp_temp_t temp;
    if (cp_temp_begin(dst, &temp) != 0) {
        cp_report(report, src, dst, "symlink", JASH_MUT_FAILED, JASH_MUT_NONE, "prepare_destination");
        return 1;
    }
    int rc = symlinkat(target, temp.hold_fd, "item");
    if (rc == 0)
        rc = flags & CP_F_NOCLOBBER ? cp_publish_noreplace(&temp) :
             renameat(temp.hold_fd, "item", temp.parent_fd, temp.name);
    int saved = errno;
    cp_temp_close(&temp, rc != 0);
    if (rc && (flags & CP_F_NOCLOBBER) && saved == EEXIST)
        return cp_report(report, src, dst, "symlink", JASH_MUT_SKIPPED, JASH_MUT_NONE, "destination_exists");
    if (rc) {
        jash_writef(w, ctx, "cp: cannot copy symlink '%s'\n", src);
        cp_report(report, src, dst, "symlink", JASH_MUT_FAILED, JASH_MUT_POSSIBLE, "publish_destination");
        return 1;
    }
    if (flags & CP_F_VERBOSE) jash_writef(w, ctx, "'%s' -> '%s'\n", src, dst);
    if (!existed) {
        char desc[256];
        snprintf(desc, sizeof(desc), "cp create symlink %s", dst);
        /* Link undo unlinks the entry itself, including dangling links. */
        jash_timeline_record(JASH_UNDO_LINK_CREATE, desc, dst, NULL);
    }
    return cp_report(report, src, dst, "symlink", JASH_MUT_COMPLETED, JASH_MUT_CHANGED, NULL);
}

static int cp_recursive(const char* src, const char* dst, int flags,
                        jash_writer_t w, void* ctx, cp_progress_t* progress,
                        jash_mutation_report_t* report) {
    if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
    if (cp_interrupted) return 130;

    struct stat st;
    if (lstat(src, &st) != 0) {
        jash_writef(w, ctx, "cp: cannot stat '%s'\n", src);
        cp_report(report, src, dst, "copy", JASH_MUT_FAILED, JASH_MUT_NONE, "inspect_source");
        return 1;
    }
    
    if (S_ISLNK(st.st_mode)) return cp_copy_symlink(src, dst, flags, w, ctx, report);
    if (!S_ISDIR(st.st_mode)) {
        /* Regular file */
        return cp_copy_file(src, dst, flags, w, ctx, progress, report);
    }
    
    if (!cp_tree_destination_ok(src, dst)) {
        jash_writef(w, ctx, "cp: refusing self-copy or inaccessible destination '%s'\n", dst);
        cp_report(report, src, dst, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "invalid_tree_destination");
        return 1;
    }
    if (flags & CP_F_DRY_RUN) {
        jash_writef(w, ctx, "cp: would copy directory '%s' -> '%s'\n", src, dst);
        return cp_report(report, src, dst, "copy_tree", JASH_MUT_PLANNED, JASH_MUT_NONE, NULL);
    }
    /* Directory: create destination */
    int made_dir = mkdir(dst, st.st_mode) == 0;
    if (!made_dir) {
        struct stat st_dst;
        if (stat(dst, &st_dst) != 0 || !S_ISDIR(st_dst.st_mode)) {
            jash_writef(w, ctx, "cp: cannot create directory '%s'\n", dst);
            cp_report(report, src, dst, "ensure_directory", JASH_MUT_FAILED, JASH_MUT_NONE, "create_directory");
            return 1;
        }
    }
    if (cp_report(report, src, dst, "ensure_directory", JASH_MUT_COMPLETED,
                  made_dir ? JASH_MUT_CHANGED : JASH_MUT_NONE, NULL)) return 125;
    
    DIR* dir = opendir(src);
    if (!dir) {
        jash_writef(w, ctx, "cp: cannot open directory '%s'\n", src);
        cp_report(report, src, dst, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "open_directory");
        return 1;
    }
    
    int errors = 0;
    struct dirent* ent;
    
    while (!cp_interrupted && !jash_mutation_report_stop(report)) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno) {
                cp_report(report, src, dst, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "read_directory");
                errors = 1;
            }
            break;
        }
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }
        
        char src_child[CP_PATH_SIZE];
        char dst_child[CP_PATH_SIZE];
        int sn = snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
        int dn = snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, ent->d_name);
        if (sn < 0 || dn < 0 || (size_t)sn >= sizeof(src_child) || (size_t)dn >= sizeof(dst_child)) {
            jash_writef(w, ctx, "cp: path too long under '%s'\n", src);
            errors++;
            cp_report(report, src, dst, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "path_too_long");
            continue;
        }
        
        int rc = cp_recursive(src_child, dst_child, flags, w, ctx, progress, report);
        if (rc == 130 || rc == 125) {
            errors = rc;
            break;
        }
        if (rc) errors = 1;
    }
    if (cp_interrupted) errors = 130;
    if (jash_mutation_report_stop(report)) errors = jash_mutation_report_stop(report);
    if (errors == 125 || errors == 130 || errors == 143 || jash_mutation_report_failed(report)) {
        closedir(dir);
        return jash_mutation_report_failed(report) ? 125 : errors;
    }
    
    if (flags & CP_F_PRESERVE) {
        int fd = open(dst, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        struct timespec tv[2];
#ifdef __APPLE__
        tv[0] = st.st_atimespec; tv[1] = st.st_mtimespec;
#else
        tv[0] = st.st_atim; tv[1] = st.st_mtim;
#endif
        if (fd < 0 || futimens(fd, tv) != 0 || fchmod(fd, st.st_mode & 0777) != 0) {
            jash_writef(w, ctx, "cp: cannot preserve directory metadata '%s'\n", dst);
            errors++;
            cp_report(report, src, dst, "preserve_directory", JASH_MUT_FAILED, JASH_MUT_POSSIBLE, "preserve_metadata");
        } else {
            cp_report(report, src, dst, "preserve_directory", JASH_MUT_COMPLETED, JASH_MUT_CHANGED, NULL);
        }
        if (fd >= 0) close(fd);
    }
    closedir(dir);
    if (cp_interrupted) return 130;
    return jash_mutation_report_failed(report) ? 125 : errors ? 1 : 0;
}

/* ── Main cp builtin ──────────────────────────────────────────────────────── */

static int cp_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                   jash_mutation_report_t* report) {
    (void)s;

    if (argc == 0) {
        jash_writes(w, ctx, "Usage: cp [-r|-a] [--progress|--no-progress] [--resume] [--dedup] [--verify] <src> <dst>\n");
        return 1;
    }

    /* Parse arguments */
    int recursive = 0;
    int flags = report ? CP_F_NO_PROGRESS : 0;

    /* ADR-0036 W2: argv arrives already tokenized quote-aware by the
     * dispatcher (core/argv.c). The old plain `strtok(args_copy, " \t")`
     * here had ZERO quote awareness — `cp "my file.txt" dest/` was already
     * broken (BUG-038-class) before this fix. */

    /* Extract flags */
    int file_start = 0;
    int end_opts = 0;
    for (int i = 0; i < argc; i++) {
        if (end_opts) { file_start = i; break; }
        if (strcmp(argv[i], "--") == 0) { end_opts = 1; file_start = i + 1; continue; }
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-R") == 0 ||
            strcmp(argv[i], "--recursive") == 0) {
            recursive = 1;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--archive") == 0) {
            /* -a = -rp: recursive + preserve mode & timestamps. */
            recursive = 1;
            flags |= CP_F_PRESERVE;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-clobber") == 0) {
            flags |= CP_F_NOCLOBBER;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            flags |= CP_F_VERBOSE;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--preserve") == 0 ||
                   strncmp(argv[i], "--preserve=", 11) == 0) {
            flags |= CP_F_PRESERVE;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--update") == 0) {
            flags |= CP_F_UPDATE; file_start = i + 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            flags |= CP_F_DRY_RUN | CP_F_NO_PROGRESS;
            if (report) report->preview = 1;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--progress") == 0) {
            flags |= CP_F_PROGRESS;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--no-progress") == 0) {
            flags |= CP_F_NO_PROGRESS;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--resume") == 0) {
            flags |= CP_F_RESUME;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--dedup") == 0) {
            flags |= CP_F_DEDUP;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--verify") == 0) {
            flags |= CP_F_VERIFY;
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            /* Short-flag cluster: -r/-R (recursive), -f (force — jash cp already
             * overwrites, so honored as the default), and common no-op-safe
             * flags -v/-p/-a. -rf, -fr, etc. all work. (BUG: `cp -f` rejected.) */
            int ok = 1;
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'a': flags |= CP_F_PRESERVE; recursive = 1; break;
                    case 'r': case 'R': recursive = 1; break;
                    case 'p': flags |= CP_F_PRESERVE; break;
                    case 'n': flags |= CP_F_NOCLOBBER; break;
                    case 'v': flags |= CP_F_VERBOSE; break;
                    case 'u': flags |= CP_F_UPDATE; break;
                    case 'f': break;  /* force — cp overwrites by default */
                    default: ok = 0; break;
                }
                if (!ok) break;
            }
            if (!ok) {
                jash_writef(w, ctx, "cp: unknown option '%s'\n", argv[i]);
                return 1;
            }
            file_start = i + 1;
        } else if (argv[i][0] == '-') {
            jash_writef(w, ctx, "cp: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            file_start = i;
            break;
        }
    }
    
    if (argc - file_start < 2) {
        jash_writes(w, ctx, "cp: missing source or destination\n");
        return 1;
    }

    const char* dst_arg = argv[argc - 1];
    if (report) flags &= ~CP_F_VERBOSE;
    int nsrc = argc - file_start - 1;
    struct stat st_dst_probe;
    int dst_is_dir = (stat(dst_arg, &st_dst_probe) == 0 && S_ISDIR(st_dst_probe.st_mode));

    if (nsrc > 1 && !dst_is_dir) {
        /* Multiple sources but dst isn't a directory — coreutils rejects this
         * rather than silently dropping operands. */
        jash_writef(w, ctx, "cp: target '%s' is not a directory\n", dst_arg);
        return 1;
    }

    /* ADR-0044: multi-source `cp a b c dir/` — copy EACH source into the dir.
     * The old code took only argv[file_start] as the single source and silently
     * dropped the rest. (Progress UI is reserved for the single-source path.) */
    if (nsrc > 1) {
        int overall = 0;
        size_t dlen = strlen(dst_arg);
        while (dlen > 1 && dst_arg[dlen - 1] == '/') dlen--;
        for (int k = file_start; k < argc - 1; k++) {
            if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
            if (cp_interrupted) return 130;
            const char* srck = argv[k];
            char base[CP_PATH_SIZE], db[CP_PATH_SIZE];
            cp_basename(srck, base, sizeof base);
            int dn = snprintf(db, sizeof db, "%.*s/%s", (int)dlen, dst_arg, base);
            if (dn < 0 || (size_t)dn >= sizeof(db)) {
                cp_report(report, srck, dst_arg, "copy", JASH_MUT_FAILED, JASH_MUT_NONE, "path_too_long");
                overall = 1; continue;
            }
            struct stat ss;
            if ((recursive ? lstat(srck, &ss) : stat(srck, &ss)) != 0) {
                jash_writef(w, ctx, "cp: cannot access '%s'\n", srck);
                cp_report(report, srck, db, "copy", JASH_MUT_FAILED, JASH_MUT_NONE, "inspect_source");
                overall = 1; continue;
            }
            if (S_ISDIR(ss.st_mode) && !recursive) {
                jash_writef(w, ctx, "cp: '%s' is a directory (use -r or -a for recursive)\n", srck);
                cp_report(report, srck, db, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "recursive_required");
                overall = 1; continue;
            }
            int rc = recursive ? cp_recursive(srck, db, flags, w, ctx, NULL, report)
                                         : cp_copy_file(srck, db, flags, w, ctx, NULL, report);
            if (rc == 125 || rc == 130) return rc;
            if (rc != 0) overall = rc;
        }
        return jash_mutation_report_failed(report) ? 125 : overall;
    }

    const char* src = argv[file_start];

    /* BUG-043: `cp file existing_dir/` — if dst is a directory, copy as
     * dst/basename(src). */
    char dst_buf[CP_PATH_SIZE];
    const char* dst = dst_arg;
    if (dst_is_dir) {
        char base[CP_PATH_SIZE];
        cp_basename(src, base, sizeof(base));
        size_t dlen = strlen(dst_arg);
        while (dlen > 1 && dst_arg[dlen - 1] == '/') dlen--;
        if (dlen + 1 + strlen(base) < sizeof(dst_buf)) {
            memcpy(dst_buf, dst_arg, dlen);
            dst_buf[dlen] = '/';
            memcpy(dst_buf + dlen + 1, base, strlen(base) + 1);
            dst = dst_buf;
        } else {
            cp_report(report, src, dst_arg, "copy", JASH_MUT_FAILED, JASH_MUT_NONE, "path_too_long");
            return 1;
        }
    }

    /* Check if source is directory */
    struct stat st_src;
    if ((recursive ? lstat(src, &st_src) : stat(src, &st_src)) != 0) {
        if (cp_interrupted) {
            jash_writes(w, ctx, "cp: interrupted\n");
            return 130;
        }
        jash_writef(w, ctx, "cp: cannot access '%s'\n", src);
        cp_report(report, src, dst, "copy", JASH_MUT_FAILED, JASH_MUT_NONE, "inspect_source");
        return 1;
    }
    
    if (S_ISDIR(st_src.st_mode) && !recursive) {
        jash_writef(w, ctx, "cp: '%s' is a directory (use -r or -a for recursive)\n", src);
        cp_report(report, src, dst, "copy_tree", JASH_MUT_FAILED, JASH_MUT_NONE, "recursive_required");
        return 1;
    }
    
    cp_progress_t progress;
    memset(&progress, 0, sizeof(progress));
    progress.utf8 = cp_utf8_ok();
    progress.start_ns = cp_now_ns();

    if (recursive && cp_progress_allowed(flags)) {
        progress.enabled = 1;
        progress.scanning = progress.enabled;
        int scan_rc = cp_scan_tree(src, &progress);
        progress.scanning = 0;
        if (scan_rc == 130) {
            cp_progress_finish(&progress);
            jash_writes(w, ctx, "cp: interrupted\n");
            return 130;
        }
        progress.enabled = cp_progress_should_enable(progress.total_bytes, progress.total_files, flags);
        if (!progress.enabled) {
            fprintf(stderr, "\r\033[K");
            fflush(stderr);
        }
        progress.start_ns = cp_now_ns();
        progress.last_render_ns = 0;
        if (progress.enabled) cp_progress_render(&progress, 1);
    } else if (!recursive) {
        progress.total_bytes = S_ISREG(st_src.st_mode) ? st_src.st_size : 0;
        progress.total_files = 1;
        progress.enabled = cp_progress_should_enable(progress.total_bytes, progress.total_files, flags);
        progress.start_ns = cp_now_ns();
        if (progress.enabled) cp_progress_render(&progress, 1);
    }

    /* Copy */
    int rc;
    if (recursive) {
        rc = cp_recursive(src, dst, flags, w, ctx, progress.enabled ? &progress : NULL, report);
    } else {
        rc = cp_copy_file(src, dst, flags, w, ctx, progress.enabled ? &progress : NULL, report);
    }
    cp_progress_finish(&progress);
    if (rc == 130) {
        jash_writes(w, ctx, "cp: interrupted\n");
    }
    return rc;
}

static int cp_run_interruptible(jash_state_t* s, jash_writer_t w, void* ctx,
                                int argc, char** argv, jash_mutation_report_t* report) {
    struct sigaction old_int;
    int installed = cp_install_sigint(&old_int) == 0;
    int rc = cp_run(s, w, ctx, argc, argv, report);
    if (installed) cp_restore_sigint(&old_int);
    return rc;
}

int jash_b_cp(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int filtered_count;
    char** filtered;
    jash_mutation_format_t format;
    int json = jash_mutation_args_mode(argc, argv, NULL, &filtered_count, &filtered, &format);
    int args_error = errno;
    if (!json) return cp_run_interruptible(s, w, ctx, argc, argv, NULL);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, "cp", "filesystem_action", format, w, ctx) != 0) { free(filtered); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    int rc = cp_run_interruptible(s, jash_stderr_writer, NULL, filtered_count, filtered, &report);
    free(filtered);
    return jash_mutation_report_end(&report, rc);
}
