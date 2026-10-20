/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif/beyondbsd/syscall.c — BeyondBSD/NetBSD backend via blibc bear_*
 *
 *   In the BeyondBSD freestanding environment, there is no libc.
 *   The blibc exposes system calls as bear_* functions.
 *   This backend translates Linux x86_64 syscall numbers to blibc calls.
 *
 *   Build with: -ffreestanding -nostdinc -fno-builtin
 *               -include jash_netbsd_compat.h
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/

#include "../osif.h"

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

/* ── blibc internals (bear_* surface) ────────────────────────────────────── */

extern int  bear_setpgid(int pid, int pgid);
extern int  bear_getpgrp(void);
extern int  bear_open(const char *path, int flags, ...);
extern long bear_getpid(void);
extern long bear_getppid(void);
extern int  bear_close(int fd);
extern long bear_read(int fd, void *buf, long count);
extern long bear_write(int fd, const void *buf, long count);
extern int  bear_kill(int pid, int sig);
extern int  bear_execve(const char *path, char *const argv[], char *const envp[]);
extern int  bear_fork(void);
extern int  bear_wait4(int pid, int *status, int options, void *rusage);
extern void bear_exit(int code) __attribute__((noreturn));
extern int  bear_sigaction(int sig, const void *act, void *oact);
extern int  bear_sigprocmask(int how, const void *set, void *oldset);
extern int  bear_getuid(void);
extern int  bear_getgid(void);
extern int  bear_geteuid(void);
extern int  bear_getegid(void);
extern int  bear_setuid(int uid);
extern int  bear_setgid(int gid);
extern int  bear_getgroups(int n, int *list);
extern int  bear_setgroups(int n, const int *list);
extern int  bear_chdir(const char *path);
extern int  bear_fchdir(int fd);
extern int  bear_dup(int fd);
extern int  bear_dup2(int old, int new);
extern int  bear_pipe(int fds[2]);
extern long bear_ioctl(int fd, unsigned long req, ...);
extern int  bear_fcntl(int fd, int cmd, ...);
extern int  bear_mkdir(const char *path, int mode);
extern int  bear_rmdir(const char *path);
extern int  bear_unlink(const char *path);
extern int  bear_rename(const char *old, const char *new);
extern int  bear_link(const char *old, const char *new);
extern int  bear_symlink(const char *old, const char *new);
extern long bear_readlink(const char *path, char *buf, long bufsz);
extern int  bear_chmod(const char *path, int mode);
extern int  bear_fchmod(int fd, int mode);
extern int  bear_chown(const char *path, int uid, int gid);
extern int  bear_fchown(int fd, int uid, int gid);
extern int  bear_mknod(const char *path, int mode, int dev);
extern int  bear_stat(const char *path, void *buf);
extern int  bear_fstat(int fd, void *buf);
extern int  bear_lstat(const char *path, void *buf);
extern void *bear_mmap(void *addr, long len, int prot, int flags, int fd, long off);
extern int  bear_munmap(void *addr, long len);
extern int  bear_mprotect(void *addr, long len, int prot);
extern int  bear_getrlimit(int resource, void *rlim);
extern int  bear_socket(int domain, int type, int protocol);
extern int  bear_connect(int fd, const void *addr, int addrlen);
extern int  bear_accept(int fd, void *addr, int *addrlen);
extern long bear_sendto(int fd, const void *buf, long len, int flags,
                        const void *addr, int addrlen);
extern long bear_recvfrom(int fd, void *buf, long len, int flags,
                          void *addr, int *addrlen);
extern int  bear_bind(int fd, const void *addr, int addrlen);
extern int  bear_listen(int fd, int backlog);
extern int  bear_shutdown(int fd, int how);
extern int  bear_getsockname(int fd, void *addr, int *addrlen);
extern int  bear_getpeername(int fd, void *addr, int *addrlen);
extern int  bear_setsockopt(int fd, int level, int opt, const void *val, int len);
extern int  bear_getsockopt(int fd, int level, int opt, void *val, int *len);
extern int  bear_socketpair(int d, int t, int p, int sv[2]);
extern int  bear_select(int nfds, void *rfds, void *wfds, void *efds, void *tv);
extern int  bear_setsid(void);
extern int  bear_getcwd(char *buf, long size);
extern long bear_gettid(void);

/* ── Translation ─────────────────────────────────────────────────────────── */

