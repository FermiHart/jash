/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif/netbsd/syscall.c — NetBSD L0 backend via libc
 *
 *   NetBSD DOES allow raw syscalls, but jash targets the stable libc ABI
 *   (like the macos backend does with libSystem): every Linux x86_64 NR
 *   in osif.h is translated to the matching NetBSD libc call. No raw
 *   trap, no versioned __stat50/__wait450 knowledge leaks above L0.
 *
 *   Copied from c/osif/macos/syscall.c (BSD libc shape) and adapted:
 *   - uname() is real (NetBSD has <sys/utsname.h>)
 *   - clock_gettime/clock_getres/nanosleep/sched_yield wired (macos stubbed)
 *   - getdents64 stays ENOSYS: upper layers use opendir/readdir
 *   - arch_prctl/futex/epoll/inotify/signalfd/timerfd/eventfd: Linux-only,
 *     ENOSYS by design
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/

#include "../osif.h"

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

/* ── NetBSD libc (always linked; no -nostdlib here) ─────────────────────── */

/* Process */
extern pid_t  fork(void);
extern int    execve(const char *, char *const [], char *const []);
extern void   _exit(int) __attribute__((noreturn));
extern pid_t  wait4(pid_t, int *, int, struct rusage *);
extern pid_t  getpid(void);
extern pid_t  getppid(void);
extern int    kill(pid_t, int);

/* File I/O */
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern int     open(const char *, int, ...);
extern int     close(int);
extern off_t   lseek(int, off_t, int);
extern int     ftruncate(int, off_t);
extern int     ioctl(int, unsigned long, ...);
extern int     fcntl(int, int, ...);
extern int     select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
extern int     access(const char *, int);
extern int     openat(int, const char *, int, ...);

/* File system */
extern int     link(const char *, const char *);
extern int     unlink(const char *);
extern int     rename(const char *, const char *);
extern int     mkdir(const char *, mode_t);
extern int     rmdir(const char *);
extern int     symlink(const char *, const char *);
extern ssize_t readlink(const char *, char *, size_t);
extern int     chmod(const char *, mode_t);
extern int     chown(const char *, uid_t, gid_t);
extern int     mknod(const char *, mode_t, dev_t);
extern int     chdir(const char *);
extern int     fchdir(int);

/* Stat */
extern int     stat(const char *, struct stat *);
extern int     fstat(int, struct stat *);
extern int     lstat(const char *, struct stat *);

/* Directory */
extern DIR    *opendir(const char *);
extern struct dirent *readdir(DIR *);
extern int     closedir(DIR *);

/* User/Group */
extern uid_t   getuid(void);
extern uid_t   geteuid(void);
extern gid_t   getgid(void);
extern gid_t   getegid(void);
extern int     setuid(uid_t);
extern int     setgid(uid_t);
extern int     getgroups(int, gid_t []);
extern int     setgroups(int, const gid_t *);

/* Signals */
extern int     sigaction(int, const struct sigaction *, struct sigaction *);
extern int     sigprocmask(int, const sigset_t *, sigset_t *);
extern int     sigaltstack(const stack_t *, stack_t *);

/* Memory */
extern void   *mmap(void *, size_t, int, int, int, off_t);
extern int     munmap(void *, size_t);
extern int     mprotect(void *, size_t, int);

/* Time */
extern int     nanosleep(const struct timespec *, struct timespec *);
extern int     clock_gettime(clockid_t, struct timespec *);
extern int     clock_getres(clockid_t, struct timespec *);

/* Network */
extern int     socket(int, int, int);
extern int     connect(int, const struct sockaddr *, socklen_t);
extern int     accept(int, struct sockaddr *, socklen_t *);
extern ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
extern ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
extern ssize_t sendmsg(int, const struct msghdr *, int);
extern ssize_t recvmsg(int, struct msghdr *, int);
extern int     shutdown(int, int);
extern int     bind(int, const struct sockaddr *, socklen_t);
extern int     listen(int, int);
extern int     getsockname(int, struct sockaddr *, socklen_t *);
extern int     getpeername(int, struct sockaddr *, socklen_t *);
extern int     socketpair(int, int, int, int *);
extern int     setsockopt(int, int, int, const void *, socklen_t);
extern int     getsockopt(int, int, int, void *, socklen_t *);

