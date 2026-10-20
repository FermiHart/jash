#!/usr/bin/env bash
# Regression tests for reviewed mutator commit/restore/cleanup risks.
set -u

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -n "${JASH:-}" ]]; then
    if ! JASH_SIGIL=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; then
        echo "FAIL: explicit JASH binary cannot run: $JASH"
        exit 1
    fi
else
    JASH=""
    for candidate in "$PROJECT_ROOT/build/jash-dev" \
                     "$PROJECT_ROOT/build/jash-release" \
                     "$PROJECT_ROOT/build/jash" \
                     "$PROJECT_ROOT/build-speed/jash-release"; do
        if JASH_SIGIL=0 "$candidate" -c 'exit 0' >/dev/null 2>&1; then
            JASH="$candidate"
            break
        fi
    done
fi
if [[ -z "$JASH" ]]; then echo "FAIL: no runnable jash binary"; exit 1; fi
if ! command -v cc >/dev/null 2>&1; then echo "FAIL: no C compiler"; exit 1; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
for source in "$PROJECT_ROOT/c/builtins/builtin_sed.c" \
              "$PROJECT_ROOT/c/builtins/builtin_trash.c" \
              "$PROJECT_ROOT/c/builtins/builtin_speculate.c"; do
    if [[ "$source" -nt "$JASH" ]]; then
        echo "FAIL: stale jash binary (older than ${source#$PROJECT_ROOT/})"
        exit 1
    fi
done
if ! grep -aFq 'target changed during commit' "$JASH" ||
   ! grep -aFq 'cannot restore %s: %s' "$JASH"; then
    echo "FAIL: stale jash binary (missing mutator safety paths)"
    exit 1
fi

T="$(mktemp -d /tmp/jash_jinv_mutators.XXXXXX)"
X=""
trap 'chmod -R u+w "$T" ${X:+"$X"} 2>/dev/null || true; rm -rf "$T" ${X:+"$X"}' EXIT
HOME_DIR="$T/home"
mkdir -p "$HOME_DIR/.jash/trash"

pass=0
fail=0
ok(){ pass=$((pass+1)); echo "  PASS $1"; }
no(){ fail=$((fail+1)); echo "  FAIL $1${2:+ ($2)}"; }
RUN(){ env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" "$@"; }

# Fault injection is limited to the sed target/temp streams. The openat hook
# swaps a cleanup directory after it is pinned and before fdopendir traverses it.
cat > "$T/faults.c" <<'C'
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif

static int fd_path(int fd, char *buf, size_t cap) {
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, buf, cap - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return 1;
}

static int path_at(int dirfd, const char *name, char *buf, size_t cap) {
    if (!name) return 0;
    if (name[0] == '/')
        return snprintf(buf, cap, "%s", name) < (int)cap;
    char parent[4096];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(parent, sizeof(parent))) return 0;
    } else if (!fd_path(dirfd, parent, sizeof(parent))) {
        return 0;
    }
    return snprintf(buf, cap, "%s/%s", parent, name) < (int)cap;
}

static int sed_input_fd(int fd) {
    char path[4096];
    const char *target = getenv("JINV_SED_TARGET");
    return target && fd_path(fd, path, sizeof(path)) && strcmp(path, target) == 0;
}

static int sed_temp_fd(int fd) {
    char path[4096], prefix[4096];
    const char *target = getenv("JINV_SED_TARGET");
    if (!target || !fd_path(fd, path, sizeof(path))) return 0;
    snprintf(prefix, sizeof(prefix), "%s.jsed.", target);
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static int mode_is(const char *mode) {
    const char *active = getenv("JINV_FAIL");
    return active && strcmp(active, mode) == 0;
}

static void write_marker(const char *env_name, const char *text) {
    const char *path = getenv(env_name);
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)write(fd, text, strlen(text));
    close(fd);
}

static int (*get_renameat2(void))(int, const char *, int, const char *, unsigned int) {
    static int (*real_renameat2)(int, const char *, int, const char *, unsigned int);
    if (!real_renameat2) real_renameat2 = dlsym(RTLD_NEXT, "renameat2");
    return real_renameat2;
}
static char spec_final_original[256];

static void exchange_env_paths(const char *left_env, const char *right_env,
                               const char *marker_env) {
    const char *left = getenv(left_env);
    const char *right = getenv(right_env);
    int (*real_renameat2)(int, const char *, int, const char *, unsigned int) =
        get_renameat2();
    if (left && right && real_renameat2 &&
        real_renameat2(AT_FDCWD, left, AT_FDCWD, right, RENAME_EXCHANGE) == 0)
        write_marker(marker_env, "swapped\n");
}

char *fgets(char *s, int size, FILE *stream) {
    static char *(*real_fgets)(char *, int, FILE *);
    if (!real_fgets) real_fgets = dlsym(RTLD_NEXT, "fgets");
    if (mode_is("read") && sed_input_fd(fileno(stream))) {
        errno = EIO;
        return NULL;
    }
    return real_fgets(s, size, stream);
}

int ferror(FILE *stream) {
    static int (*real_ferror)(FILE *);
    if (!real_ferror) real_ferror = dlsym(RTLD_NEXT, "ferror");
    if (mode_is("read") && sed_input_fd(fileno(stream))) return 1;
    return real_ferror(stream);
}

int fputs(const char *s, FILE *stream) {
    static int (*real_fputs)(const char *, FILE *);
    if (!real_fputs) real_fputs = dlsym(RTLD_NEXT, "fputs");
    if (mode_is("write") && sed_temp_fd(fileno(stream))) {
        errno = EIO;
        return EOF;
    }
    char path[4096];
    if (mode_is("trash-meta-write") && fd_path(fileno(stream), path, sizeof(path)) &&
        strstr(path, "/.jash/trash/.jmeta.")) {
        write_marker("JINV_TRASH_FAULT_MARKER", "meta-write\n");
        errno = EIO;
        return EOF;
    }
    return real_fputs(s, stream);
}

int fflush(FILE *stream) {
    static int (*real_fflush)(FILE *);
    if (!real_fflush) real_fflush = dlsym(RTLD_NEXT, "fflush");
    if (stream && mode_is("fflush") && sed_temp_fd(fileno(stream))) {
        errno = EIO;
        return EOF;
    }
    return real_fflush(stream);
}

int fchmod(int fd, mode_t mode) {
    static int (*real_fchmod)(int, mode_t);
    if (!real_fchmod) real_fchmod = dlsym(RTLD_NEXT, "fchmod");
    if (mode_is("fchmod") && sed_temp_fd(fd)) {
        errno = EIO;
        return -1;
    }
    return real_fchmod(fd, mode);
}

int fsync(int fd) {
    static int (*real_fsync)(int);
    static int dir_sync_count;
    if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, "fsync");
    if (mode_is("fsync") && sed_temp_fd(fd)) {
        errno = EIO;
        return -1;
    }
    char path[4096];
    const char *sed_dir = getenv("JINV_SED_FSYNC_DIR");
    if (mode_is("sed-dir-fsync") && sed_dir &&
        fd_path(fd, path, sizeof(path)) && strcmp(path, sed_dir) == 0) {
        write_marker("JINV_SED_FSYNC_MARKER", "dir-fsync\n");
        errno = EIO;
        return -1;
    }
    const char *fail_dir = getenv("JINV_TRASH_FSYNC_DIR");
    struct stat sync_st;
    const char *source_sync = getenv("JINV_TRASH_SOURCE_SYNC");
    if (mode_is("trash-source-fsync") && source_sync &&
        fd_path(fd, path, sizeof(path)) && strcmp(path, source_sync) == 0 &&
        fstat(fd, &sync_st) == 0 && S_ISREG(sync_st.st_mode)) {
        write_marker("JINV_TRASH_FAULT_MARKER", "source-fsync\n");
        errno = EIO;
        return -1;
    }
    if (mode_is("trash-dir-fsync") && fail_dir &&
        fd_path(fd, path, sizeof(path)) && strcmp(path, fail_dir) == 0 &&
        fstat(fd, &sync_st) == 0 && S_ISDIR(sync_st.st_mode)) {
        const char *nth_env = getenv("JINV_TRASH_FSYNC_N");
        int nth = nth_env ? atoi(nth_env) : 1;
        if (++dir_sync_count == nth) {
            write_marker("JINV_TRASH_FAULT_MARKER", "dir-fsync\n");
            errno = EIO;
            return -1;
        }
    }
    if (mode_is("trash-forward-reappear") && fail_dir &&
        fd_path(fd, path, sizeof(path)) && strcmp(path, fail_dir) == 0) {
        const char *source = getenv("JINV_TRASH_SOURCE");
        const char *decoy = getenv("JINV_TRASH_SOURCE_DECOY");
        if (source && decoy && rename(decoy, source) == 0)
            write_marker("JINV_TRASH_FAULT_MARKER", "reappeared\n");
        errno = EIO;
        return -1;
    }
    int rc = real_fsync(fd);
    const char *dest = getenv("JINV_TRASH_DEST");
    if (rc == 0 && dest && fd_path(fd, path, sizeof(path)) && strcmp(path, dest) == 0) {
        if (mode_is("trash-source-swap"))
            exchange_env_paths("JINV_TRASH_SOURCE", "JINV_TRASH_SOURCE_DECOY",
                               "JINV_TRASH_SWAP_MARKER");
        else if (mode_is("trash-dest-swap"))
            exchange_env_paths("JINV_TRASH_DEST", "JINV_TRASH_DEST_DECOY",
                               "JINV_TRASH_SWAP_MARKER");
    }
    return rc;
}