static long
beyondbsd_syscall(long nr, long a1, long a2, long a3,
                  long a4, long a5, long a6)
{
    switch (nr) {

    /* File I/O */
    case 0:  return bear_read((int)a1, (void *)a2, a3);
    case 1:  return bear_write((int)a1, (const void *)a2, a3);
    case 2:  return bear_open((const char *)a1, (int)a2, a3);
    case 3:  return bear_close((int)a1);
    case 4:  return bear_stat((const char *)a1, (void *)a2);
    case 5:  return bear_fstat((int)a1, (void *)a2);
    case 6:  return bear_lstat((const char *)a1, (void *)a2);

    /* Memory */
    case 9:  return (long)bear_mmap((void *)a1, a2, (int)a3, (int)a4, (int)a5, a6);
    case 10: return bear_mprotect((void *)a1, a2, (int)a3);
    case 11: return bear_munmap((void *)a1, a2);

    /* Process */
    case 39: return bear_getpid();
    case 57: return bear_fork();
    case 59: return bear_execve((const char *)a1, (char *const *)a2, (char *const *)a3);
    case 60: bear_exit((int)a1); __builtin_unreachable();
    case 61: return bear_wait4((int)a1, (int *)a2, (int)a3, (void *)a4);
    case 62: return bear_kill((int)a1, (int)a2);
    case 110: return bear_getppid();
    case 186: return bear_gettid();

    /* Signals */
    case 13: return bear_sigaction((int)a1, (const void *)a2, (void *)a3);
    case 14: return bear_sigprocmask((int)a1, (const void *)a2, (void *)a3);
    case 132: return -BEAR_OSIF_ENOSYS; /* sigaltstack TBD in blibc */

    /* User/Group */
    case 102: return bear_getuid();
    case 104: return bear_getgid();
    case 107: return bear_geteuid();
    case 108: return bear_getegid();
    case 105: return bear_setuid((int)a1);
    case 106: return bear_setgid((int)a1);
    case 109: return bear_setpgid((int)a1, (int)a2);
    case 112: return bear_setsid();
    case 115: return bear_getgroups((int)a1, (int *)a2);
    case 116: return bear_setgroups((int)a1, (const int *)a2);

    /* File ops */
    case 79: return bear_getcwd((char *)a1, a2);
    case 80: return bear_chdir((const char *)a1);
    case 81: return bear_fchdir((int)a1);
    case 82: return bear_rename((const char *)a1, (const char *)a2);
    case 83: return bear_mkdir((const char *)a1, (int)a2);
    case 84: return bear_rmdir((const char *)a1);
    case 86: return bear_link((const char *)a1, (const char *)a2);
    case 87: return bear_unlink((const char *)a1);
    case 88: return bear_symlink((const char *)a1, (const char *)a2);
    case 89: return bear_readlink((const char *)a1, (char *)a2, a3);
    case 90: return bear_chmod((const char *)a1, (int)a2);
    case 92: return bear_chown((const char *)a1, (int)a2, (int)a3);
    case 133: return bear_mknod((const char *)a1, (int)a2, (int)a3);
    case 207: return bear_fchown((int)a1, (int)a2, (int)a3);
    case 209: return bear_fchmod((int)a1, (int)a2);

    /* Misc */
    case 32: return bear_dup((int)a1);
    case 33: return bear_dup2((int)a1, (int)a2);
    case 22: return bear_pipe((int *)a1);
    case 72: return bear_fcntl((int)a1, (int)a2, a3);
    case 16: return bear_ioctl((int)a1, (unsigned long)a2, a3, a4, a5, a6);
    case 77: return -BEAR_OSIF_ENOSYS; /* ftruncate via bear_* TBD */
    case 183: return bear_getrlimit((int)a1, (void *)a2);

    /* Network */
    case 41: return bear_socket((int)a1, (int)a2, (int)a3);
    case 42: return bear_connect((int)a1, (const void *)a2, (int)a3);
    case 43: return bear_accept((int)a1, (void *)a2, (int *)a3);
    case 44: return bear_sendto((int)a1, (const void *)a2, a3, (int)a4,
                               (const void *)a5, (int)a6);
    case 45: return bear_recvfrom((int)a1, (void *)a2, a3, (int)a4,
                                  (void *)a5, (int *)a6);
    case 48: return bear_shutdown((int)a1, (int)a2);
    case 49: return bear_bind((int)a1, (const void *)a2, (int)a3);
    case 50: return bear_listen((int)a1, (int)a2);
    case 201: return bear_socketpair((int)a1, (int)a2, (int)a3, (int *)a4);
    case 203: return bear_getsockname((int)a1, (void *)a2, (int *)a3);
    case 204: return bear_getpeername((int)a1, (void *)a2, (int *)a3);
    case 205: return bear_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (int)a5);
    case 206: return bear_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (int *)a5);

    /* Time */
    case 35: return bear_nanosleep((const void *)a1, (void *)a2);

    /* Exit */
    case 231: bear_exit((int)a1); __builtin_unreachable();

    /* select */
    case 23: return bear_select((int)a1, (void *)a2, (void *)a3, (void *)a4, (void *)a5);

    /* Unsupported / not yet in blibc */
    default: return -BEAR_OSIF_ENOSYS;
    }
}

long bear_osif_syscall6(long nr, long a1, long a2, long a3,
                         long a4, long a5, long a6)
{
    return beyondbsd_syscall(nr, a1, a2, a3, a4, a5, a6);
}

void bear_osif_exit(int code)
{
    bear_exit(code);
    __builtin_unreachable();
}