/* Misc */
extern int     getrlimit(int, struct rlimit *);
extern int     setrlimit(int, const struct rlimit *);
extern int     dup(int);
extern int     dup2(int, int);
extern int     pipe(int [2]);
extern int     uname(struct utsname *);

/* ── Translation: Linux NR → NetBSD libc ────────────────────────────────── */

static long
netbsd_syscall(long nr, long a1, long a2, long a3,
               long a4, long a5, long a6)
{
    long ret;
    switch (nr) {

    /* File I/O */
    case 0:  ret = (long)read((int)a1, (void *)a2, (size_t)a3); break;
    case 1:  ret = (long)write((int)a1, (const void *)a2, (size_t)a3); break;
    case 2:  ret = (long)open((const char *)a1, (int)a2, (mode_t)a3); break;
    case 3:  ret = (long)close((int)a1); break;
    case 4:  ret = (long)stat((const char *)a1, (struct stat *)a2); break;
    case 5:  ret = (long)fstat((int)a1, (struct stat *)a2); break;
    case 6:  ret = (long)lstat((const char *)a1, (struct stat *)a2); break;
    case 8:  ret = (long)lseek((int)a1, (off_t)a2, (int)a3); break;
    case 9:  ret = (long)mmap((void *)a1, (size_t)a2, (int)a3,
                              (int)a4, (int)a5, (off_t)a6); break;
    case 10: ret = (long)mprotect((void *)a1, (size_t)a2, (int)a3); break;
    case 11: ret = (long)munmap((void *)a1, (size_t)a2); break;
    case 16: ret = (long)ioctl((int)a1, (unsigned long)a2, (void *)a3); break;
    case 22: ret = (long)pipe((int *)a1); break;
    case 23: ret = (long)select((int)a1, (fd_set *)a2, (fd_set *)a3,
                                (fd_set *)a4, (struct timeval *)a5); break;
    case 32: ret = (long)dup((int)a1); break;
    case 33: ret = (long)dup2((int)a1, (int)a2); break;
    case 57: ret = (long)fork(); break;
    case 58: ret = (long)fork(); break; /* vfork = fork on NetBSD backend */
    case 59: ret = (long)execve((const char *)a1, (char *const *)a2,
                                (char *const *)a3); break;
    case 60: _exit((int)a1); __builtin_unreachable();
    case 61: ret = (long)wait4((pid_t)a1, (int *)a2, (int)a3,
                               (struct rusage *)a4); break;
    case 62: ret = (long)kill((pid_t)a1, (int)a2); break;
    case 72: ret = (long)fcntl((int)a1, (int)a2, (void *)a3); break;
    case 77: ret = (long)ftruncate((int)a1, (off_t)a2); break;
    case 79: ret = (long)getcwd((char *)a1, (size_t)a2); break;
    case 80: ret = (long)chdir((const char *)a1); break;
    case 81: ret = (long)fchdir((int)a1); break;
    case 82: ret = (long)rename((const char *)a1, (const char *)a2); break;
    case 83: ret = (long)mkdir((const char *)a1, (mode_t)a2); break;
    case 84: ret = (long)rmdir((const char *)a1); break;
    case 86: ret = (long)link((const char *)a1, (const char *)a2); break;
    case 87: ret = (long)unlink((const char *)a1); break;
    case 88: ret = (long)symlink((const char *)a1, (const char *)a2); break;
    case 89: ret = (long)readlink((const char *)a1, (char *)a2, (size_t)a3); break;
    case 90: ret = (long)chmod((const char *)a1, (mode_t)a2); break;
    case 92: ret = (long)chown((const char *)a1, (uid_t)a2, (gid_t)a3); break;
    case 102: ret = (long)getuid(); break;
    case 104: ret = (long)getgid(); break;
    case 105: ret = (long)setuid((uid_t)a1); break;
    case 106: ret = (long)setgid((gid_t)a1); break;
    case 107: ret = (long)geteuid(); break;
    case 108: ret = (long)getegid(); break;
    case 109: ret = (long)setpgid((pid_t)a1, (pid_t)a2); break;
    case 110: ret = (long)getppid(); break;
    case 112: ret = (long)setsid(); break;
    case 115: ret = (long)getgroups((int)a1, (gid_t *)a2); break;
    case 116: ret = (long)setgroups((int)a1, (const gid_t *)a2); break;
    case 133: ret = (long)mknod((const char *)a1, (mode_t)a2, (dev_t)a3); break;
    case 160: ret = (long)setrlimit((int)a1, (const struct rlimit *)a2); break;
    case 183: ret = (long)getrlimit((int)a1, (struct rlimit *)a2); break;
    case 207: ret = (long)fchown((int)a1, (uid_t)a2, (gid_t)a3); break;
    case 209: ret = (long)fchmod((int)a1, (mode_t)a2); break;
    case 217: ret = (long)-BEAR_OSIF_ENOSYS; break; /* getdents64: use readdir */
    case 231: _exit((int)a1); __builtin_unreachable();
    case 257: ret = (long)openat((int)a1, (const char *)a2, (int)a3,
                                 (mode_t)a4); break;

    /* Signals */
    case 13: ret = (long)sigaction((int)a1, (const struct sigaction *)a2,
                                   (struct sigaction *)a3); break;
    case 14: ret = (long)sigprocmask((int)a1, (const sigset_t *)a2,
                                     (sigset_t *)a3); break;
    case 132: ret = (long)sigaltstack((const stack_t *)a1, (stack_t *)a2); break;

    /* Network */
    case 41: ret = (long)socket((int)a1, (int)a2, (int)a3); break;
    case 42: ret = (long)connect((int)a1, (const struct sockaddr *)a2, (socklen_t)a3); break;
    case 43: ret = (long)accept((int)a1, (struct sockaddr *)a2, (socklen_t *)a3); break;
    case 44: ret = (long)sendto((int)a1, (const void *)a2, (size_t)a3, (int)a4,
                                (const struct sockaddr *)a5, (socklen_t)a6); break;
    case 45: ret = (long)recvfrom((int)a1, (void *)a2, (size_t)a3, (int)a4,
                                  (struct sockaddr *)a5, (socklen_t *)a6); break;
    case 46: ret = (long)sendmsg((int)a1, (const struct msghdr *)a2, (int)a3); break;
    case 47: ret = (long)recvmsg((int)a1, (struct msghdr *)a2, (int)a3); break;
    case 48: ret = (long)shutdown((int)a1, (int)a2); break;
    case 49: ret = (long)bind((int)a1, (const struct sockaddr *)a2, (socklen_t)a3); break;
    case 50: ret = (long)listen((int)a1, (int)a2); break;
    case 201: ret = (long)socketpair((int)a1, (int)a2, (int)a3, (int *)a4); break;
    case 203: ret = (long)getsockname((int)a1, (struct sockaddr *)a2, (socklen_t *)a3); break;
    case 204: ret = (long)getpeername((int)a1, (struct sockaddr *)a2, (socklen_t *)a3); break;
    case 205: ret = (long)setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (socklen_t)a5); break;
    case 206: ret = (long)getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (socklen_t *)a5); break;

    /* Time */
    case 24: ret = (long)sched_yield(); break;
    case 35: ret = (long)nanosleep((const struct timespec *)a1, (struct timespec *)a2); break;
    case 63: ret = (long)uname((struct utsname *)a1); break;
    case 228: ret = (long)clock_gettime((clockid_t)a1, (struct timespec *)a2); break;
    case 229: ret = (long)clock_getres((clockid_t)a1, (struct timespec *)a2); break;

    /* Misc */
    case 39: ret = (long)getpid(); break;

    /* Linux-only: no NetBSD equivalent, ENOSYS by design */
    default: ret = -BEAR_OSIF_ENOSYS; break;
    }

    /* libc sets errno on failure; translate to negative return */
    if (ret < 0) return -errno;
    return ret;
}

long bear_osif_syscall6(long nr, long a1, long a2, long a3,
                         long a4, long a5, long a6)
{
    return netbsd_syscall(nr, a1, a2, a3, a4, a5, a6);
}

void bear_osif_exit(int code)
{
    _exit(code);
    __builtin_unreachable();
}
