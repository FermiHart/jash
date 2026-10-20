#!/usr/bin/env bash
# Deterministic regressions for cp/mv metadata races and kill target validation.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
JASH=${JASH:-$ROOT/build/jash}
if [[ ! -x "$JASH" ]]; then
    echo "FAIL: required jash binary is missing or not executable: $JASH"
    exit 1
fi
for source in "$ROOT/c/builtins/builtin_cp.c" "$ROOT/c/builtins/builtin_mv.c" \
              "$ROOT/c/builtins/builtin_kill.c" "$ROOT/c/main.c"; do
    if [[ ! "$JASH" -nt "$source" ]]; then
        echo "FAIL: stale jash binary $JASH (not newer than $source)"
        exit 1
    fi
done
if ! command -v cc >/dev/null 2>&1; then
    echo "FAIL: required C compiler is unavailable"
    exit 1
fi
PLATFORM=$(uname -s)
T=$(mktemp -d "${TMPDIR:-/tmp}/jash-cp-mv-kill.XXXXXX") || exit 2
trap 'chmod -R u+rwX "$T" 2>/dev/null || true; rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/home/.jash"

pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  PASS %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }

# This source-level boundary is portable and must run even where the remaining
# deterministic syscall races cannot use Linux LD_PRELOAD.
cat >"$T/platform_signal_bounds.c" <<'C'
#include <signal.h>
#undef NSIG
#define NSIG 48
#undef SIGRTMAX
#define SIGRTMAX 47
#include "builtins/builtin_kill.c"
int main(void) {
    int sig = -1;
    return !(kill_parse_signal("-47", &sig) && sig == 47 &&
             !kill_parse_signal("-48", &sig));
}
C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$ROOT/c" \
      "$T/platform_signal_bounds.c" -o "$T/platform_signal_bounds" \
      >/dev/null 2>&1 && "$T/platform_signal_bounds"; then
    ok "kill accepts and rejects numeric signals at the platform boundary"
else
    no "kill numeric parser ignored NSIG or SIGRTMAX"
fi

if [[ $PLATFORM != Linux ]]; then
    echo "SKIP: remaining deterministic syscall interposers require Linux LD_PRELOAD"
    echo "cp/mv/kill portable regressions: PASS=$pass FAIL=$fail"
    [[ $fail -eq 0 ]]
    exit $?
fi
case "$PLATFORM" in
    Darwin|FreeBSD|NetBSD|OpenBSD|DragonFly)
        mode_of() { stat -f '%Lp' "$1"; }
        mtime_of() { stat -f '%m' "$1"; }
        ;;
    *)
        mode_of() { stat -c '%a' "$1"; }
        mtime_of() { stat -c '%Y' "$1"; }
        ;;
esac
run_jash() {
    env HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
        JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
        "$JASH" "$@"
}

cat >"$T/race_hook.c" <<'C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int (*next_close)(int);
static int (*next_fsync)(int);
static int (*next_kill)(pid_t, int);
static int (*next_link)(const char *, const char *);
static int (*next_linkat)(int, const char *, int, const char *, int);
static int (*next_open)(const char *, int, ...);
static int (*next_openat)(int, const char *, int, ...);
static int (*next_rename)(const char *, const char *);
static int (*next_renameat)(int, const char *, int, const char *);
static int (*next_renameat2)(int, const char *, int, const char *, unsigned int);
static int (*next_lstat)(const char *, struct stat *);
static int (*next_stat)(const char *, struct stat *);
static int (*next_unlinkat)(int, const char *, int);
static ssize_t (*next_write)(int, const void *, size_t);
static int destination_swapped;
static int source_close_swapped;
static int source_open_swapped;
static int source_link_swapped;
static int forced_exdev;
static int forced_rename_failure;
static int copy_write_failed;
static int source_delete_failed;
static int source_delete_completed;
static int noreplace_cleanup_failed;
static int cp_race_created;
static int mv_race_created;
static int mv_race_seen;
static int mv_commit_fsync_seen;
static int mv_commit_fsync_failed;

static void load_symbols(void) {
    if (!next_close) next_close = dlsym(RTLD_NEXT, "close");
    if (!next_fsync) next_fsync = dlsym(RTLD_NEXT, "fsync");
    if (!next_kill) next_kill = dlsym(RTLD_NEXT, "kill");
    if (!next_link) next_link = dlsym(RTLD_NEXT, "link");
    if (!next_linkat) next_linkat = dlsym(RTLD_NEXT, "linkat");
    if (!next_open) next_open = dlsym(RTLD_NEXT, "open");
    if (!next_openat) next_openat = dlsym(RTLD_NEXT, "openat");
    if (!next_rename) next_rename = dlsym(RTLD_NEXT, "rename");
    if (!next_renameat) next_renameat = dlsym(RTLD_NEXT, "renameat");
    if (!next_renameat2) next_renameat2 = dlsym(RTLD_NEXT, "renameat2");
    if (!next_lstat) next_lstat = dlsym(RTLD_NEXT, "lstat");
    if (!next_stat) next_stat = dlsym(RTLD_NEXT, "stat");
    if (!next_unlinkat) next_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    if (!next_write) next_write = dlsym(RTLD_NEXT, "write");
}

static void mark_rename_fault(void) {
    const char *marker = getenv("JASH_RENAME_FAULT_MARKER");
    if (!marker) return;
    int fd = next_open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) next_close(fd);
}

static int path_at(int dirfd, const char *name, char *buf, size_t cap) {
    if (!name) return 0;
    if (name[0] == '/') return snprintf(buf, cap, "%s", name) < (int)cap;
    char parent[4096];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(parent, sizeof(parent))) return 0;
    } else {
        char proc[64];
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(proc, parent, sizeof(parent) - 1);
        if (n < 0) return 0;
        parent[n] = '\0';
    }
    return snprintf(buf, cap, "%s/%s", parent, name) < (int)cap;
}

static void create_mv_racer(const char *path) {
    const char *dst = getenv("JASH_MV_N_RACE_DST");
    const char *after_text = getenv("JASH_MV_RACE_AFTER");
    int after = after_text ? atoi(after_text) : 1;
    if (dst && strcmp(path, dst) == 0 && ++mv_race_seen >= after &&
        __sync_bool_compare_and_swap(&mv_race_created, 0, 1)) {
        int fd = next_open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            (void)next_write(fd, "racer", 5);
            next_close(fd);
        }
    }
}

int stat(const char *path, struct stat *st) {
    load_symbols();
    int rc = next_stat(path, st);
    if (rc != 0 && errno == ENOENT) create_mv_racer(path);
    return rc;
}

int lstat(const char *path, struct stat *st) {
    load_symbols();
    int rc = next_lstat(path, st);
    if (rc != 0 && errno == ENOENT) create_mv_racer(path);
    return rc;
}

int unlinkat(int dirfd, const char *path, int flags) {
    char proc[64], target[4096];
    load_symbols();
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", dirfd);
    ssize_t n = readlink(proc, target, sizeof(target) - 1);
    if (n >= 0) target[n] = '\0';
    if (getenv("JASH_FAIL_SOURCE_DELETE") && strcmp(path, "item") == 0 &&
        n >= 0 && strstr(target, "/.jash-mv-hold-") != NULL &&
        __sync_bool_compare_and_swap(&source_delete_failed, 0, 1)) {
        errno = EIO;
        return -1;
    }
    if (getenv("JASH_FAIL_CAPTURE_DELETE_SYNC") && strcmp(path, "item") == 0 &&
        n >= 0 && strstr(target, "/.jash-mv-hold-") != NULL) {
        int rc = next_unlinkat(dirfd, path, flags);
        if (rc == 0) source_delete_completed = 1;
        return rc;
    }
    if (getenv("JASH_FAIL_NOREPLACE_CLEANUP") && strcmp(path, "item") == 0 &&
        n >= 0 && strstr(target, "/.jash-cp-copy-") != NULL &&
        __sync_bool_compare_and_swap(&noreplace_cleanup_failed, 0, 1)) {
        const char *marker = getenv("JASH_NOREPLACE_CLEANUP_MARKER");
        if (marker) {
            int fd = next_open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) next_close(fd);
        }
        errno = EIO;
        return -1;
    }
    return next_unlinkat(dirfd, path, flags);
}

static int is_copy_output(int fd) {
    const char *dst = getenv("JASH_FAIL_COPY_DST");
    struct stat opened, named;
    char proc[64], target[4096];
    if (dst && fstat(fd, &opened) == 0 && lstat(dst, &named) == 0 &&
        opened.st_dev == named.st_dev && opened.st_ino == named.st_ino)
        return 1;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = '\0';
    return (strstr(target, "/.jash-mv-hold-") != NULL &&
            strstr(target, "/copy") != NULL) ||
           (strstr(target, "/.jash-cp-copy-") != NULL &&
            strstr(target, "/item") != NULL);
}