int fclose(FILE *stream) {
    static int (*real_fclose)(FILE *);
    if (!real_fclose) real_fclose = dlsym(RTLD_NEXT, "fclose");
    int is_temp = sed_temp_fd(fileno(stream));
    int injected = (mode_is("fclose-input") && sed_input_fd(fileno(stream))) ||
                   (mode_is("fclose-output") && is_temp);
    char close_path[4096];
    int meta_close = mode_is("trash-meta-close") &&
                     fd_path(fileno(stream), close_path, sizeof(close_path)) &&
                     strstr(close_path, "/.jash/trash/.jmeta.");
    int rc = real_fclose(stream);
    if (mode_is("replace-target") && is_temp) {
        const char *replacement = getenv("JINV_SED_REPLACEMENT");
        const char *target = getenv("JINV_SED_TARGET");
        if (replacement && target && rename(replacement, target) == 0)
            write_marker("JINV_SED_SWAP_MARKER", "swapped\n");
    }
    if (injected) {
        errno = EIO;
        return EOF;
    }
    if (meta_close) {
        write_marker("JINV_TRASH_FAULT_MARKER", "meta-close\n");
        errno = EIO;
        return EOF;
    }
    return rc;
}

ssize_t write(int fd, const void *buf, size_t count) {
    static ssize_t (*real_write)(int, const void *, size_t);
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    char path[4096];
    if ((mode_is("trash-forward-write") ||
         mode_is("trash-forward-write-reappear")) &&
        fd_path(fd, path, sizeof(path)) &&
        strstr(path, "/.jash/trash/") && strstr(path, ".jtrash.")) {
        if (mode_is("trash-forward-write-reappear")) {
            const char *source = getenv("JINV_TRASH_SOURCE");
            const char *replacement = getenv("JINV_TRASH_SOURCE_DECOY");
            if (source && replacement && rename(replacement, source) == 0)
                write_marker("JINV_TRASH_SWAP_MARKER", "source-reappeared\n");
        }
        write_marker("JINV_TRASH_FAULT_MARKER", "write\n");
        errno = EIO;
        return -1;
    }
    return real_write(fd, buf, count);
}

int close(int fd) {
    static int (*real_close)(int);
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    char path[4096];
    int inject = mode_is("trash-forward-close") &&
                 fd_path(fd, path, sizeof(path)) &&
                 strstr(path, "/.jash/trash/") && strstr(path, ".jtrash.");
    int rc = real_close(fd);
    if (inject) {
        write_marker("JINV_TRASH_FAULT_MARKER", "close\n");
        errno = EIO;
        return -1;
    }
    return rc;
}

int unlinkat(int dirfd, const char *name, int flags) {
    static int (*real_unlinkat)(int, const char *, int);
    static int injected;
    if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    if (!injected && mode_is("trash-final-unlink-swap") &&
        strncmp(name, ".jhold.", 7) == 0) {
        char parent[4096], hold[4096];
        const char *decoy = getenv("JINV_TRASH_SOURCE_DECOY");
        int (*real_renameat2)(int, const char *, int, const char *, unsigned int) =
            get_renameat2();
        if (decoy && real_renameat2 && fd_path(dirfd, parent, sizeof(parent)) &&
            snprintf(hold, sizeof(hold), "%s/%s", parent, name) < (int)sizeof(hold) &&
            real_renameat2(AT_FDCWD, hold, AT_FDCWD, decoy,
                           RENAME_EXCHANGE) == 0) {
            injected = 1;
            write_marker("JINV_TRASH_SWAP_MARKER", "final-unlink-swap\n");
            errno = EBUSY;
            return -1;
        }
    }
    if (!injected && mode_is("trash-source-reappear-before-delete") &&
        strncmp(name, ".jhold.", 7) == 0) {
        const char *source = getenv("JINV_TRASH_SOURCE");
        const char *replacement = getenv("JINV_TRASH_SOURCE_DECOY");
        if (source && replacement && rename(replacement, source) == 0) {
            injected = 1;
            write_marker("JINV_TRASH_SWAP_MARKER", "source-reappeared\n");
            errno = EBUSY;
            return -1;
        }
    }
    if (!injected && mode_is("spec-final-dir-replace") &&
        strncmp(name, ".jspec.", 7) == 0 && (flags & AT_REMOVEDIR)) {
        char parent[4096], flip[4096];
        const char *victim = getenv("JINV_SPEC_VICTIM");
        const char *root_suffix = "/.jash/speculate";
        if (victim && spec_final_original[0] &&
            fd_path(dirfd, parent, sizeof(parent)) &&
            strlen(parent) >= strlen(root_suffix) &&
            strcmp(parent + strlen(parent) - strlen(root_suffix), root_suffix) == 0 &&
            snprintf(flip, sizeof(flip), "%s/%s", parent,
                     spec_final_original) < (int)sizeof(flip) &&
            rename(victim, flip) == 0) {
            injected = 1;
            write_marker("JINV_SPEC_FINAL_MARKER", flip);
            write_marker("JINV_SPEC_FINAL_MARKER", "\n");
        }
    }
    return real_unlinkat(dirfd, name, flags);
}

int renameat2(int olddirfd, const char *oldpath, int newdirfd,
              const char *newpath, unsigned int flags) {
    static int injected;
    static int sed_exchange_count;
    int (*real_renameat2)(int, const char *, int, const char *, unsigned int) =
        get_renameat2();
    const char *target = getenv("JINV_SED_TARGET");
    char resolved_new[4096];
    int target_match = target &&
        path_at(newdirfd, newpath, resolved_new, sizeof(resolved_new)) &&
        strcmp(resolved_new, target) == 0;
    if (real_renameat2 && mode_is("spec-final-dir-replace") &&
        strncmp(newpath, ".jspec.", 7) == 0) {
        char parent[4096];
        const char *root_suffix = "/.jash/speculate";
        if (fd_path(newdirfd, parent, sizeof(parent)) &&
            strlen(parent) >= strlen(root_suffix) &&
            strcmp(parent + strlen(parent) - strlen(root_suffix), root_suffix) == 0)
            snprintf(spec_final_original, sizeof(spec_final_original), "%s", oldpath);
    }
    if (real_renameat2 && mode_is("replace-at-rollback") &&
        (flags & RENAME_EXCHANGE) && target_match) {
        const char *replacement;
        sed_exchange_count++;
        replacement = getenv(sed_exchange_count == 1
                             ? "JINV_SED_REPLACEMENT"
                             : "JINV_SED_SECOND_REPLACEMENT");
        if (replacement && rename(replacement, target) == 0)
            write_marker("JINV_SED_SWAP_MARKER",
                         sed_exchange_count == 1 ? "first\n" : "second\n");
    }
    if (!injected && real_renameat2 && mode_is("replace-at-exchange") &&
        (flags & RENAME_EXCHANGE) && target_match) {
        const char *replacement = getenv("JINV_SED_REPLACEMENT");
        if (replacement && rename(replacement, target) == 0) {
            injected = 1;
            write_marker("JINV_SED_SWAP_MARKER", "swapped\n");
        }
    }
    if (!injected && real_renameat2 && mode_is("trash-hold-swap") &&
        strncmp(newpath, ".jhold.", 7) == 0) {
        const char *source = getenv("JINV_TRASH_SOURCE");
        const char *decoy = getenv("JINV_TRASH_SOURCE_DECOY");
        if (source && decoy &&
            real_renameat2(AT_FDCWD, source, AT_FDCWD, decoy,
                           RENAME_EXCHANGE) == 0) {
            injected = 1;
            write_marker("JINV_TRASH_SWAP_MARKER", "hold-swap\n");
        }
    }
    if (real_renameat2 && mode_is("trash-meta-publish") &&
        strncmp(oldpath, ".jmeta.", 7) == 0 && strstr(newpath, ".meta")) {
        write_marker("JINV_TRASH_FAULT_MARKER", "meta-publish\n");
        errno = EIO;
        return -1;
    }
    if (!real_renameat2) { errno = ENOSYS; return -1; }
    return real_renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
}

