/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   ╔═╗╔═╗╦═╗╔╦╗   ╔═╗╔═╗╦═╗╔═╗╦╔═╗╦╔═╗╦═╗
 *   ╚═╗╠═╣║ ╦║║║   ╠═╣║ ║╠╦╝║ ╦║║ ╦║║║║╠╦╝
 *   ╚═╝╩ ╩╚═╝╩ ╩   ╩ ╩╚═╝╩╚═╚═╝╩╚═╝╩╚╩╝╩╚═
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  @file        sys/syscalls.h
 *  @brief       BearJash syscall compatibility layer
 *  @module      sys
 *
 *  SPDX-License-Identifier: LicenseRef-SPL-1.0
 *  Sovereign Public License 1.0.  No warranty.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  Maps jash's sys_* functions to Bear libc equivalents.
 *  On macOS, Bear libc uses libSystem.dylib under the hood.
 *  On Linux, Bear libc uses raw syscalls via osif/linux/.
 *
 *═══════════════════════════════════════════════════════════════════════════════*/

#ifndef BEARJASH_SYSCALLS_H
#define BEARJASH_SYSCALLS_H

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

/* ── Temporary directory helper ───────────────────────────────────────────── */

/* J1 fix: use TMPDIR env var, fallback to compile-time JASH_TMPDIR or "/tmp" */
#ifndef JASH_TMPDIR
#define JASH_TMPDIR "/tmp"
#endif

static inline const char* jash_tmpdir(void) {
    const char* tmp = getenv("TMPDIR");
    return (tmp && *tmp) ? tmp : JASH_TMPDIR;
}

/* ── Safe string functions (J2 fix: replace strcpy/strcat) ─────────────── */

/* Darwin libc provides strlcpy/strlcat natively (and wraps them in
 * FORTIFY macros under _DARWIN_C_SOURCE, which would mangle a local
 * redefinition). glibc 2.38+ also declares them natively. Only define
 * our own on platforms that lack them. */
#if !defined(HAVE_STRLCPY)
static inline size_t jash_strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size > 0) {
        size_t n = (len >= size) ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#define strlcpy jash_strlcpy
#endif

#if !defined(HAVE_STRLCAT)
static inline size_t jash_strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen < size - 1) {
        size_t n = (dlen + slen >= size) ? size - dlen - 1 : slen;
        memcpy(dst + dlen, src, n);
        dst[dlen + n] = '\0';
    }
    return dlen + slen;
}
#define strlcat jash_strlcat
#endif

#include <sys/stat.h>
#include <sys/wait.h>

/* ── Core syscalls ─────────────────────────────────────────────────────────── */

static inline long sys_read(int fd, void *buf, unsigned long count) {
    return (long)read(fd, buf, count);
}

static inline long sys_write(int fd, const void *buf, unsigned long count) {
    return (long)write(fd, buf, count);
}

/* Helper to silence gcc warn_unused_result on write() */
#define WRITE_IGNORE(fd, buf, len) do { \
    ssize_t _wr = write((fd), (buf), (len)); \
    (void)_wr; \
} while(0)

static inline long sys_open(const char *path, int flags, int mode) {
    return (long)open(path, flags, mode);
}

static inline long sys_close(int fd) {
    return (long)close(fd);
}

static inline long sys_fork(void) {
    return (long)fork();
}

static inline long sys_vfork(void) {
#ifdef JASH_BLIBC
    /* blibc deliberately has no vfork; the child execs or exits. */
    return (long)fork();
#else
    return (long)vfork();
#endif
}

static inline long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return (long)execve(path, argv, envp);
}

static inline long sys_waitpid(long pid, int *status, int opts) {
    return (long)waitpid((pid_t)pid, status, opts);
}

static inline long sys_dup2(int oldfd, int newfd) {
    return (long)dup2(oldfd, newfd);
}

static inline long sys_pipe(int fds[2]) {
    return (long)pipe(fds);
}

static inline long sys_getcwd(char *buf, unsigned long size) {
    return (long)getcwd(buf, size);
}

static inline long sys_chdir(const char *path) {
    return (long)chdir(path);
}

static inline long sys_getpid(void) {
    return (long)getpid();
}

static inline long sys_kill(long pid, int sig) {
    return (long)kill((pid_t)pid, sig);
}

static inline long sys_exit(int code) {
    _exit(code);
    __builtin_unreachable();
}

/* ── Directory enumeration ───────────────────────────────────────────────────
 *
 *  jash uses sys_getdents64 for tab completion.
 *  On Bear libc, we use opendir/readdir/closedir.
 */

#include <dirent.h>

/* Minimal linux_dirent64 struct for compatibility */
struct linux_dirent64 {
    uint64_t        d_ino;
    int64_t         d_off;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[];
};

static inline long sys_getdents64(int fd, void *dirp, unsigned long count) {
#ifdef __linux__
    return syscall(SYS_getdents64, fd, dirp, count);
#else
    /* macOS / other: not yet implemented */
    (void)fd; (void)dirp; (void)count;
    return -38; /* -ENOSYS */
#endif
}

/* ── Sleep ─────────────────────────────────────────────────────────────────── */

#include <time.h>

static inline long sys_sleep_ms(long ms) {
#ifdef JASH_BETHOS
    struct bear_timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000
    };
    return (long)bear_nanosleep(&ts, NULL);
#else
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000
    };
    return (long)nanosleep(&ts, NULL);
#endif
}

static inline long sys_unlink(const char *path) {
    return (long)unlink(path);
}

/* ── Josh-specific stubs (not available on POSIX) ─────────────────────────── */

static inline long sys_uptime(void) { return 0; }
static inline long sys_stdin_pending(void) { return 0; }

/* ps and meminfo structs for compatibility */
typedef struct {
    long pid;
    char state;
    long ppid;
    char name[64];
} jash_ps_entry_t;

typedef struct {
    unsigned long total;
    unsigned long free;
    unsigned long used;
} jash_meminfo_t;

static inline long sys_ps(jash_ps_entry_t *buf, long max_entries) {
    (void)buf; (void)max_entries;
    return -38; /* -ENOSYS */
}

static inline long sys_meminfo(jash_meminfo_t *mi) {
    (void)mi;
    return -38; /* -ENOSYS */
}

#endif /* BEARJASH_SYSCALLS_H */