ssize_t write(int fd, const void *buf, size_t count) {
    load_symbols();
    if (getenv("JASH_FAIL_COPY_WRITE") && !copy_write_failed &&
        is_copy_output(fd) &&
        __sync_bool_compare_and_swap(&copy_write_failed, 0, 1)) {
        const char *marker = getenv("JASH_COPY_WRITE_MARKER");
        if (marker) {
            int marker_fd = next_open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (marker_fd >= 0) next_close(marker_fd);
        }
        if (count > 0) (void)next_write(fd, buf, count > 3 ? 3 : count);
        errno = EIO;
        return -1;
    }
    return next_write(fd, buf, count);
}

static void swap_source_at_link(void) {
    const char *src = getenv("JASH_SWAP_SOURCE_ON_LINK");
    const char *replacement = getenv("JASH_SWAP_LINK_REPLACEMENT");
    const char *saved = getenv("JASH_SWAP_LINK_SAVED");
    const char *dst = getenv("JASH_DEDUP_DST");
    const char *gap = getenv("JASH_DEDUP_GAP_MARKER");
    struct stat st;

    if (dst && gap && lstat(dst, &st) != 0 && errno == ENOENT) {
        int fd = next_open(gap, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) next_close(fd);
    }
    if (src && replacement && saved &&
        __sync_bool_compare_and_swap(&source_link_swapped, 0, 1)) {
        next_rename(src, saved);
        next_rename(replacement, src);
    }
}

int link(const char *old_path, const char *new_path) {
    load_symbols();
    swap_source_at_link();
    return next_link(old_path, new_path);
}

int linkat(int old_fd, const char *old_path, int new_fd, const char *new_path,
           int flags) {
    load_symbols();
    swap_source_at_link();
    return next_linkat(old_fd, old_path, new_fd, new_path, flags);
}