int fstatat(int dirfd, const char *name, struct stat *st, int flags) {
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    static int swapped;
    if (!real_fstatat) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    int rc = real_fstatat(dirfd, name, st, flags);
    const char *victim = getenv("JINV_SPEC_VICTIM");
    char parent[4096], flip[4096];
    int should_swap = mode_is("spec-preunlink-replace");
    if (!swapped && rc == 0 && should_swap && victim &&
        strcmp(name, "flip") == 0 && (flags & AT_SYMLINK_NOFOLLOW) &&
        fd_path(dirfd, parent, sizeof(parent)) &&
        strstr(parent, "/.jash/speculate/")) {
        int (*real_renameat2)(int, const char *, int, const char *, unsigned int) =
            get_renameat2();
        if (real_renameat2 &&
            snprintf(flip, sizeof(flip), "%s/%s", parent, name) < (int)sizeof(flip) &&
            real_renameat2(AT_FDCWD, flip, AT_FDCWD, victim,
                           RENAME_EXCHANGE) == 0) {
            swapped = 1;
            const char *marker = mode_is("spec-final-dir-replace")
                                 ? "JINV_SPEC_FINAL_MARKER"
                                 : "JINV_SPEC_PREUNLINK_MARKER";
            write_marker(marker, flip);
            write_marker(marker, "\n");
        }
    }
    return rc;
}

int openat(int dirfd, const char *name, int flags, ...) {
    static int (*real_openat)(int, const char *, int, ...);
    static int swapped;
    mode_t create_mode = 0;
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        create_mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    const char *victim = getenv("JINV_SPEC_VICTIM");
    char parent[4096];
    if (!swapped && mode_is("spec-preopen-replace") && victim &&
        strcmp(name, "flip") == 0 && (flags & O_DIRECTORY) &&
        fd_path(dirfd, parent, sizeof(parent))) {
        char flip[4096];
        int (*real_renameat2)(int, const char *, int, const char *, unsigned int) =
            get_renameat2();
        if (real_renameat2 &&
            snprintf(flip, sizeof(flip), "%s/%s", parent, name) < (int)sizeof(flip) &&
            real_renameat2(AT_FDCWD, flip, AT_FDCWD, victim,
                           RENAME_EXCHANGE) == 0) {
            swapped = 1;
            write_marker("JINV_SPEC_PREOPEN_MARKER", flip);
            write_marker("JINV_SPEC_PREOPEN_MARKER", "\n");
        }
    }
    int fd = (flags & O_CREAT) ? real_openat(dirfd, name, flags, create_mode)
                               : real_openat(dirfd, name, flags);
    if (!swapped && fd >= 0 && mode_is("spec-swap") && victim &&
        strcmp(name, "flip") == 0 && (flags & O_DIRECTORY) &&
        fd_path(dirfd, parent, sizeof(parent)) &&
        strstr(parent, "/.jash/speculate/")) {
        if (unlinkat(dirfd, name, AT_REMOVEDIR) == 0 &&
            symlinkat(victim, dirfd, name) == 0) {
            swapped = 1;
            char flip[4096];
            if (snprintf(flip, sizeof(flip), "%s/%s", parent, name) < (int)sizeof(flip)) {
                write_marker("JINV_SPEC_SWAP_MARKER", flip);
                write_marker("JINV_SPEC_SWAP_MARKER", "\n");
            }
        }
    }
    return fd;
}

DIR *fdopendir(int fd) {
    static DIR *(*real_fdopendir)(int);
    if (!real_fdopendir) real_fdopendir = dlsym(RTLD_NEXT, "fdopendir");
    char path[4096];
    if (mode_is("spec-swap") && fd_path(fd, path, sizeof(path)) &&
        strstr(path, "/.jash/speculate/") && strstr(path, "/flip"))
        write_marker("JINV_SPEC_FDOPENDIR_MARKER", "traversed\n");
    return real_fdopendir(fd);
}
C
if ! cc -shared -fPIC -O0 "$T/faults.c" -ldl -o "$T/faults.so"; then
    echo "FAIL: could not build fault injector"
    exit 1
fi

sed_target="$T/sed-target.txt"
for mode in read write fflush fchmod fsync fclose-input fclose-output; do
    printf 'original line\n' > "$sed_target"
    env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
        JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
        LD_PRELOAD="$T/faults.so" JINV_FAIL="$mode" JINV_SED_TARGET="$sed_target" \
        "$JASH" -c "sed -i 's/original/changed/' $sed_target" >/dev/null 2>&1
    rc=$?
    if [[ "$rc" -ne 0 && "$(<"$sed_target")" == "original line" ]]; then
        ok "sed -i preserves original on $mode failure"
    else
        no "sed -i preserves original on $mode failure" "rc=$rc content=$(<"$sed_target")"
    fi
done

sed_replacement="$T/sed-replacement.txt"
sed_swap_marker="$T/sed-swap.marker"
printf 'original line\n' > "$sed_target"
printf 'replacement won\n' > "$sed_replacement"
env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=replace-target JINV_SED_TARGET="$sed_target" \
    JINV_SED_REPLACEMENT="$sed_replacement" JINV_SED_SWAP_MARKER="$sed_swap_marker" \
    "$JASH" -c "sed -i 's/original/changed/' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 && -f "$sed_swap_marker" ]] &&
   [[ "$(<"$sed_target")" == "replacement won" ]]; then
    ok "sed -i refuses to clobber a replacement target inode"
else
    no "sed -i refuses to clobber a replacement target inode" \
       "rc=$rc swapped=$([[ -f $sed_swap_marker ]] && echo yes || echo no) content=$(<"$sed_target")"
fi

sed_after_check="$T/sed-after-check.txt"
sed_after_marker="$T/sed-after-check.marker"
printf 'original line\n' > "$sed_target"
printf 'after-check replacement\n' > "$sed_after_check"
env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=replace-at-exchange \
    JINV_SED_TARGET="$sed_target" JINV_SED_REPLACEMENT="$sed_after_check" \
    JINV_SED_SWAP_MARKER="$sed_after_marker" \
    "$JASH" -c "sed -i 's/original/changed/' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 && -f "$sed_after_marker" ]] &&
   [[ "$(<"$sed_target")" == "after-check replacement" ]]; then
    ok "sed -i rolls back a target swapped at atomic commit"
else
    no "sed -i rolls back a target swapped at atomic commit" \
       "rc=$rc swapped=$([[ -f $sed_after_marker ]] && echo yes || echo no) content=$(<"$sed_target")"
fi

sed_second_replacement="$T/sed-second-replacement.txt"
sed_second_marker="$T/sed-second-race.marker"
printf 'original line\n' > "$sed_target"
printf 'first replacement\n' > "$sed_after_check"
printf 'second replacement must survive\n' > "$sed_second_replacement"
env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=replace-at-rollback \
    JINV_SED_TARGET="$sed_target" JINV_SED_REPLACEMENT="$sed_after_check" \
    JINV_SED_SECOND_REPLACEMENT="$sed_second_replacement" \
    JINV_SED_SWAP_MARKER="$sed_second_marker" \
    "$JASH" -c "sed -i 's/original/changed/' $sed_target" >/dev/null 2>&1
rc=$?
sed_second_survivor="$(grep -l 'second replacement must survive' \
    "$sed_target" "$sed_target".jsed.* 2>/dev/null || true)"
if [[ "$rc" -ne 0 && "$(wc -l < "$sed_second_marker" 2>/dev/null || echo 0)" -eq 2 &&
      -n "$sed_second_survivor" ]]; then
    ok "sed -i rollback never deletes a second replacement inode"
else
    no "sed -i rollback never deletes a second replacement inode" \
       "rc=$rc survivor=${sed_second_survivor:-none}"
fi

sed_parent_marker="$T/sed-parent-fsync.marker"
printf 'original line\n' > "$sed_target"
env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=sed-dir-fsync \
    JINV_SED_TARGET="$sed_target" JINV_SED_FSYNC_DIR="$T" \
    JINV_SED_FSYNC_MARKER="$sed_parent_marker" \
    "$JASH" -c "sed -i 's/original/changed/' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 && -f "$sed_parent_marker" &&
      "$(<"$sed_target")" == "original line" ]]; then
    ok "sed -i rolls back a parent-directory fsync failure"
else
    no "sed -i rolls back a parent-directory fsync failure" "rc=$rc"
fi

printf 'aaaa\n' > "$sed_target"
RUN -c "sed -E -i 's|^(a+)$|prefix-\\1-suffix|' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -eq 0 && "$(<"$sed_target")" == "prefix-aaaa-suffix" ]]; then
    ok "sed -i preserves normal backreference expansion"
else
    no "sed -i preserves normal backreference expansion" "rc=$rc"
fi

printf -v sed_long_pattern '%*s' 2048 ''
sed_long_pattern=${sed_long_pattern// /a}
printf '%sZ\n' "$sed_long_pattern" > "$sed_target"
cp "$sed_target" "$T/sed-before.txt"
RUN -c "sed -i 's|${sed_long_pattern}|x|' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 ]] && cmp -s "$sed_target" "$T/sed-before.txt"; then
    ok "sed -i rejects an over-capacity pattern without truncating the file"
else
    no "sed -i rejects an over-capacity pattern without truncating the file" "rc=$rc"
fi

sed_logical_line="$T/sed-logical-line.txt"
printf -v sed_long_line '%*s' 131072 ''
sed_long_line=${sed_long_line// /a}
printf '%sb\n' "$sed_long_line" > "$sed_logical_line"
RUN -c "sed -i 's/^a*b\$/matched/' $sed_logical_line" >/dev/null 2>&1
rc=$?
if [[ "$rc" -eq 0 && "$(<"$sed_logical_line")" == "matched" ]]; then
    ok "sed treats a greater-than-64KiB logical line as one pattern space"
else
    no "sed treats a greater-than-64KiB logical line as one pattern space" \
       "rc=$rc bytes=$(wc -c < "$sed_logical_line")"
fi

sed_over_cap="$T/sed-over-cap.txt"
dd if=/dev/zero bs=1048576 count=17 2>/dev/null | tr '\0' a > "$sed_over_cap"
cp "$sed_over_cap" "$T/sed-over-cap-before.txt"
RUN -c "sed -i 's/never-present/x/' $sed_over_cap" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 ]] && cmp -s "$sed_over_cap" "$T/sed-over-cap-before.txt"; then
    ok "sed -i rejects a logical line above its explicit cap without publication"
else
    no "sed -i rejects a logical line above its explicit cap without publication" "rc=$rc"
fi