int open(const char *path, int flags, ...) {
    const char *src;
    const char *replacement;
    const char *saved;
    const char *race_dst;
    mode_t mode = 0;
    va_list ap;

    load_symbols();
    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    race_dst = getenv("JASH_CP_N_RACE_DST");
    if (race_dst && !cp_race_created &&
        (strcmp(path, race_dst) == 0 || strstr(path, "/.jash-cp-copy-") != NULL) &&
        __sync_bool_compare_and_swap(&cp_race_created, 0, 1)) {
        int race_fd = next_open(race_dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (race_fd >= 0) {
            (void)next_write(race_fd, "racer", 5);
            next_close(race_fd);
        }
    }
    src = getenv("JASH_SWAP_ON_OPEN_SRC");
    replacement = getenv("JASH_SWAP_ON_OPEN_REPLACEMENT");
    saved = getenv("JASH_SWAP_ON_OPEN_SAVED");
    if (!source_open_swapped && src && replacement && saved &&
        strcmp(path, src) == 0 &&
        __sync_bool_compare_and_swap(&source_open_swapped, 0, 1)) {
        next_rename(src, saved);
        next_rename(replacement, src);
    }
    if (flags & O_CREAT) return next_open(path, flags, mode);
    return next_open(path, flags);
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    va_list ap;
    load_symbols();
    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    const char *race_dst = getenv("JASH_CP_N_RACE_DST");
    if (race_dst && !cp_race_created && strstr(path, ".jash-cp-copy-") != NULL &&
        __sync_bool_compare_and_swap(&cp_race_created, 0, 1)) {
        int race_fd = next_open(race_dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (race_fd >= 0) {
            (void)next_write(race_fd, "racer", 5);
            next_close(race_fd);
        }
    }
    if (flags & O_CREAT) return next_openat(dirfd, path, flags, mode);
    return next_openat(dirfd, path, flags);
}

int close(int fd) {
    struct stat opened;
    struct stat named;
    const char *dst;
    const char *replacement;
    const char *saved;
    const char *source;
    const char *source_replacement;
    const char *source_saved;
    int is_destination = 0;
    int fail_copy_close = 0;
    int rc;

    load_symbols();
    fail_copy_close = getenv("JASH_FAIL_COPY_CLOSE") && is_copy_output(fd);
    dst = getenv("JASH_RACE_DST");
    replacement = getenv("JASH_RACE_REPLACEMENT");
    saved = getenv("JASH_RACE_SAVED");
    if (dst && ((fstat(fd, &opened) == 0 && lstat(dst, &named) == 0 &&
                 opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) ||
                is_copy_output(fd)))
        is_destination = 1;

    rc = next_close(fd);
    if (is_destination && replacement && saved &&
        __sync_bool_compare_and_swap(&destination_swapped, 0, 1)) {
        next_rename(dst, saved);
        next_rename(replacement, dst);
    }
    source = getenv("JASH_SWAP_SOURCE_ON_CLOSE");
    source_replacement = getenv("JASH_SWAP_SOURCE_REPLACEMENT");
    source_saved = getenv("JASH_SWAP_SOURCE_SAVED");
    if (is_destination && source && source_replacement && source_saved &&
        __sync_bool_compare_and_swap(&source_close_swapped, 0, 1)) {
        next_rename(source, source_saved);
        next_rename(source_replacement, source);
    }
    if (fail_copy_close) { errno = EIO; return -1; }
    return rc;
}

int rename(const char *old_path, const char *new_path) {
    const char *src;
    const char *dst;

    load_symbols();
    src = getenv("JASH_FAIL_RENAME_SRC");
    dst = getenv("JASH_FAIL_RENAME_DST");
    if (!forced_rename_failure && src && dst && strcmp(old_path, src) == 0 &&
        strcmp(new_path, dst) == 0 &&
        __sync_bool_compare_and_swap(&forced_rename_failure, 0, 1)) {
        mark_rename_fault();
        errno = EIO;
        return -1;
    }
    src = getenv("JASH_FORCE_EXDEV_SRC");
    dst = getenv("JASH_FORCE_EXDEV_DST");
    if (!forced_exdev && src && dst && strcmp(old_path, src) == 0 &&
        strcmp(new_path, dst) == 0) {
        forced_exdev = 1;
        mark_rename_fault();
        errno = EXDEV;
        return -1;
    }
    return next_rename(old_path, new_path);
}

int renameat(int old_fd, const char *old_path, int new_fd,
             const char *new_path) {
    char old_full[4096], new_full[4096];
    const char *src;
    const char *dst;
    load_symbols();
    int resolved = path_at(old_fd, old_path, old_full, sizeof(old_full)) &&
                   path_at(new_fd, new_path, new_full, sizeof(new_full));
    src = getenv("JASH_FAIL_RENAME_SRC");
    dst = getenv("JASH_FAIL_RENAME_DST");
    if (resolved && !forced_rename_failure && src && dst &&
        strcmp(old_full, src) == 0 && strcmp(new_full, dst) == 0 &&
        __sync_bool_compare_and_swap(&forced_rename_failure, 0, 1)) {
        const char *race_src = getenv("JASH_ROLLBACK_RACE_SRC");
        if (race_src) {
            int race_fd = next_open(race_src,
                                    O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (race_fd >= 0) {
                (void)next_write(race_fd, "raced", 5);
                next_close(race_fd);
            }
        }
        mark_rename_fault();
        errno = EIO;
        return -1;
    }
    src = getenv("JASH_FORCE_EXDEV_SRC");
    dst = getenv("JASH_FORCE_EXDEV_DST");
    if (resolved && !forced_exdev && src && dst &&
        strcmp(old_full, src) == 0 && strcmp(new_full, dst) == 0) {
        forced_exdev = 1;
        mark_rename_fault();
        errno = EXDEV;
        return -1;
    }
    return next_renameat(old_fd, old_path, new_fd, new_path);
}

int renameat2(int old_fd, const char *old_path, int new_fd,
              const char *new_path, unsigned int flags) {
    const char *src;
    const char *dst;
    char old_full[4096], new_full[4096];

    load_symbols();
    int resolved = path_at(old_fd, old_path, old_full, sizeof(old_full)) &&
                   path_at(new_fd, new_path, new_full, sizeof(new_full));
    const char *race_dst = getenv("JASH_ATOMIC_DEST_RACE");
    if (resolved && race_dst && strcmp(new_full, race_dst) == 0) {
        int race_fd = -1;
        if (getenv("JASH_ATOMIC_DEST_RACE_DIR")) {
            if (mkdir(race_dst, 0700) == 0) {
                char occupant[4096];
                snprintf(occupant, sizeof(occupant), "%s/occupant", race_dst);
                race_fd = next_open(occupant, O_WRONLY | O_CREAT | O_EXCL, 0600);
            }
        } else {
            race_fd = next_open(race_dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        }
        if (race_fd >= 0) {
            (void)next_write(race_fd, "racer", 5);
            next_close(race_fd);
        }
    }
    src = getenv("JASH_FAIL_RENAME_SRC");
    dst = getenv("JASH_FAIL_RENAME_DST");
    if (resolved && !forced_rename_failure && src && dst &&
        strcmp(old_full, src) == 0 && strcmp(new_full, dst) == 0 &&
        __sync_bool_compare_and_swap(&forced_rename_failure, 0, 1)) {
        const char *race_src = getenv("JASH_ROLLBACK_RACE_SRC");
        if (race_src) {
            int race_fd = next_open(race_src, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (race_fd >= 0) {
                (void)next_write(race_fd, "raced", 5);
                next_close(race_fd);
            }
        }
        mark_rename_fault();
        errno = EIO;
        return -1;
    }
    src = getenv("JASH_FORCE_EXDEV_SRC");
    dst = getenv("JASH_FORCE_EXDEV_DST");
    if (resolved && !forced_exdev && src && dst &&
        strcmp(old_full, src) == 0 && strcmp(new_full, dst) == 0) {
        forced_exdev = 1;
        mark_rename_fault();
        errno = EXDEV;
        return -1;
    }
    return next_renameat2(old_fd, old_path, new_fd, new_path, flags);
}

int fsync(int fd) {
    char proc[64], path[4096];
    load_symbols();
    const char *dir = getenv("JASH_MV_COMMIT_FSYNC_DIR");
    const char *nth_text = getenv("JASH_MV_COMMIT_FSYNC_N");
    int nth = nth_text ? atoi(nth_text) : 2;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, path, sizeof(path) - 1);
    if (n >= 0) path[n] = '\0';
    const char *publish_dir = getenv("JASH_MV_EXDEV_DEST_FSYNC");
    if (publish_dir && n >= 0 && strcmp(path, publish_dir) == 0) {
        errno = EIO;
        return -1;
    }
    if (source_delete_completed && getenv("JASH_FAIL_CAPTURE_DELETE_SYNC") &&
        n >= 0 && strstr(path, "/.jash-mv-hold-") != NULL) {
        errno = EIO;
        return -1;
    }
    if (dir && n >= 0 && strcmp(path, dir) == 0 &&
        ++mv_commit_fsync_seen == nth &&
        __sync_bool_compare_and_swap(&mv_commit_fsync_failed, 0, 1)) {
        errno = EIO;
        return -1;
    }
    return next_fsync(fd);
}

int kill(pid_t pid, int sig) {
    const char *log;
    char record[64];
    int fd;
    int n;

    load_symbols();
    log = getenv("JASH_KILL_LOG");
    if (!log) return next_kill(pid, sig);
    fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        n = snprintf(record, sizeof(record), "%ld %d\n", (long)pid, sig);
        if (n > 0) (void)write(fd, record, (size_t)n);
        next_close(fd);
    }
    return 0;
}
C

cc -shared -fPIC "$T/race_hook.c" -ldl -o "$T/race_hook.so" || exit 2
HOOK=$T/race_hook.so

# Swap the source immediately before open(2). Content and all preserved
# metadata must come from that opened descriptor, not an earlier stat result.
cp_open_src=$T/cp-open-src
cp_open_replacement=$T/cp-open-replacement
cp_open_saved=$T/cp-open-saved
cp_open_dst=$T/cp-open-dst
printf stale >"$cp_open_src"
printf opened >"$cp_open_replacement"
touch -t 200404040404 "$cp_open_src"
touch -t 200505050505 "$cp_open_replacement"
chmod 0600 "$cp_open_src"
chmod 0751 "$cp_open_replacement"
cp_open_mtime=$(mtime_of "$cp_open_replacement")
env LD_PRELOAD="$HOOK" JASH_SWAP_ON_OPEN_SRC="$cp_open_src" \
    JASH_SWAP_ON_OPEN_REPLACEMENT="$cp_open_replacement" \
    JASH_SWAP_ON_OPEN_SAVED="$cp_open_saved" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"cp\",\"-p\",\"$cp_open_src\",\"$cp_open_dst\"]}" \
    >/dev/null 2>&1 || true
if [[ -f "$cp_open_saved" && $(<"$cp_open_dst") == opened &&
      $(mode_of "$cp_open_dst") == 751 && $(mtime_of "$cp_open_dst") == "$cp_open_mtime" ]]; then
    ok "cp derives source metadata from the opened descriptor"
else
    no "cp used metadata captured before source open"
fi

# --dedup must hard-link the source descriptor, not resolve src again after the
# content check. The destination must remain present until atomic replacement.
cp_dedup_src=$T/cp-dedup-src
cp_dedup_replacement=$T/cp-dedup-replacement
cp_dedup_saved=$T/cp-dedup-saved
cp_dedup_dst=$T/cp-dedup-dst
cp_dedup_gap=$T/cp-dedup-gap
printf verified >"$cp_dedup_src"
printf attacker >"$cp_dedup_replacement"
printf verified >"$cp_dedup_dst"
env LD_PRELOAD="$HOOK" JASH_SWAP_SOURCE_ON_LINK="$cp_dedup_src" \
    JASH_SWAP_LINK_REPLACEMENT="$cp_dedup_replacement" \
    JASH_SWAP_LINK_SAVED="$cp_dedup_saved" JASH_DEDUP_DST="$cp_dedup_dst" \
    JASH_DEDUP_GAP_MARKER="$cp_dedup_gap" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"cp\",\"--dedup\",\"$cp_dedup_src\",\"$cp_dedup_dst\"]}" \
    >/dev/null 2>&1 || true
dedup_src_inode=$(stat -c '%d:%i' "$cp_dedup_saved" 2>/dev/null || true)
dedup_dst_inode=$(stat -c '%d:%i' "$cp_dedup_dst" 2>/dev/null || true)
if [[ -n "$dedup_src_inode" && "$dedup_dst_inode" == "$dedup_src_inode" &&
      $(<"$cp_dedup_dst") == verified && ! -e "$cp_dedup_gap" ]]; then
    ok "cp --dedup atomically links the verified source descriptor"
else
    no "cp --dedup reopened source or exposed an absent destination"
fi

# A destination created after option parsing but before output creation must win
# atomically. cp -n may neither truncate nor replace it.
cp_race_src=$T/cp-race-src
cp_race_dst=$T/cp-race-dst
printf source >"$cp_race_src"
env LD_PRELOAD="$HOOK" JASH_CP_N_RACE_DST="$cp_race_dst" HOME="$T/home" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"cp\",\"-n\",\"$cp_race_src\",\"$cp_race_dst\"]}" \
    >/dev/null 2>&1
cp_race_rc=$?
if [[ $cp_race_rc -eq 0 && -f "$cp_race_dst" && $(<"$cp_race_dst") == racer ]]; then
    ok "cp -n atomically preserves a concurrently created destination"
else
    no "cp -n concurrent destination creation"
fi

cp_fresh_src=$T/cp-fresh-src
cp_fresh_dst=$T/cp-fresh-dst
printf fresh >"$cp_fresh_src"
run_jash --exec-json \
    "{\"argv\":[\"cp\",\"-n\",\"$cp_fresh_src\",\"$cp_fresh_dst\"]}" \
    >/dev/null 2>&1
cp_fresh_rc=$?
if [[ $cp_fresh_rc -eq 0 && -f "$cp_fresh_dst" && $(<"$cp_fresh_dst") == fresh ]]; then
    ok "cp -n publishes a completed copy when destination is absent"
else
    no "cp -n fresh destination publication"
fi

cp_fail_src=$T/cp-fail-src
cp_fail_dst=$T/cp-fail-dst
printf replacement-data >"$cp_fail_src"
printf original-data >"$cp_fail_dst"
cp_fail_inode=$(stat -c '%d:%i' "$cp_fail_dst")
cp_fail_marker=$T/cp-fail-write-reached
env LD_PRELOAD="$HOOK" JASH_FAIL_COPY_DST="$cp_fail_dst" \
    JASH_FAIL_COPY_WRITE=1 JASH_COPY_WRITE_MARKER="$cp_fail_marker" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"cp\",\"-n\",\"$cp_fail_src\",\"$cp_fail_dst\"]}" \
    >/dev/null 2>&1
cp_fail_rc=$?
if [[ $cp_fail_rc -ne 0 && $(<"$cp_fail_dst") == original-data &&
      $(stat -c '%d:%i' "$cp_fail_dst") == "$cp_fail_inode" &&
      -f "$cp_fail_marker" &&
      ! $(compgen -G "$T/.jash-cp-copy-*") ]]; then
    ok "cp -n executes write-failure recovery and preserves destination"
else
    no "cp -n write-failure branch, preservation, or private-temp cleanup"
fi

# Exercise the portable no-replace link fallback directly, then fail removal of
# its private name after destination publication. The caller's ordinary cleanup
# must consume the recoverable private name after publication is rolled back.
cp_link_cleanup_dst=$T/cp-link-cleanup-dst
cp_link_cleanup_marker=$T/cp-link-cleanup-reached
cat >"$T/cp_link_cleanup.c" <<'C'
#include "builtins/builtin_cp.c"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    cp_temp_t temp;
    if (cp_temp_begin(argv[1], &temp) != 0) return 3;
    int fd = openat(temp.hold_fd, "item", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write(fd, "source", 6) != 6 || close(fd) != 0) return 4;
    int rc = cp_publish_noreplace_link(&temp);
    cp_temp_close(&temp, rc != 0);
    return rc == 0 ? 5 : 0;
}
C
cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$ROOT/c" \
    "$T/cp_link_cleanup.c" -o "$T/cp_link_cleanup" >/dev/null 2>&1 || exit 2
env LD_PRELOAD="$HOOK" \
    JASH_FAIL_NOREPLACE_CLEANUP=1 \
    JASH_NOREPLACE_CLEANUP_MARKER="$cp_link_cleanup_marker" \
    "$T/cp_link_cleanup" "$cp_link_cleanup_dst" >/dev/null 2>&1
cp_link_cleanup_rc=$?
if [[ $cp_link_cleanup_rc -eq 0 && -f "$cp_link_cleanup_marker" &&
      ! -e "$cp_link_cleanup_dst" &&
      ! $(compgen -G "$T/.jash-cp-copy-*") ]]; then
    ok "cp -n link fallback rolls back publication on cleanup failure"
else
    no "cp -n link cleanup: rc=$cp_link_cleanup_rc marker=$([[ -f "$cp_link_cleanup_marker" ]] && printf yes || printf no) dst=$([[ -e "$cp_link_cleanup_dst" ]] && printf yes || printf no) holds=$(compgen -G "$T/.jash-cp-copy-*" || printf none)"
fi

# access(2) follows dangling symlinks and therefore cannot implement -n.
cp_dangling_src=$T/cp-dangling-src
cp_dangling_dst=$T/cp-dangling-dst
cp_dangling_target=$T/cp-dangling-target
printf source >"$cp_dangling_src"
ln -s "$cp_dangling_target" "$cp_dangling_dst"
run_jash --exec-json \
    "{\"argv\":[\"cp\",\"-n\",\"$cp_dangling_src\",\"$cp_dangling_dst\"]}" \
    >/dev/null 2>&1
cp_dangling_rc=$?
if [[ $cp_dangling_rc -eq 0 && -L "$cp_dangling_dst" && ! -e "$cp_dangling_target" ]]; then
    ok "cp -n preserves dangling symlinks without creating their targets"
else
    no "cp -n dangling symlink no-clobber"
fi

cp_directory_src=$T/cp-directory-src
cp_directory_dst=$T/cp-directory-dst
printf source >"$cp_directory_src"
mkdir "$cp_directory_dst"
mkdir "$cp_directory_dst/${cp_directory_src##*/}"
run_jash --exec-json \
    "{\"argv\":[\"cp\",\"-n\",\"$cp_directory_src\",\"$cp_directory_dst\"]}" \
    >/dev/null 2>&1
cp_directory_rc=$?
if [[ $cp_directory_rc -eq 0 && -d "$cp_directory_dst/${cp_directory_src##*/}" ]]; then
    ok "cp -n preserves existing destination directories"
else
    no "cp -n destination directory no-clobber"
fi

# Dedup publication must use the same no-replace boundary, not an early access
# check. Swapping src at link time proves the descriptor-link path was reached.
cp_dedup_n_src=$T/cp-dedup-n-src
cp_dedup_n_replacement=$T/cp-dedup-n-replacement
cp_dedup_n_saved=$T/cp-dedup-n-saved
cp_dedup_n_dst=$T/cp-dedup-n-dst
printf verified >"$cp_dedup_n_src"
printf attacker >"$cp_dedup_n_replacement"
printf verified >"$cp_dedup_n_dst"
cp_dedup_n_inode=$(stat -c '%d:%i' "$cp_dedup_n_dst")
env LD_PRELOAD="$HOOK" JASH_SWAP_SOURCE_ON_LINK="$cp_dedup_n_src" \
    JASH_SWAP_LINK_REPLACEMENT="$cp_dedup_n_replacement" \
    JASH_SWAP_LINK_SAVED="$cp_dedup_n_saved" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"cp\",\"--dedup\",\"-n\",\"$cp_dedup_n_src\",\"$cp_dedup_n_dst\"]}" \
    >/dev/null 2>&1
cp_dedup_n_rc=$?
if [[ $cp_dedup_n_rc -eq 0 && -f "$cp_dedup_n_saved" &&
      $(stat -c '%d:%i' "$cp_dedup_n_dst") == "$cp_dedup_n_inode" &&
      $(<"$cp_dedup_n_dst") == verified ]]; then
    ok "cp --dedup -n publishes with atomic no-replace semantics"
else
    no "cp --dedup -n relied on a pathname precheck or replaced destination"
fi

# The close hook renames the copied inode aside and puts an unrelated file at
# the destination pathname. Metadata must have been applied to the open inode.
cp_src=$T/cp-src
cp_dst=$T/cp-dst
cp_replacement=$T/cp-replacement
cp_saved=$T/cp-saved
printf source >"$cp_src"
printf victim >"$cp_replacement"
chmod 6751 "$cp_src"
chmod 0600 "$cp_replacement"
touch -t 200101010101 "$cp_src"
env LD_PRELOAD="$HOOK" JASH_RACE_DST="$cp_dst" \
    JASH_RACE_REPLACEMENT="$cp_replacement" JASH_RACE_SAVED="$cp_saved" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json "{\"argv\":[\"cp\",\"-p\",\"$cp_src\",\"$cp_dst\"]}" \
    >/dev/null 2>&1 || true
if [[ -f "$cp_saved" && $(mode_of "$cp_saved") == 6751 &&
      $(mtime_of "$cp_saved") == $(mtime_of "$cp_src") &&
      $(mode_of "$cp_dst") == 600 ]]; then
    ok "cp applies preserved metadata through the owned descriptor"
else
    no "cp metadata followed a replaced destination pathname"
fi

# Without -p, cp consistently strips special bits while still touching only
# the copied inode.
rm -f "$cp_dst" "$cp_saved"
printf victim >"$cp_replacement"
chmod 0600 "$cp_replacement"
env LD_PRELOAD="$HOOK" JASH_RACE_DST="$cp_dst" \
    JASH_RACE_REPLACEMENT="$cp_replacement" JASH_RACE_SAVED="$cp_saved" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json "{\"argv\":[\"cp\",\"$cp_src\",\"$cp_dst\"]}" \
    >/dev/null 2>&1 || true
if [[ -f "$cp_saved" && $(mode_of "$cp_saved") == 751 && $(mode_of "$cp_dst") == 600 ]]; then
    ok "cp strips special bits on its descriptor without -p"
else
    no "cp non-preserve mode handling raced with destination replacement"
fi

# Force rename(2) to report EXDEV, then replace the copied destination when its
# descriptor closes. A move may delete src only while dst still names its copy.
mv_src=$T/mv-src
mv_dst=$T/mv-dst
mv_replacement=$T/mv-replacement
mv_saved=$T/mv-saved
mv_fault_marker=$T/mv-exdev.marker
printf source >"$mv_src"
printf victim >"$mv_replacement"
chmod 6751 "$mv_src"
chmod 0600 "$mv_replacement"
touch -t 200202020202 "$mv_src"
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_src" JASH_FORCE_EXDEV_DST="$mv_dst" \
    JASH_RENAME_FAULT_MARKER="$mv_fault_marker" \
    JASH_RACE_DST="$mv_dst" JASH_RACE_REPLACEMENT="$mv_replacement" \
    JASH_RACE_SAVED="$mv_saved" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_src\",\"$mv_dst\"]}" >/dev/null 2>&1 || true
if [[ -f "$mv_fault_marker" && -f "$mv_src" && ! -e "$mv_saved" && $(<"$mv_dst") == victim &&
      $(mode_of "$mv_dst") == 600 ]]; then
    ok "EXDEV mv retains source when destination identity is replaced"
else
    no "EXDEV mv deleted source or applied metadata to a replacement"
fi

# The identity guard must still permit the ordinary EXDEV copy-and-delete path.
mv_ok_src=$T/mv-ok-src
mv_ok_dst=$T/mv-ok-dst
printf ordinary >"$mv_ok_src"
touch -t 200303030303 "$mv_ok_src"
chmod 6751 "$mv_ok_src"
mv_ok_mtime=$(mtime_of "$mv_ok_src")
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_ok_src" \
    JASH_FORCE_EXDEV_DST="$mv_ok_dst" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_ok_src\",\"$mv_ok_dst\"]}" >/dev/null 2>&1
mv_ok_rc=$?
if [[ $mv_ok_rc -eq 0 && ! -e "$mv_ok_src" && -f "$mv_ok_dst" &&
      $(mode_of "$mv_ok_dst") == 6751 && $(mtime_of "$mv_ok_dst") == "$mv_ok_mtime" ]]; then
    ok "EXDEV mv preserves metadata and removes a verified source"
else
    no "EXDEV mv ordinary copy-and-delete path"
fi

# Destination publication must cross its directory durability barrier before
# source deletion. A failed barrier rolls publication back and keeps source.
mv_publish_dir=$T/mv-publish-dir
mv_publish_src_dir=$T/mv-publish-source-dir
mkdir "$mv_publish_dir" "$mv_publish_src_dir"
mv_publish_src=$mv_publish_src_dir/source
mv_publish_dst=$mv_publish_dir/destination
printf source >"$mv_publish_src"
printf prior >"$mv_publish_dst"
mv_publish_out="$(env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_publish_src" \
    JASH_FORCE_EXDEV_DST="$mv_publish_dst" \
    JASH_MV_EXDEV_DEST_FSYNC="$mv_publish_dir" HOME="$T/home" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_publish_src\",\"$mv_publish_dst\"]}" 2>&1)"
mv_publish_rc=$?
if [[ $mv_publish_rc -ne 0 && -f "$mv_publish_src" &&
      $(<"$mv_publish_src") == source && $(<"$mv_publish_dst") == prior ]]; then
    ok "EXDEV mv syncs destination publication before source deletion"
else
    no "EXDEV mv deleted source before destination publication was durable"
fi

# unlink may succeed while the source-directory durability sync fails. That
# positive capture-delete result is still a command failure; the durable
# destination remains the recovery copy.
mv_delete_sync_src_dir=$T/mv-delete-sync-source
mv_delete_sync_dst_dir=$T/mv-delete-sync-destination
mkdir "$mv_delete_sync_src_dir" "$mv_delete_sync_dst_dir"
mv_delete_sync_src=$mv_delete_sync_src_dir/source
mv_delete_sync_dst=$mv_delete_sync_dst_dir/destination
printf durable-copy >"$mv_delete_sync_src"
mv_delete_sync_out="$(env LD_PRELOAD="$HOOK" \
    JASH_FORCE_EXDEV_SRC="$mv_delete_sync_src" \
    JASH_FORCE_EXDEV_DST="$mv_delete_sync_dst" \
    JASH_FAIL_CAPTURE_DELETE_SYNC=1 HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_delete_sync_src\",\"$mv_delete_sync_dst\"]}" 2>&1)"