printf -v sed_long_replacement '%*s' 2048 ''
sed_long_replacement=${sed_long_replacement// /r}
printf 'needle\n' > "$sed_target"
cp "$sed_target" "$T/sed-before.txt"
RUN -c "sed -i 's|needle|${sed_long_replacement}|' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 ]] && cmp -s "$sed_target" "$T/sed-before.txt"; then
    ok "sed -i rejects an over-capacity replacement without truncating the file"
else
    no "sed -i rejects an over-capacity replacement without truncating the file" "rc=$rc"
fi

printf -v sed_long_capture '%*s' 5000 ''
sed_long_capture=${sed_long_capture// /a}
printf '%s\n' "$sed_long_capture" > "$sed_target"
cp "$sed_target" "$T/sed-before.txt"
RUN -c "sed -E -i 's|^(a+)$|\\1\\1|' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 ]] && cmp -s "$sed_target" "$T/sed-before.txt"; then
    ok "sed -i rejects over-capacity backreference expansion without truncating the file"
else
    no "sed -i rejects over-capacity backreference expansion without truncating the file" "rc=$rc"
fi

printf -v sed_wide_replacement '%*s' 1024 ''
sed_wide_replacement=${sed_wide_replacement// /r}
printf -v sed_many_matches '%*s' 200 ''
sed_many_matches=${sed_many_matches// /a}
printf '%s\n' "$sed_many_matches" > "$sed_target"
cp "$sed_target" "$T/sed-before.txt"
RUN -c "sed -i 's|a|${sed_wide_replacement}|g' $sed_target" >/dev/null 2>&1
rc=$?
if [[ "$rc" -ne 0 ]] && cmp -s "$sed_target" "$T/sed-before.txt"; then
    ok "sed -i rejects over-capacity final output without truncating the file"
else
    no "sed -i rejects over-capacity final output without truncating the file" "rc=$rc"
fi

trash_dir="$HOME_DIR/.jash/trash"
restore_root="$T/restore-root"
outside="$T/outside"
mkdir -p "$restore_root/relative" "$HOME_DIR/absolute" "$outside"

printf 'relative data\n' > "$trash_dir/100_relative.txt"
printf 'relative\n' > "$trash_dir/100_relative.txt.meta"
(cd "$restore_root" && RUN -c 'trash restore relative.txt') >/dev/null 2>&1
if [[ -f "$restore_root/relative/relative.txt" ]] &&
   [[ "$(<"$restore_root/relative/relative.txt")" == "relative data" ]]; then
    ok "trash restore accepts a legitimate relative original directory"
else
    no "trash restore accepts a legitimate relative original directory"
fi

printf 'absolute data\n' > "$trash_dir/101_absolute.txt"
printf '%s\n' "$HOME_DIR/absolute" > "$trash_dir/101_absolute.txt.meta"
RUN -c 'trash restore absolute.txt' >/dev/null 2>&1
if [[ -f "$HOME_DIR/absolute/absolute.txt" ]] &&
   [[ "$(<"$HOME_DIR/absolute/absolute.txt")" == "absolute data" ]]; then
    ok "trash restore accepts a legitimate HOME-absolute original directory"
else
    no "trash restore accepts a legitimate HOME-absolute original directory"
fi

printf 'traversal data\n' > "$trash_dir/102_traversal.txt"
printf '../outside\n' > "$trash_dir/102_traversal.txt.meta"
(cd "$restore_root" && RUN -c 'trash restore traversal.txt') >/dev/null 2>&1
if [[ ! -e "$T/outside/traversal.txt" && -e "$trash_dir/102_traversal.txt" ]]; then
    ok "trash restore rejects relative traversal"
else
    no "trash restore rejects relative traversal"
fi

ln -s "$outside" "$restore_root/link"
printf 'relative symlink data\n' > "$trash_dir/103_relative_symlink.txt"
printf 'link\n' > "$trash_dir/103_relative_symlink.txt.meta"
(cd "$restore_root" && RUN -c 'trash restore relative_symlink.txt') >/dev/null 2>&1
if [[ ! -e "$outside/relative_symlink.txt" && -e "$trash_dir/103_relative_symlink.txt" ]]; then
    ok "trash restore rejects relative symlink escape"
else
    no "trash restore rejects relative symlink escape"
fi

ln -s "$outside" "$HOME_DIR/absolute-link"
printf 'absolute symlink data\n' > "$trash_dir/104_absolute_symlink.txt"
printf '%s\n' "$HOME_DIR/absolute-link" > "$trash_dir/104_absolute_symlink.txt.meta"
RUN -c 'trash restore absolute_symlink.txt' >/dev/null 2>&1
if [[ ! -e "$outside/absolute_symlink.txt" && -e "$trash_dir/104_absolute_symlink.txt" ]]; then
    ok "trash restore rejects HOME-absolute symlink escape"
else
    no "trash restore rejects HOME-absolute symlink escape"
fi

printf 'trashed version\n' > "$trash_dir/105_existing.txt"
printf 'relative\n' > "$trash_dir/105_existing.txt.meta"
printf 'existing version\n' > "$restore_root/relative/existing.txt"
existing_out="$(cd "$restore_root" && RUN -c 'trash restore existing.txt' 2>&1)"
existing_rc=$?
if [[ "$existing_rc" -ne 0 && "$existing_out" == *"cannot restore"* &&
      "$(<"$restore_root/relative/existing.txt")" == "existing version" &&
      -e "$trash_dir/105_existing.txt" && -e "$trash_dir/105_existing.txt.meta" ]]; then
    ok "trash restore no-clobber fails nonzero and preserves both copies"
else
    no "trash restore no-clobber fails nonzero and preserves both copies" \
       "rc=$existing_rc output=$existing_out"
fi

if [[ ! -d /dev/shm || ! -w /dev/shm ]]; then
    echo "FAIL: /dev/shm is required for deterministic EXDEV tests"
    exit 1
fi
X="$(mktemp -d /dev/shm/jash_jinv_mutators.XXXXXX)" || {
    echo "FAIL: cannot create EXDEV fixture"; exit 1;
}
if [[ "$(stat -c %d "$T")" == "$(stat -c %d "$X")" ]]; then
    echo "FAIL: EXDEV fixtures are on the same filesystem"
    exit 1
fi

source_item="$trash_dir/106_exdev_source.txt"
source_decoy="$trash_dir/source-swap-decoy"
source_dest="$X/exdev_source.txt"
source_marker="$T/trash-source-swap.marker"
printf 'trusted trash data\n' > "$source_item"
printf '.\n' > "$source_item.meta"
printf 'changed trash entry\n' > "$source_decoy"
source_out="$(cd "$X" && env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-hold-swap \
    JINV_TRASH_SOURCE="$source_item" JINV_TRASH_SOURCE_DECOY="$source_decoy" \
    JINV_TRASH_DEST="$source_dest" JINV_TRASH_SWAP_MARKER="$source_marker" \
    "$JASH" -c 'trash restore exdev_source.txt' 2>&1)"
source_rc=$?
if [[ "$source_rc" -ne 0 && -f "$source_marker" &&
      "$(<"$source_item")" == "changed trash entry" &&
      "$(<"$source_decoy")" == "trusted trash data" && -e "$source_item.meta" ]]; then
    ok "EXDEV restore rolls back a source swap at quarantine capture"
else
    no "EXDEV restore rolls back a source swap at quarantine capture" \
       "rc=$source_rc output=$source_out"
fi

final_item="$trash_dir/109_final_unlink.txt"
final_decoy="$trash_dir/final-unlink-decoy"
final_dest="$X/final_unlink.txt"
final_marker="$T/final-unlink-swap.marker"
printf 'final trusted data\n' > "$final_item"
printf '.\n' > "$final_item.meta"
printf 'final unrelated data\n' > "$final_decoy"
final_out="$(cd "$X" && env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-final-unlink-swap \
    JINV_TRASH_SOURCE_DECOY="$final_decoy" \
    JINV_TRASH_SWAP_MARKER="$final_marker" \
    "$JASH" -c 'trash restore final_unlink.txt' 2>&1)"
final_rc=$?
if [[ "$final_rc" -ne 0 && -f "$final_marker" && -f "$final_item" &&
      "$(<"$final_item")" == "final unrelated data" &&
      "$(<"$final_decoy")" == "final trusted data" &&
      ! -e "$final_dest" && -e "$final_item.meta" &&
      "$final_out" == *"$final_item.meta"* ]]; then
    ok "EXDEV restore rolls back a swap at final held-inode deletion"
else
    no "EXDEV restore rolls back a swap at final held-inode deletion" \
       "rc=$final_rc output=$final_out"
fi

missing_out="$(cd "$X" && RUN -c 'trash definitely-missing-input' 2>&1)"
missing_rc=$?
if [[ "$missing_rc" -ne 0 && "$missing_out" == *"not found"* ]]; then
    ok "forward trash returns nonzero for missing input"
else
    no "forward trash returns nonzero for missing input" "rc=$missing_rc output=$missing_out"
fi

source_sync="$HOME_DIR/source-data-sync.txt"
source_sync_marker="$T/source-data-sync.marker"
printf 'source data must be durable\n' > "$source_sync"
source_sync_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-source-fsync \
    JINV_TRASH_SOURCE_SYNC="$source_sync" \
    JINV_TRASH_FAULT_MARKER="$source_sync_marker" \
    "$JASH" -c "trash $source_sync" 2>&1)"
source_sync_rc=$?
if [[ "$source_sync_rc" -ne 0 && -f "$source_sync_marker" &&
      -f "$source_sync" && "$(<"$source_sync")" == "source data must be durable" ]] &&
   ! compgen -G "$trash_dir/*_source-data-sync.txt" >/dev/null; then
    ok "same-filesystem trash fsyncs source data before namespace mutation"
else
    no "same-filesystem trash fsyncs source data before namespace mutation" \
       "rc=$source_sync_rc output=$source_sync_out"
fi

alias_source="$X/forward_alias.txt"
alias_peer="$X/forward_alias_peer.txt"
printf 'alias data\n' > "$alias_source"
ln "$alias_source" "$alias_peer"
alias_out="$(RUN -c "trash $alias_source" 2>&1)"
alias_rc=$?
alias_items=("$trash_dir"/*_forward_alias.txt)
if [[ "$alias_rc" -eq 0 && ! -e "$alias_source" && -f "$alias_peer" &&
      "$(<"$alias_peer")" == "alias data" && ${#alias_items[@]} -eq 1 &&
      "$(<"${alias_items[0]}")" == "alias data" && -f "${alias_items[0]}.meta" ]]; then
    ok "forward EXDEV trash preserves existing hardlink aliases"
else
    no "forward EXDEV trash preserves existing hardlink aliases" \
       "rc=$alias_rc output=$alias_out"
fi

run_forward_fault() {
    local mode="$1" name="$2"
    local source="$X/$name" marker="$T/$name.marker"
    printf 'forward fault data\n' > "$source"
    local output rc
    output="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
        JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
        LD_PRELOAD="$T/faults.so" JINV_FAIL="$mode" \
        JINV_TRASH_FAULT_MARKER="$marker" "$JASH" -c "trash $source" 2>&1)"
    rc=$?
    if [[ "$rc" -ne 0 && -f "$marker" && -f "$source" ]] &&
       ! compgen -G "$trash_dir/*_$name" >/dev/null; then
        ok "forward EXDEV trash rolls back $mode failure"
    else
        no "forward EXDEV trash rolls back $mode failure" \
           "rc=$rc marker=$([[ -f $marker ]] && echo yes || echo no) output=$output"
    fi
}

run_forward_fault trash-forward-write forward_write.txt
run_forward_fault trash-forward-close forward_close.txt
run_forward_fault trash-meta-write metadata_write.txt
run_forward_fault trash-meta-close metadata_close.txt
run_forward_fault trash-meta-publish metadata_publish.txt

prepublish_source="$X/prepublish_recovery.txt"
prepublish_decoy="$X/prepublish_recovery_decoy.txt"
prepublish_marker="$T/prepublish-recovery.marker"
prepublish_fault_marker="$T/prepublish-fault.marker"
printf 'prepublish original\n' > "$prepublish_source"
printf 'prepublish racer\n' > "$prepublish_decoy"
prepublish_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-forward-write-reappear \
    JINV_TRASH_SOURCE="$prepublish_source" \
    JINV_TRASH_SOURCE_DECOY="$prepublish_decoy" \
    JINV_TRASH_SWAP_MARKER="$prepublish_marker" \
    JINV_TRASH_FAULT_MARKER="$prepublish_fault_marker" \
    "$JASH" -c "trash $prepublish_source" 2>&1)"
prepublish_rc=$?
prepublish_holds=("$X"/.jhold.*)
prepublish_hold=""
for hold in "${prepublish_holds[@]}"; do
    [[ -f "$hold" ]] || continue
    [[ "$(<"$hold")" == "prepublish original" ]] && prepublish_hold=$hold
done
if [[ "$prepublish_rc" -ne 0 && -f "$prepublish_marker" &&
      "$(<"$prepublish_source")" == "prepublish racer" &&
      -n "$prepublish_hold" && "$prepublish_out" == *"$prepublish_hold"* ]] &&
   ! compgen -G "$trash_dir/*_prepublish_recovery.txt" >/dev/null &&
   ! compgen -G "$trash_dir/*_prepublish_recovery.txt.meta" >/dev/null; then
    ok "EXDEV trash discloses stranded hold when copy fails before publication"
else
    no "EXDEV trash hid or misreported pre-publication recovery" \
       "rc=$prepublish_rc output=$prepublish_out"
fi

forward_swap_source="$X/forward_swap.txt"
forward_swap_decoy="$X/forward_swap_decoy.txt"
forward_swap_marker="$T/forward-hold-swap.marker"
printf 'forward trusted data\n' > "$forward_swap_source"
printf 'forward unrelated data\n' > "$forward_swap_decoy"
forward_swap_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-hold-swap \
    JINV_TRASH_SOURCE="$forward_swap_source" \
    JINV_TRASH_SOURCE_DECOY="$forward_swap_decoy" \
    JINV_TRASH_SWAP_MARKER="$forward_swap_marker" \
    "$JASH" -c "trash $forward_swap_source" 2>&1)"
forward_swap_rc=$?
if [[ "$forward_swap_rc" -ne 0 && -f "$forward_swap_marker" &&
      "$(<"$forward_swap_source")" == "forward unrelated data" &&
      "$(<"$forward_swap_decoy")" == "forward trusted data" ]] &&
   ! compgen -G "$trash_dir/*_forward_swap.txt" >/dev/null; then
    ok "forward EXDEV trash rolls back source identity change"
else
    no "forward EXDEV trash rolls back source identity change" \
       "rc=$forward_swap_rc output=$forward_swap_out"
fi

forward_final_source="$X/forward_final.txt"
forward_final_decoy="$X/forward_final_decoy.txt"
forward_final_marker="$T/forward-final.marker"
printf 'forward final trusted\n' > "$forward_final_source"
printf 'forward final unrelated\n' > "$forward_final_decoy"
forward_final_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-final-unlink-swap \
    JINV_TRASH_SOURCE_DECOY="$forward_final_decoy" \
    JINV_TRASH_SWAP_MARKER="$forward_final_marker" \
    "$JASH" -c "trash $forward_final_source" 2>&1)"
forward_final_rc=$?
if [[ "$forward_final_rc" -ne 0 && -f "$forward_final_marker" &&
      "$(<"$forward_final_source")" == "forward final unrelated" &&
      "$(<"$forward_final_decoy")" == "forward final trusted" ]] &&
   ! compgen -G "$trash_dir/*_forward_final.txt" >/dev/null; then
    ok "forward EXDEV trash rolls back a final held-inode swap"
else
    no "forward EXDEV trash rolls back a final held-inode swap" \
       "rc=$forward_final_rc output=$forward_final_out"
fi


reappear_source="$X/crossdevice_recovery.txt"
reappear_decoy="$X/crossdevice_recovery_decoy.txt"
reappear_marker="$T/forward-reappear.marker"
printf 'forward original recovery\n' > "$reappear_source"
printf 'forward raced source\n' > "$reappear_decoy"
reappear_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-source-reappear-before-delete \
    JINV_TRASH_SOURCE="$reappear_source" JINV_TRASH_SOURCE_DECOY="$reappear_decoy" \
    JINV_TRASH_SWAP_MARKER="$reappear_marker" \
    "$JASH" -c "trash $reappear_source" 2>&1)"
reappear_rc=$?
reappear_items=("$trash_dir"/*_crossdevice_recovery.txt)
reappear_hold_ok=0
reappear_hold=""
for hold in "$X"/.jhold.*; do
    [ -f "$hold" ] || continue
    if [ "$(<"$hold")" = "forward original recovery" ]; then
        reappear_hold_ok=1
        reappear_hold=$hold
    fi
done
if [[ "$reappear_rc" -ne 0 && -f "$reappear_marker" &&
      "$(<"$reappear_source")" == "forward raced source" &&
      -f "${reappear_items[0]}" &&
      "$(<"${reappear_items[0]}")" == "forward original recovery" &&
      -f "${reappear_items[0]}.meta" && "$reappear_hold_ok" -eq 1 &&
      "$reappear_out" == *"$reappear_hold"* &&
      "$reappear_out" == *"${reappear_items[0]}"* &&
      "$reappear_out" == *"${reappear_items[0]}.meta"* ]]; then
    ok "EXDEV trash keeps discoverable recovery when source reappears"
else
    no "EXDEV trash orphaned recovery when source reappeared" \
       "rc=$reappear_rc output=$reappear_out"
fi

dest_item="$trash_dir/107_exdev_dest.txt"
dest_path="$X/exdev_dest.txt"
dest_decoy="$X/dest-swap-decoy"
dest_marker="$T/trash-dest-swap.marker"
printf 'trusted destination copy\n' > "$dest_item"
printf '.\n' > "$dest_item.meta"
printf 'replacement destination\n' > "$dest_decoy"
dest_out="$(cd "$X" && env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dest-swap \
    JINV_TRASH_DEST="$dest_path" JINV_TRASH_DEST_DECOY="$dest_decoy" \
    JINV_TRASH_SWAP_MARKER="$dest_marker" \
    "$JASH" -c 'trash restore exdev_dest.txt' 2>&1)"
dest_rc=$?
if [[ "$dest_rc" -ne 0 && -f "$dest_marker" &&
      "$(<"$dest_item")" == "trusted destination copy" &&
      "$(<"$dest_path")" == "replacement destination" &&
      "$(<"$dest_decoy")" == "trusted destination copy" && -e "$dest_item.meta" &&
      "$dest_out" == *"$dest_item"* && "$dest_out" == *"$dest_item.meta"* ]]; then
    ok "EXDEV restore preserves trash source after destination identity change"
else
    no "EXDEV restore preserves trash source after destination identity change" \
       "rc=$dest_rc output=$dest_out"
fi

dir_item="$trash_dir/108_crossdir"
mkdir "$dir_item"
printf 'directory data\n' > "$dir_item/file.txt"
printf '.\n' > "$dir_item.meta"
dir_out="$(cd "$X" && RUN -c 'trash restore crossdir' 2>&1)"
dir_rc=$?
if [[ "$dir_rc" -ne 0 && "$dir_out" == *"cannot restore"* &&
      -f "$dir_item/file.txt" && ! -e "$X/crossdir" ]]; then
    ok "EXDEV directory restore fails nonzero without losing the trash tree"
else
    no "EXDEV directory restore fails nonzero without losing the trash tree" \
       "rc=$dir_rc output=$dir_out"
fi

dirsync_restore_item="$trash_dir/110_dirsync_restore.txt"
dirsync_restore_dest="$X/dirsync_restore.txt"
dirsync_restore_marker="$T/dirsync-restore.marker"
printf 'dirsync restore data\n' > "$dirsync_restore_item"
printf '.\n' > "$dirsync_restore_item.meta"
dirsync_restore_out="$(cd "$X" && env HOME="$HOME_DIR" PATH="/usr/bin:/bin" \
    JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
    JASH_NUDGE=0 LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dir-fsync \
    JINV_TRASH_FSYNC_DIR="$X" JINV_TRASH_FSYNC_N=1 \
    JINV_TRASH_FAULT_MARKER="$dirsync_restore_marker" \
    "$JASH" -c 'trash restore dirsync_restore.txt' 2>&1)"
dirsync_restore_rc=$?
if [[ "$dirsync_restore_rc" -ne 0 && -f "$dirsync_restore_marker" &&
      -f "$dirsync_restore_item" && -f "$dirsync_restore_item.meta" &&
      ! -e "$dirsync_restore_dest" &&
      "$dirsync_restore_out" == *"$dirsync_restore_item"* &&
      "$dirsync_restore_out" == *"$dirsync_restore_item.meta"* ]]; then
    ok "EXDEV restore rolls back destination-directory fsync failure"
else
    no "EXDEV restore rolls back destination-directory fsync failure" \
       "rc=$dirsync_restore_rc output=$dirsync_restore_out"
fi

samefs_restore_dir="$HOME_DIR/samefs-restore"
samefs_restore_item="$trash_dir/111_samefs_restore.txt"
samefs_restore_marker="$T/samefs-restore.marker"
mkdir -p "$samefs_restore_dir"
printf 'samefs restore data\n' > "$samefs_restore_item"
printf '%s\n' "$samefs_restore_dir" > "$samefs_restore_item.meta"
samefs_restore_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dir-fsync \
    JINV_TRASH_FSYNC_DIR="$samefs_restore_dir" JINV_TRASH_FSYNC_N=1 \
    JINV_TRASH_FAULT_MARKER="$samefs_restore_marker" \
    "$JASH" -c 'trash restore samefs_restore.txt' 2>&1)"
samefs_restore_rc=$?
if [[ "$samefs_restore_rc" -ne 0 && -f "$samefs_restore_marker" &&
      -f "$samefs_restore_item" && -f "$samefs_restore_item.meta" &&
      ! -e "$samefs_restore_dir/samefs_restore.txt" &&
      "$samefs_restore_out" == *"$samefs_restore_item"* &&
      "$samefs_restore_out" == *"$samefs_restore_item.meta"* ]]; then
    ok "same-filesystem restore rolls back destination-directory fsync failure"
else
    no "same-filesystem restore rolls back destination-directory fsync failure" \
       "rc=$samefs_restore_rc output=$samefs_restore_out"
fi

samefs_trashsync_item="$trash_dir/112_samefs_trashsync.txt"
samefs_trashsync_marker="$T/samefs-trashsync.marker"
printf 'samefs trash sync data\n' > "$samefs_trashsync_item"
printf '%s\n' "$samefs_restore_dir" > "$samefs_trashsync_item.meta"
samefs_trashsync_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dir-fsync \
    JINV_TRASH_FSYNC_DIR="$trash_dir" JINV_TRASH_FSYNC_N=1 \
    JINV_TRASH_FAULT_MARKER="$samefs_trashsync_marker" \
    "$JASH" -c 'trash restore samefs_trashsync.txt' 2>&1)"
samefs_trashsync_rc=$?
if [[ "$samefs_trashsync_rc" -ne 0 && -f "$samefs_trashsync_marker" &&
      -f "$samefs_trashsync_item" && -f "$samefs_trashsync_item.meta" &&
      ! -e "$samefs_restore_dir/samefs_trashsync.txt" &&
      "$samefs_trashsync_out" == *"$samefs_trashsync_item"* &&
      "$samefs_trashsync_out" == *"$samefs_trashsync_item.meta"* ]]; then
    ok "same-filesystem restore rolls back trash-directory fsync failure"
else
    no "same-filesystem restore rolls back trash-directory fsync failure" \
       "rc=$samefs_trashsync_rc output=$samefs_trashsync_out"
fi

samefs_forward_dir="$HOME_DIR/samefs-forward"
samefs_forward_source="$samefs_forward_dir/reappear.txt"
samefs_forward_decoy="$samefs_forward_dir/reappear-decoy.txt"
samefs_forward_marker="$T/samefs-forward-reappear.marker"
mkdir -p "$samefs_forward_dir"
printf 'samefs forward data\n' > "$samefs_forward_source"
printf 'path replacement\n' > "$samefs_forward_decoy"
samefs_forward_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-forward-reappear \
    JINV_TRASH_FSYNC_DIR="$trash_dir" JINV_TRASH_SOURCE="$samefs_forward_source" \
    JINV_TRASH_SOURCE_DECOY="$samefs_forward_decoy" \
    JINV_TRASH_FAULT_MARKER="$samefs_forward_marker" \
    "$JASH" -c "trash $samefs_forward_source" 2>&1)"
samefs_forward_rc=$?
samefs_forward_items=("$trash_dir"/*_reappear.txt)
if [[ "$samefs_forward_rc" -ne 0 && -f "$samefs_forward_marker" &&
      "$(<"$samefs_forward_source")" == "path replacement" &&
      ${#samefs_forward_items[@]} -eq 1 && -f "${samefs_forward_items[0]}.meta" &&
      "$(<"${samefs_forward_items[0]}")" == "samefs forward data" &&
      "$samefs_forward_out" == *"${samefs_forward_items[0]}"* &&
      "$samefs_forward_out" == *"${samefs_forward_items[0]}.meta"* ]]; then
    ok "failed same-filesystem forward rollback retains discoverable recovery"
else
    no "failed same-filesystem forward rollback retains discoverable recovery" \
       "rc=$samefs_forward_rc output=$samefs_forward_out"
fi

dirsync_forward_source="$X/dirsync_forward.txt"
dirsync_forward_marker="$T/dirsync-forward.marker"
printf 'dirsync forward data\n' > "$dirsync_forward_source"
dirsync_forward_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dir-fsync \
    JINV_TRASH_FSYNC_DIR="$X" JINV_TRASH_FSYNC_N=1 \
    JINV_TRASH_FAULT_MARKER="$dirsync_forward_marker" \
    "$JASH" -c "trash $dirsync_forward_source" 2>&1)"
dirsync_forward_rc=$?
if [[ "$dirsync_forward_rc" -ne 0 && -f "$dirsync_forward_marker" &&
      -f "$dirsync_forward_source" ]] &&
   ! compgen -G "$trash_dir/*_dirsync_forward.txt" >/dev/null; then
    ok "forward EXDEV trash rolls back source-directory fsync failure"
else
    no "forward EXDEV trash rolls back source-directory fsync failure" \
       "rc=$dirsync_forward_rc output=$dirsync_forward_out"
fi

dirsync_final_source="$X/dirsync_final.txt"
dirsync_final_marker="$T/dirsync-final.marker"
printf 'dirsync retained copy\n' > "$dirsync_final_source"
dirsync_final_out="$(env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=trash-dir-fsync \
    JINV_TRASH_FSYNC_DIR="$X" JINV_TRASH_FSYNC_N=2 \
    JINV_TRASH_FAULT_MARKER="$dirsync_final_marker" \
    "$JASH" -c "trash $dirsync_final_source" 2>&1)"
dirsync_final_rc=$?
dirsync_final_items=("$trash_dir"/*_dirsync_final.txt)
if [[ "$dirsync_final_rc" -ne 0 && -f "$dirsync_final_marker" &&
      ! -e "$dirsync_final_source" && ${#dirsync_final_items[@]} -eq 1 &&
      -f "${dirsync_final_items[0]}" && -f "${dirsync_final_items[0]}.meta" &&
      "$dirsync_final_out" == *"${dirsync_final_items[0]}"* &&
      "$dirsync_final_out" == *"${dirsync_final_items[0]}.meta"* ]]; then
    ok "forward EXDEV trash retains published recovery after final dir fsync failure"
else
    no "forward EXDEV trash retains published recovery after final dir fsync failure" \
       "rc=$dirsync_final_rc output=$dirsync_final_out"
fi

spec_work="$T/spec-work"
spec_victim="$T/spec-victim"
spec_swap_marker="$T/spec-swap.marker"
spec_fdopendir_marker="$T/spec-fdopendir.marker"
mkdir -p "$spec_work" "$spec_victim"
printf 'must survive\n' > "$spec_victim/sentinel"
spec_swap_out="$(cd "$spec_work" && env HOME="$HOME_DIR" PATH="/usr/bin:/bin" JASH_SIGIL=0 \
    JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    LD_PRELOAD="$T/faults.so" JINV_FAIL=spec-swap JINV_SPEC_VICTIM="$spec_victim" \
    JINV_SPEC_SWAP_MARKER="$spec_swap_marker" \
    JINV_SPEC_FDOPENDIR_MARKER="$spec_fdopendir_marker" \
    "$JASH" -c 'speculate mkdir flip' 2>&1)"
spec_swap_rc=$?
spec_swap_path="$(sed -n '1p' "$spec_swap_marker" 2>/dev/null || true)"
if [[ "$spec_swap_rc" -ne 0 && -n "$spec_swap_path" &&
      -f "$spec_fdopendir_marker" && -L "$spec_swap_path" &&
      -f "$spec_victim/sentinel" &&
      "$spec_swap_out" == *"${spec_swap_path%/flip}"* ]] &&
   [[ "$(<"$spec_victim/sentinel")" == "must survive" ]]; then
    ok "speculate cleanup pins the directory across a proven symlink swap"
else
    no "speculate cleanup pins the directory across a proven symlink swap" \
       "rc=$spec_swap_rc path=$spec_swap_path fdopendir=$([[ -f $spec_fdopendir_marker ]] && echo yes || echo no) output=$spec_swap_out"
fi

spec_preopen_work="$T/spec-preopen-work"
spec_preopen_victim="$T/spec-preopen-victim"
spec_preopen_marker="$T/spec-preopen.marker"
mkdir -p "$spec_preopen_work" "$spec_preopen_victim/nested"
printf 'replacement must survive\n' > "$spec_preopen_victim/nested/sentinel"
spec_preopen_out="$(cd "$spec_preopen_work" && env HOME="$HOME_DIR" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 LD_PRELOAD="$T/faults.so" \
    JINV_FAIL=spec-preopen-replace JINV_SPEC_VICTIM="$spec_preopen_victim" \
    JINV_SPEC_PREOPEN_MARKER="$spec_preopen_marker" \
    "$JASH" -c 'speculate mkdir flip' 2>&1)"
spec_preopen_rc=$?
spec_preopen_path="$(sed -n '1p' "$spec_preopen_marker" 2>/dev/null || true)"
if [[ "$spec_preopen_rc" -ne 0 && -n "$spec_preopen_path" &&
      -f "$spec_preopen_path/nested/sentinel" &&
      "$spec_preopen_out" == *"${spec_preopen_path%/flip}"* ]] &&
   [[ "$(<"$spec_preopen_path/nested/sentinel")" == "replacement must survive" ]]; then
    ok "speculate refuses a directory replaced before openat traversal"
else
    no "speculate refuses a directory replaced before openat traversal" \
       "rc=$spec_preopen_rc path=$spec_preopen_path output=$spec_preopen_out"
fi

for spec_kind in regular symlink; do
    spec_replace_work="$T/spec-$spec_kind-work"
    spec_replace_victim="$T/spec-$spec_kind-victim"
    spec_replace_marker="$T/spec-$spec_kind.marker"
    mkdir -p "$spec_replace_work"
    if [[ "$spec_kind" == regular ]]; then
        printf 'replacement must survive\n' > "$spec_replace_victim"
    else
        ln -s "$spec_victim/sentinel" "$spec_replace_victim"
    fi
    spec_replace_out="$(cd "$spec_replace_work" && env HOME="$HOME_DIR" \
        PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
        JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 LD_PRELOAD="$T/faults.so" \
        JINV_FAIL=spec-preunlink-replace JINV_SPEC_VICTIM="$spec_replace_victim" \
        JINV_SPEC_PREUNLINK_MARKER="$spec_replace_marker" \
        "$JASH" -c 'speculate touch flip' 2>&1)"
    spec_replace_rc=$?
    spec_replace_path="$(sed -n '1p' "$spec_replace_marker" 2>/dev/null || true)"
    if [[ "$spec_replace_rc" -ne 0 && -n "$spec_replace_path" ]] &&
       { [[ "$spec_kind" == regular && -f "$spec_replace_path" &&
            "$(<"$spec_replace_path")" == "replacement must survive" ]] ||
         [[ "$spec_kind" == symlink && -L "$spec_replace_path" ]]; } &&
       [[ "$spec_replace_out" == *"${spec_replace_path%/flip}"* ]]; then
        ok "speculate cleanup preserves a replacement $spec_kind inode"
    else
        no "speculate cleanup preserves a replacement $spec_kind inode" \
           "rc=$spec_replace_rc path=$spec_replace_path output=$spec_replace_out"
    fi
done

cat >"$T/spec_noreplace_unsupported.c" <<'EOF_C'
#define JASH_SPEC_NOREPLACE_UNSUPPORTED 1
#include "builtins/builtin_speculate.c"

int main(void) {
    char dir[] = "/tmp/jash-spec-noreplace.XXXXXX";
    if (!mkdtemp(dir)) return 10;
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) return 11;
    int fd = openat(dirfd, "owned", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || close(fd) != 0) return 12;
    errno = 0;
    int rc = spec_rename_noreplace(dirfd, "owned", "capture");
    int saved_errno = errno;
    struct stat owned, capture;
    int owned_ok = fstatat(dirfd, "owned", &owned, AT_SYMLINK_NOFOLLOW) == 0;
    int capture_missing = fstatat(dirfd, "capture", &capture, AT_SYMLINK_NOFOLLOW) != 0 &&
                          errno == ENOENT;
    unlinkat(dirfd, "owned", 0);
    close(dirfd);
    rmdir(dir);
    return rc == -1 && saved_errno == ENOTSUP && owned_ok && capture_missing ? 0 : 1;
}
EOF_C
if cc -std=gnu11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
      -ffunction-sections -fdata-sections -Wl,--gc-sections \
      -I"$PROJECT_ROOT/c" -I"$PROJECT_ROOT/c/sys" -I"$PROJECT_ROOT/c/osif" \
      -I"$PROJECT_ROOT/c/liquid" -I"$PROJECT_ROOT/libjef" \
      "$T/spec_noreplace_unsupported.c" \
      -o "$T/spec_noreplace_unsupported" >/dev/null 2>&1 &&
   "$T/spec_noreplace_unsupported"; then
    ok "speculate fails closed when no atomic no-replace primitive exists"
else
    no "speculate fails closed when no atomic no-replace primitive exists"
fi

spec_final_work="$T/spec-final-dir-work"
spec_final_victim="$T/spec-final-dir-victim"
spec_final_marker="$T/spec-final-dir.marker"
mkdir -p "$spec_final_work" "$spec_final_victim"
printf 'final replacement must survive\n' > "$spec_final_victim/sentinel"
spec_final_out="$(cd "$spec_final_work" && env HOME="$HOME_DIR" \
    PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 JASH_MEMO=0 \
    JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 LD_PRELOAD="$T/faults.so" \
    JINV_FAIL=spec-final-dir-replace JINV_SPEC_VICTIM="$spec_final_victim" \
    JINV_SPEC_FINAL_MARKER="$spec_final_marker" \
    "$JASH" -c 'speculate mkdir flip' 2>&1)"
spec_final_rc=$?
spec_final_path="$(sed -n '1p' "$spec_final_marker" 2>/dev/null || true)"
if [[ "$spec_final_rc" -ne 0 && -n "$spec_final_path" &&
      -f "$spec_final_path/sentinel" &&
      "$(<"$spec_final_path/sentinel")" == "final replacement must survive" &&
      "$spec_final_out" == *"$spec_final_path"* ]]; then
    ok "speculate cleanup preserves a replacement directory inode"
else
    no "speculate cleanup preserves a replacement directory inode" \
       "rc=$spec_final_rc path=$spec_final_path output=$spec_final_out"
fi

spec_json_work="$T/spec-json-work"
mkdir -p "$spec_json_work"
spec_json_out="$(cd "$spec_json_work" && RUN -c 'speculate --json true' 2>/dev/null)"
if python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["cleanup"] == "removed" and d["sandbox"] is None' \
       "$spec_json_out" >/dev/null 2>&1; then
    ok "speculate JSON reports removed only after successful cleanup"
else
    no "speculate JSON claimed cleanup state before completion" \
       "output=$spec_json_out"
fi

echo "security mutator regressions: PASS=$pass FAIL=$fail"
[[ "$fail" -eq 0 ]]