mv_delete_sync_rc=$?
if [[ $mv_delete_sync_rc -ne 0 && ! -e "$mv_delete_sync_src" &&
      -f "$mv_delete_sync_dst" && $(<"$mv_delete_sync_dst") == durable-copy ]]; then
    ok "EXDEV mv propagates capture-delete durability failure"
else
    no "EXDEV mv treated capture-delete durability failure as success"
fi

# A regular file cannot replace a directory at its final destination. Forced
# EXDEV must not turn the directory into a private leaked hold.
mv_over_dir_src=$T/mv-over-dir-src
mv_over_dir_parent=$T/mv-over-dir-parent
mv_over_dir_dst=$mv_over_dir_parent/${mv_over_dir_src##*/}
printf source >"$mv_over_dir_src"
mkdir "$mv_over_dir_parent" "$mv_over_dir_dst"
printf occupant >"$mv_over_dir_dst/occupant"
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_over_dir_src" \
    JASH_FORCE_EXDEV_DST="$mv_over_dir_dst" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_over_dir_src\",\"$mv_over_dir_parent\"]}" \
    >/dev/null 2>&1
mv_over_dir_rc=$?
if [[ $mv_over_dir_rc -ne 0 && -f "$mv_over_dir_src" &&
      -d "$mv_over_dir_dst" && $(<"$mv_over_dir_dst/occupant") == occupant &&
      ! $(compgen -G "$mv_over_dir_parent/.jash-mv-hold-*") ]]; then
    ok "EXDEV mv refuses regular-file replacement of an existing directory"
else
    no "EXDEV mv hid, replaced, or leaked an existing destination directory"
fi

# Multi-source --atomic cannot claim rollback for EXDEV copy+delete. Once EXDEV
# is reported, it must leave this operand and the pre-existing destination intact.
mv_atomic_exdev_src=$T/mv-atomic-exdev-src
mv_atomic_exdev_late=$T/mv-atomic-exdev-late
mv_atomic_exdev_dst=$T/mv-atomic-exdev-dst
mv_atomic_exdev_marker=$T/mv-atomic-exdev.marker
mkdir "$mv_atomic_exdev_dst"
printf replacement >"$mv_atomic_exdev_src"
printf late >"$mv_atomic_exdev_late"
printf original >"$mv_atomic_exdev_dst/${mv_atomic_exdev_src##*/}"
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_atomic_exdev_src" \
    JASH_FORCE_EXDEV_DST="$mv_atomic_exdev_dst/${mv_atomic_exdev_src##*/}" \
    JASH_RENAME_FAULT_MARKER="$mv_atomic_exdev_marker" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_atomic_exdev_src\",\"$mv_atomic_exdev_late\",\"$mv_atomic_exdev_dst\"]}" \
    >/dev/null 2>&1
mv_atomic_exdev_rc=$?
if [[ $mv_atomic_exdev_rc -ne 0 && -f "$mv_atomic_exdev_marker" &&
      -f "$mv_atomic_exdev_src" &&
      $(<"$mv_atomic_exdev_src") == replacement &&
      -f "$mv_atomic_exdev_late" && $(<"$mv_atomic_exdev_late") == late &&
      $(<"$mv_atomic_exdev_dst/${mv_atomic_exdev_src##*/}") == original ]]; then
    ok "multi-source atomic mv fails EXDEV before copy-and-delete mutation"
else
    no "multi-source atomic mv left a successful EXDEV operand after later failure"
fi

# A later rename failure must restore both the first source and the destination
# inode/content that the first same-device move overwrote.
mv_atomic_src=$T/mv-atomic-src
mv_atomic_late_src=$T/mv-atomic-late-src
mv_atomic_dst=$T/mv-atomic-dst
mv_atomic_marker=$T/mv-atomic-rename.marker
mkdir "$mv_atomic_dst"
printf replacement >"$mv_atomic_src"
printf late >"$mv_atomic_late_src"
printf original >"$mv_atomic_dst/${mv_atomic_src##*/}"
mv_atomic_dst_inode=$(stat -c '%d:%i' "$mv_atomic_dst/${mv_atomic_src##*/}")
env LD_PRELOAD="$HOOK" JASH_FAIL_RENAME_SRC="$mv_atomic_late_src" \
    JASH_FAIL_RENAME_DST="$mv_atomic_dst/${mv_atomic_late_src##*/}" \
    JASH_RENAME_FAULT_MARKER="$mv_atomic_marker" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_atomic_src\",\"$mv_atomic_late_src\",\"$mv_atomic_dst\"]}" \
    >/dev/null 2>&1
mv_atomic_rc=$?
if [[ $mv_atomic_rc -ne 0 && -f "$mv_atomic_marker" && -f "$mv_atomic_src" &&
      $(<"$mv_atomic_src") == replacement &&
      -f "$mv_atomic_late_src" && $(<"$mv_atomic_late_src") == late &&
      $(<"$mv_atomic_dst/${mv_atomic_src##*/}") == original &&
      $(stat -c '%d:%i' "$mv_atomic_dst/${mv_atomic_src##*/}") == "$mv_atomic_dst_inode" ]]; then
    ok "atomic mv restores overwritten destination on later operand failure"
else
    no "atomic mv failed to restore source and overwritten destination"
fi

# Rollback capture is inode-generic: symlinks and FIFOs must be restored with
# the same no-replace publication rule used for regular files.
for mv_special_kind in symlink fifo; do
    mv_special_dir=$T/mv-special-$mv_special_kind
    mv_special_dst=$mv_special_dir/dst
    mkdir -p "$mv_special_dst"
    mv_special_src=$mv_special_dir/source
    mv_special_late=$mv_special_dir/late
    mv_special_target=$mv_special_dst/source
    mv_special_marker=$mv_special_dir/rename.marker
    printf replacement >"$mv_special_src"
    printf late >"$mv_special_late"
    if [[ $mv_special_kind == symlink ]]; then
        ln -s special-link-target "$mv_special_target"
    else
        mkfifo "$mv_special_target"
    fi
    env LD_PRELOAD="$HOOK" JASH_FAIL_RENAME_SRC="$mv_special_late" \
        JASH_FAIL_RENAME_DST="$mv_special_dst/late" HOME="$T/home" \
        JASH_RENAME_FAULT_MARKER="$mv_special_marker" \
        PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
        JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
        "{\"argv\":[\"mv\",\"--atomic\",\"$mv_special_src\",\"$mv_special_late\",\"$mv_special_dst\"]}" \
        >/dev/null 2>&1
    mv_special_rc=$?
    if [[ $mv_special_kind == symlink ]]; then
        mv_special_restored=$([[ -L $mv_special_target &&
            $(readlink "$mv_special_target") == special-link-target ]] && printf 1 || printf 0)
    else
        mv_special_restored=$([[ -p $mv_special_target ]] && printf 1 || printf 0)
    fi
    if [[ $mv_special_rc -ne 0 && -f "$mv_special_marker" && -f "$mv_special_src" &&
          $(<"$mv_special_src") == replacement && $mv_special_restored -eq 1 ]]; then
        ok "atomic mv restores captured $mv_special_kind destination"
    else
        no "atomic mv failed no-replace restore for $mv_special_kind destination"
    fi
done

# A destination created after rollback capture must not be overwritten by the
# atomic publication. Both it and the captured original remain recoverable.
mv_capture_race_dir=$T/mv-capture-race
mkdir "$mv_capture_race_dir"
mv_capture_race_src=$mv_capture_race_dir/src
mv_capture_race_dst=$mv_capture_race_dir/dst
printf replacement >"$mv_capture_race_src"
printf original >"$mv_capture_race_dst"
mv_capture_race_out="$(env LD_PRELOAD="$HOOK" JASH_ATOMIC_DEST_RACE="$mv_capture_race_dst" \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_capture_race_src\",\"$mv_capture_race_dst\"]}" \
    2>&1)"
mv_capture_race_rc=$?
mv_capture_race_holds=("$mv_capture_race_dir"/.jash-mv-hold-*/item)
mv_capture_race_hold=${mv_capture_race_holds[0]}
if [[ $mv_capture_race_rc -ne 0 && $(<"$mv_capture_race_src") == replacement &&
      $(<"$mv_capture_race_dst") == racer && -f "$mv_capture_race_hold" &&
      ${#mv_capture_race_holds[@]} -eq 1 &&
      $(<"$mv_capture_race_hold") == original &&
      "$mv_capture_race_out" == *"$mv_capture_race_hold"* ]]; then
    ok "atomic mv never clobbers a destination created after capture"
else
    no "atomic mv clobbered a destination created after capture"
fi
rm -rf "$mv_capture_race_dir"

mv_dir_race_src=$T/mv-dir-race-src
mv_dir_race_dst=$T/mv-dir-race-dst
mkdir "$mv_dir_race_src"
printf source >"$mv_dir_race_src/source"
env LD_PRELOAD="$HOOK" JASH_ATOMIC_DEST_RACE="$mv_dir_race_dst" \
    JASH_ATOMIC_DEST_RACE_DIR=1 HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_dir_race_src\",\"$mv_dir_race_dst\"]}" \
    >/dev/null 2>&1
mv_dir_race_rc=$?
if [[ $mv_dir_race_rc -ne 0 && -f "$mv_dir_race_src/source" &&
      "$(<"$mv_dir_race_dst/occupant")" == racer ]]; then
    ok "atomic directory mv never clobbers a raced destination"
else
    no "atomic directory mv clobbered a raced destination"
fi

# If a new inode appears at an earlier source before rollback, atomic mode must
# retain it, retain the moved inode at dst, and keep the prior destination in
# its recovery hold instead of overwriting either endpoint.
mv_race_src=$T/mv-rollback-race-src
mv_race_late=$T/mv-rollback-race-late
mv_race_dst=$T/mv-rollback-race-dst
mkdir "$mv_race_dst"
printf replacement >"$mv_race_src"
printf late >"$mv_race_late"
printf original >"$mv_race_dst/${mv_race_src##*/}"
env LD_PRELOAD="$HOOK" JASH_FAIL_RENAME_SRC="$mv_race_late" \
    JASH_FAIL_RENAME_DST="$mv_race_dst/${mv_race_late##*/}" \
    JASH_ROLLBACK_RACE_SRC="$mv_race_src" HOME="$T/home" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_race_src\",\"$mv_race_late\",\"$mv_race_dst\"]}" \
    >/dev/null 2>&1
mv_race_rc=$?
mv_race_holds=("$mv_race_dst"/.jash-mv-hold-*/item)
mv_race_hold=${mv_race_holds[0]}
if [[ $mv_race_rc -ne 0 && $(<"$mv_race_src") == raced &&
      $(<"$mv_race_dst/${mv_race_src##*/}") == replacement &&
      ${#mv_race_holds[@]} -eq 1 && -f "$mv_race_hold" &&
      $(<"$mv_race_hold") == original ]]; then
    ok "atomic mv rollback preserves raced source and both owned inodes"
else
    no "atomic mv rollback clobbered a raced source or lost recovery"
fi
rm -rf "$mv_race_dst"/.jash-mv-hold-* "$mv_race_src" \
       "$mv_race_dst/${mv_race_src##*/}"

# Commit cleanup failure keeps the old destination in a named recovery hold;
# the move reports failure but does not leak an open-only, undiscoverable inode.
mv_cleanup_dir=$T/mv-cleanup
mkdir "$mv_cleanup_dir"
mv_cleanup_src=$mv_cleanup_dir/src
mv_cleanup_dst=$mv_cleanup_dir/dst
printf replacement >"$mv_cleanup_src"
printf original >"$mv_cleanup_dst"
mv_cleanup_out="$(env LD_PRELOAD="$HOOK" JASH_FAIL_SOURCE_DELETE=1 \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_cleanup_src\",\"$mv_cleanup_dst\"]}" \
    2>&1)"
mv_cleanup_rc=$?
mv_cleanup_holds=("$mv_cleanup_dir"/.jash-mv-hold-*/item)
mv_cleanup_hold=${mv_cleanup_holds[0]}
if [[ $mv_cleanup_rc -ne 0 && ! -e "$mv_cleanup_src" &&
      $(<"$mv_cleanup_dst") == replacement && ${#mv_cleanup_holds[@]} -eq 1 &&
      -f "$mv_cleanup_hold" &&
      $(<"$mv_cleanup_hold") == original &&
      "$mv_cleanup_out" == *"$mv_cleanup_hold"* ]]; then
    ok "atomic mv commit cleanup exposes exact recoverable old destination"
else
    no "atomic mv commit cleanup leaked or hid old destination"
fi
rm -rf "$mv_cleanup_dir"

# A later operation's durability failure retains every prior destination in
# the batch; commit cannot discard holds operation-by-operation.
mv_batch_src1=$T/mv-batch-src1
mv_batch_src2=$T/mv-batch-src2
mv_batch_dst=$T/mv-batch-dst
mkdir "$mv_batch_dst"
printf replacement1 >"$mv_batch_src1"
printf replacement2 >"$mv_batch_src2"
printf original1 >"$mv_batch_dst/${mv_batch_src1##*/}"
printf original2 >"$mv_batch_dst/${mv_batch_src2##*/}"
mv_batch_out="$(env LD_PRELOAD="$HOOK" JASH_MV_COMMIT_FSYNC_DIR="$mv_batch_dst" \
    JASH_MV_COMMIT_FSYNC_N=4 HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_batch_src1\",\"$mv_batch_src2\",\"$mv_batch_dst\"]}" 2>&1)"
mv_batch_rc=$?
mv_batch_original1=0
mv_batch_original2=0
for hold in "$mv_batch_dst"/.jash-mv-hold-*/item; do
    [ -f "$hold" ] || continue
    [ "$(<"$hold")" = original1 ] && mv_batch_original1=1
    [ "$(<"$hold")" = original2 ] && mv_batch_original2=1
done
if [[ $mv_batch_rc -ne 0 && $mv_batch_original1 -eq 1 &&
      $mv_batch_original2 -eq 1 &&
      "$(<"$mv_batch_dst/${mv_batch_src1##*/}")" == replacement1 &&
      "$(<"$mv_batch_dst/${mv_batch_src2##*/}")" == replacement2 ]]; then
    ok "atomic multi-source commit retains every hold until all syncs pass"
else
    no "atomic multi-source commit deleted a hold before all syncs passed"
fi
rm -rf "$mv_batch_dst"/.jash-mv-hold-*

# A failed commit durability barrier must retain the prior destination rather
# than deleting the only rollback copy.
mv_sync_dir=$T/mv-sync
mkdir "$mv_sync_dir"
mv_sync_src=$mv_sync_dir/src
mv_sync_dst=$mv_sync_dir/dst
printf replacement >"$mv_sync_src"
printf original >"$mv_sync_dst"
mv_sync_out="$(env LD_PRELOAD="$HOOK" JASH_MV_COMMIT_FSYNC_DIR="$mv_sync_dir" \
    JASH_MV_COMMIT_FSYNC_N=2 HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_sync_src\",\"$mv_sync_dst\"]}" 2>&1)"
mv_sync_rc=$?
mv_sync_holds=("$mv_sync_dir"/.jash-mv-hold-*/item)
mv_sync_hold=${mv_sync_holds[0]}
if [[ $mv_sync_rc -ne 0 && ! -e "$mv_sync_src" &&
      $(<"$mv_sync_dst") == replacement && ${#mv_sync_holds[@]} -eq 1 &&
      -f "$mv_sync_hold" && $(<"$mv_sync_hold") == original &&
      "$mv_sync_out" == *"$mv_sync_hold"* ]]; then
    ok "atomic mv retains rollback copy after commit directory fsync failure"
else
    no "atomic mv deleted rollback copy after commit directory fsync failure"
fi
rm -rf "$mv_sync_dir"

# Source inspection failures are known before mutation in atomic mode.
mv_atomic_stat_src=$T/mv-atomic-stat-src
mv_atomic_stat_missing=$T/mv-atomic-stat-missing
mv_atomic_stat_dst=$T/mv-atomic-stat-dst
mkdir "$mv_atomic_stat_dst"
printf replacement >"$mv_atomic_stat_src"
printf original >"$mv_atomic_stat_dst/${mv_atomic_stat_src##*/}"
run_jash --exec-json \
    "{\"argv\":[\"mv\",\"--atomic\",\"$mv_atomic_stat_src\",\"$mv_atomic_stat_missing\",\"$mv_atomic_stat_dst\"]}" \
    >/dev/null 2>&1
mv_atomic_stat_rc=$?
if [[ $mv_atomic_stat_rc -ne 0 && -f "$mv_atomic_stat_src" &&
      $(<"$mv_atomic_stat_src") == replacement &&
      $(<"$mv_atomic_stat_dst/${mv_atomic_stat_src##*/}") == original ]]; then
    ok "atomic mv preflights later source stat failures before mutation"
else
    no "atomic mv mutated an earlier operand before a source stat failure"
fi

# A destination appearing immediately after the policy snapshot must make every
# keep-existing spelling skip on both rename and EXDEV publication paths.
for policy in -n --resolve=keep-old --resolve=skip; do
    suffix=${policy//[^a-zA-Z0-9]/_}
    mv_n_src=$T/mv-n-src-$suffix
    mv_n_dst=$T/mv-n-dst-$suffix
    printf source >"$mv_n_src"
    env LD_PRELOAD="$HOOK" JASH_MV_N_RACE_DST="$mv_n_dst" JASH_MV_RACE_AFTER=2 \
        HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
        JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
        "$JASH" --exec-json \
        "{\"argv\":[\"mv\",\"$policy\",\"$mv_n_src\",\"$mv_n_dst\"]}" \
        >/dev/null 2>&1
    mv_n_rc=$?
    if [[ $mv_n_rc -eq 0 && -f "$mv_n_src" && $(<"$mv_n_dst") == racer ]]; then
        ok "same-filesystem mv $policy preserves a concurrent destination"
    else
        no "same-filesystem mv $policy concurrent destination policy"
    fi

    mv_n_exdev_src=$T/mv-n-exdev-src-$suffix
    mv_n_exdev_dst=$T/mv-n-exdev-dst-$suffix
    mv_n_exdev_replacement=$T/mv-n-exdev-replacement-$suffix
    mv_n_exdev_saved=$T/mv-n-exdev-saved-$suffix
    printf source >"$mv_n_exdev_src"
    printf racer >"$mv_n_exdev_replacement"
    env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_n_exdev_src" \
        JASH_FORCE_EXDEV_DST="$mv_n_exdev_dst" HOME="$T/home" \
        JASH_RACE_DST="$mv_n_exdev_dst" \
        JASH_RACE_REPLACEMENT="$mv_n_exdev_replacement" \
        JASH_RACE_SAVED="$mv_n_exdev_saved" \
        PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
        JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
        "{\"argv\":[\"mv\",\"$policy\",\"$mv_n_exdev_src\",\"$mv_n_exdev_dst\"]}" \
        >/dev/null 2>&1
    mv_n_exdev_rc=$?
    if [[ $mv_n_exdev_rc -eq 0 && -f "$mv_n_exdev_src" &&
          $(<"$mv_n_exdev_dst") == racer ]]; then
        ok "EXDEV mv $policy preserves a concurrent destination"
    else
        no "EXDEV mv $policy concurrent destination policy"
    fi
done

# Default policy must retain overwrite behavior for the same race.
mv_force_src=$T/mv-force-src
mv_force_dst=$T/mv-force-dst
printf source >"$mv_force_src"
env LD_PRELOAD="$HOOK" JASH_MV_N_RACE_DST="$mv_force_dst" JASH_MV_RACE_AFTER=2 \
    HOME="$T/home" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_force_src\",\"$mv_force_dst\"]}" \
    >/dev/null 2>&1
mv_force_rc=$?
if [[ $mv_force_rc -eq 0 && ! -e "$mv_force_src" && $(<"$mv_force_dst") == source ]]; then
    ok "default mv overwrites a concurrent destination"
else
    no "default mv concurrent overwrite semantics"
fi

# Directory fast-path rename must honor every keep-existing spelling.
for policy in -n --resolve=keep-old --resolve=skip; do
    suffix=${policy//[^a-zA-Z0-9]/_}
    mv_dir_src=$T/mv-dir-src-$suffix
    mv_dir_parent=$T/mv-dir-parent-$suffix
    mkdir "$mv_dir_src" "$mv_dir_parent"
    printf source >"$mv_dir_src/file"
    mkdir "$mv_dir_parent/${mv_dir_src##*/}"
    run_jash --exec-json \
        "{\"argv\":[\"mv\",\"$policy\",\"$mv_dir_src\",\"$mv_dir_parent\"]}" \
        >/dev/null 2>&1
    mv_dir_rc=$?
    if [[ $mv_dir_rc -eq 0 && -d "$mv_dir_src" &&
          -d "$mv_dir_parent/${mv_dir_src##*/}" ]]; then
        ok "directory mv $policy honors keep-old policy"
    else
        no "directory mv $policy conflict policy"
    fi
done

# Copy errors must occur only in an unpublished private inode. The existing
# destination and every hard-link alias must retain both identity and content.
mv_write_src=$T/mv-write-src
mv_write_dst=$T/mv-write-dst
mv_write_alias=$T/mv-write-alias
printf replacement-data >"$mv_write_src"
printf original-data >"$mv_write_dst"
ln "$mv_write_dst" "$mv_write_alias"
mv_write_inode=$(stat -c '%d:%i' "$mv_write_dst")
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_write_src" \
    JASH_FORCE_EXDEV_DST="$mv_write_dst" JASH_FAIL_COPY_DST="$mv_write_dst" \
    JASH_FAIL_COPY_WRITE=1 HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_write_src\",\"$mv_write_dst\"]}" >/dev/null 2>&1
mv_write_rc=$?
if [[ $mv_write_rc -ne 0 && -f "$mv_write_src" && -f "$mv_write_dst" &&
      -f "$mv_write_alias" &&
      $(<"$mv_write_dst") == original-data && $(<"$mv_write_alias") == original-data &&
      $(stat -c '%d:%i' "$mv_write_dst") == "$mv_write_inode" &&
      $(stat -c '%d:%i' "$mv_write_alias") == "$mv_write_inode" ]]; then
    ok "EXDEV mv partial write preserves destination and hard-link aliases"
else
    no "EXDEV mv partial write exposed or damaged the final destination"
fi

mv_close_src=$T/mv-close-src
mv_close_dst=$T/mv-close-dst
mv_close_alias=$T/mv-close-alias
printf replacement-data >"$mv_close_src"
printf original-data >"$mv_close_dst"
ln "$mv_close_dst" "$mv_close_alias"
mv_close_inode=$(stat -c '%d:%i' "$mv_close_dst")
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_close_src" \
    JASH_FORCE_EXDEV_DST="$mv_close_dst" JASH_FAIL_COPY_DST="$mv_close_dst" \
    JASH_FAIL_COPY_CLOSE=1 HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_close_src\",\"$mv_close_dst\"]}" >/dev/null 2>&1
mv_close_rc=$?
if [[ $mv_close_rc -ne 0 && -f "$mv_close_src" && -f "$mv_close_dst" &&
      -f "$mv_close_alias" &&
      $(<"$mv_close_dst") == original-data && $(<"$mv_close_alias") == original-data &&
      $(stat -c '%d:%i' "$mv_close_dst") == "$mv_close_inode" &&
      $(stat -c '%d:%i' "$mv_close_alias") == "$mv_close_inode" ]]; then
    ok "EXDEV mv close failure preserves destination and hard-link aliases"
else
    no "EXDEV mv close failure exposed or damaged the final destination"
fi

# If source deletion fails after atomic publication, rollback must exchange the
# old destination back, restore source, and discard only the new copied inode.
mv_rollback_src=$T/mv-rollback-src
mv_rollback_dst=$T/mv-rollback-dst
mv_rollback_alias=$T/mv-rollback-alias
printf replacement-data >"$mv_rollback_src"
printf original-data >"$mv_rollback_dst"
ln "$mv_rollback_dst" "$mv_rollback_alias"
mv_rollback_inode=$(stat -c '%d:%i' "$mv_rollback_dst")
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_rollback_src" \
    JASH_FORCE_EXDEV_DST="$mv_rollback_dst" JASH_FAIL_SOURCE_DELETE=1 \
    HOME="$T/home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_rollback_src\",\"$mv_rollback_dst\"]}" \
    >/dev/null 2>&1
mv_rollback_rc=$?
if [[ $mv_rollback_rc -ne 0 && -f "$mv_rollback_src" &&
      $(<"$mv_rollback_src") == replacement-data &&
      $(<"$mv_rollback_dst") == original-data &&
      $(<"$mv_rollback_alias") == original-data &&
      $(stat -c '%d:%i' "$mv_rollback_dst") == "$mv_rollback_inode" &&
      $(stat -c '%d:%i' "$mv_rollback_alias") == "$mv_rollback_inode" ]]; then
    ok "EXDEV mv rollback restores destination aliases and source"
else
    no "EXDEV mv post-publication rollback ownership"
fi

# The EXDEV helper must likewise bind metadata and deletion identity to the
# descriptor opened after rename(2) reports EXDEV.
mv_open_src=$T/mv-open-src
mv_open_replacement=$T/mv-open-replacement
mv_open_saved=$T/mv-open-saved
mv_open_dst=$T/mv-open-dst
printf stale >"$mv_open_src"
printf opened >"$mv_open_replacement"
touch -t 200606060606 "$mv_open_src"
touch -t 200707070707 "$mv_open_replacement"
chmod 0600 "$mv_open_src"
chmod 0751 "$mv_open_replacement"
mv_open_mtime=$(mtime_of "$mv_open_replacement")
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_open_src" \
    JASH_FORCE_EXDEV_DST="$mv_open_dst" JASH_SWAP_ON_OPEN_SRC="$mv_open_src" \
    JASH_SWAP_ON_OPEN_REPLACEMENT="$mv_open_replacement" \
    JASH_SWAP_ON_OPEN_SAVED="$mv_open_saved" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_open_src\",\"$mv_open_dst\"]}" >/dev/null 2>&1
mv_open_rc=$?
if [[ $mv_open_rc -eq 0 && ! -e "$mv_open_src" && -f "$mv_open_saved" &&
      $(<"$mv_open_dst") == opened && $(mode_of "$mv_open_dst") == 751 &&
      $(mtime_of "$mv_open_dst") == "$mv_open_mtime" ]]; then
    ok "EXDEV mv derives metadata and identity from its source descriptor"
else
    no "EXDEV mv used pre-open source metadata or identity"
fi

# Swap source identity after the destination copy closes. The move must fail,
# restore the replacement at src, retain the original, and remove its own dst.
mv_swap_src=$T/mv-swap-src
mv_swap_replacement=$T/mv-swap-replacement
mv_swap_saved=$T/mv-swap-saved
mv_swap_dst=$T/mv-swap-dst
printf original >"$mv_swap_src"
printf replacement >"$mv_swap_replacement"
env LD_PRELOAD="$HOOK" JASH_FORCE_EXDEV_SRC="$mv_swap_src" \
    JASH_FORCE_EXDEV_DST="$mv_swap_dst" JASH_RACE_DST="$mv_swap_dst" \
    JASH_SWAP_SOURCE_ON_CLOSE="$mv_swap_src" \
    JASH_SWAP_SOURCE_REPLACEMENT="$mv_swap_replacement" \
    JASH_SWAP_SOURCE_SAVED="$mv_swap_saved" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --exec-json \
    "{\"argv\":[\"mv\",\"$mv_swap_src\",\"$mv_swap_dst\"]}" >/dev/null 2>&1
mv_swap_rc=$?
if [[ $mv_swap_rc -ne 0 && -f "$mv_swap_src" && $(<"$mv_swap_src") == replacement &&
      -f "$mv_swap_saved" && $(<"$mv_swap_saved") == original && ! -e "$mv_swap_dst" ]]; then
    ok "EXDEV mv source swap fails closed and removes its destination"
else
    no "EXDEV mv source swap cleanup or retention"
fi
if ! compgen -G "$T/.jash-mv-hold-*" >/dev/null; then
    ok "EXDEV mv removes private inode holds after success and recovery"
else
    no "EXDEV mv leaked private inode holds"
fi

# Signal 0 makes an accidental group/broadcast syscall harmless. The preload
# log proves validation happens before sys_kill, not merely after its result.
kill_log=$T/kill.log
env LD_PRELOAD="$HOOK" JASH_KILL_LOG="$kill_log" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" -c 'kill --unsafe -0 -- 0' >/dev/null 2>&1 || true
if [[ ! -s "$kill_log" ]]; then ok "kill rejects PID 0 before sys_kill";
else no "kill passed PID 0 to sys_kill"; fi

rm -f "$kill_log"
env LD_PRELOAD="$HOOK" JASH_KILL_LOG="$kill_log" HOME="$T/home" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" -c 'kill --unsafe -0 -- -1' >/dev/null 2>&1 || true
if [[ ! -s "$kill_log" ]]; then ok "kill rejects negative PIDs before sys_kill";
else no "kill passed a negative PID to sys_kill"; fi

# Linux libc reserves the kernel realtime slots from 32 up to SIGRTMIN. This is
# 32/33 on glibc and also 34 on musl; SIGRTMIN itself must remain valid.
cat >"$T/sigrtmin.c" <<'C'
#include <signal.h>
#include <stdio.h>
int main(void) {
#ifdef SIGRTMIN
    printf("%d\n", SIGRTMIN);
#else
    printf("-1\n");
#endif
    return 0;
}
C
cc "$T/sigrtmin.c" -o "$T/sigrtmin" || exit 2
rtmin=$($T/sigrtmin)
for sig in 32 33 34; do
    run_jash -c "kill --unsafe --dry-run -$sig $$" >/dev/null 2>&1
    signal_rc=$?
    if (( rtmin > sig )); then
        if (( signal_rc != 0 )); then ok "kill rejects platform-reserved signal $sig";
        else no "kill accepted platform-reserved signal $sig"; fi
    elif (( rtmin > 0 )); then
        if (( signal_rc == 0 )); then ok "kill accepts platform realtime signal $sig";
        else no "kill unconditionally rejected platform signal $sig"; fi
    else
        ok "signal $sig reservation test not applicable"
    fi
done

# Exercise the production predicate with musl's SIGRTMIN even on a glibc host.
cat >"$T/musl_reserved.c" <<'C'
#include <signal.h>
#undef SIGRTMIN
#define SIGRTMIN 35
#include "builtins/builtin_kill.c"
int main(void) {
    return !(kill_signal_is_reserved(32) && kill_signal_is_reserved(33) &&
             kill_signal_is_reserved(34) && !kill_signal_is_reserved(35));
}
C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$ROOT/c" \
      "$T/musl_reserved.c" -o "$T/musl_reserved" >/dev/null 2>&1 &&
   "$T/musl_reserved"; then
    ok "kill reserves musl signal 34 without rejecting SIGRTMIN"
else
    no "kill musl realtime reservation boundary"
fi

# Source-level exercise of the generic regular-file restoration path. It must
# consume the hold after success and preserve both names on no-clobber failure.
mkdir "$T/restore-hold" "$T/restore-parent"
cat >"$T/restore_noreplace.c" <<'C'
#include "builtins/builtin_mv.c"

static int put(int dirfd, const char *name, const char *text) {
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len ? 0 : -1;
    close(fd);
    return ok;
}

static int has(int dirfd, const char *name, const char *text) {
    char buf[32] = {0};
    int fd = openat(dirfd, name, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int held = open(argv[1], O_RDONLY | O_DIRECTORY);
    int parent = open(argv[2], O_RDONLY | O_DIRECTORY);
    if (held < 0 || parent < 0) return 3;
    if (put(held, "item", "replacement") != 0 ||
        mv_restore_noreplace(held, "item", parent, "source") != 0 ||
        !has(parent, "source", "replacement") ||
        fstatat(held, "item", &(struct stat){0}, 0) == 0 || errno != ENOENT)
        return 4;
    if (put(held, "item", "replacement") != 0 ||
        put(parent, "occupied", "occupant") != 0)
        return 5;
    errno = 0;
    if (mv_restore_noreplace(held, "item", parent, "occupied") == 0 ||
        errno != EEXIST || !has(parent, "occupied", "occupant") ||
        !has(held, "item", "replacement"))
        return 6;
    close(held);
    close(parent);
    return 0;
}
C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$ROOT/c" \
      "$T/restore_noreplace.c" -o "$T/restore_noreplace" >/dev/null 2>&1 &&
   "$T/restore_noreplace" "$T/restore-hold" "$T/restore-parent"; then
    ok "mv generic restore is regular-file safe and no-clobber"
else
    no "mv generic regular-file restoration"
fi

# The mv generic no-replace fallback has the same publication boundary. A
# failed private-name cleanup must roll back its verified public hard link.
mkdir "$T/.jash-mv-hold-direct" "$T/mv-link-parent"
cat >"$T/mv_link_cleanup.c" <<'C'
#include "builtins/builtin_mv.c"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int held = open(argv[1], O_RDONLY | O_DIRECTORY);
    int parent = open(argv[2], O_RDONLY | O_DIRECTORY);
    int fd = openat(held, "item", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (held < 0 || parent < 0 || fd < 0 || write(fd, "held", 4) != 4 ||
        close(fd) != 0) return 3;
    int rc = mv_link_noreplace_move(held, "item", parent, "published");
    struct stat st;
    int held_exists = fstatat(held, "item", &st, 0) == 0;
    int published_exists = fstatat(parent, "published", &st, 0) == 0;
    close(held);
    close(parent);
    return rc != 0 && held_exists && !published_exists ? 0 : 4;
}
C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections -I"$ROOT/c" \
      "$T/mv_link_cleanup.c" -o "$T/mv_link_cleanup" >/dev/null 2>&1 &&
   env LD_PRELOAD="$HOOK" JASH_FAIL_SOURCE_DELETE=1 \
       "$T/mv_link_cleanup" "$T/.jash-mv-hold-direct" "$T/mv-link-parent"; then
    ok "mv link fallback rolls back publication on cleanup failure"
else
    no "mv link fallback cleanup failure leaked or lost a name"
fi

printf 'cp/mv/kill security regressions: PASS=%d FAIL=%d\n' "$pass" "$fail"
(( fail == 0 ))
