/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Translate Jash's stable Linux-number OSIF requests to MINIX libc. */
#include "../osif.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* MINIX's public sched.h redirects this name to an internal libc hook; the
 * selected userland provides the public implementation in libmthread.a. */
#undef sched_yield

static long
minix_result(long result)
{
    return result < 0 ? -errno : result;
}

long
bear_osif_syscall6(long nr, long a1, long a2, long a3,
    long a4, long a5, long a6)
{
    long result;

    switch (nr) {
    case BEAR_OSIF_NR_read:
        result = (long)read((int)a1, (void *)a2, (size_t)a3);
        break;
    case BEAR_OSIF_NR_write:
        result = (long)write((int)a1, (const void *)a2, (size_t)a3);
        break;
    case BEAR_OSIF_NR_open:
        result = (long)open((const char *)a1, (int)a2, (mode_t)a3);
        break;
    case BEAR_OSIF_NR_close:
        result = (long)close((int)a1);
        break;
    case BEAR_OSIF_NR_stat:
        result = (long)stat((const char *)a1, (struct stat *)a2);
        break;
    case BEAR_OSIF_NR_fstat:
        result = (long)fstat((int)a1, (struct stat *)a2);
        break;
    case BEAR_OSIF_NR_lstat:
        result = (long)lstat((const char *)a1, (struct stat *)a2);
        break;
    case BEAR_OSIF_NR_lseek:
        result = (long)lseek((int)a1, (off_t)a2, (int)a3);
        break;
    case BEAR_OSIF_NR_mmap: {
        void *address = mmap((void *)a1, (size_t)a2, (int)a3,
            (int)a4, (int)a5, (off_t)a6);
        if (address == MAP_FAILED) return -errno;
        return (long)(intptr_t)address;
    }
    case BEAR_OSIF_NR_mprotect:
        /* MINIX lib/libc/sys/MISSING_SYSCALLS lists mprotect as unsupported. */
        return -BEAR_OSIF_ENOSYS;
    case BEAR_OSIF_NR_munmap:
        result = (long)munmap((void *)a1, (size_t)a2);
        break;
    case BEAR_OSIF_NR_ioctl:
        result = (long)ioctl((int)a1, (unsigned long)a2, (void *)a3);
        break;
    case BEAR_OSIF_NR_dup:
        result = (long)dup((int)a1);
        break;
    case BEAR_OSIF_NR_dup2:
        result = (long)dup2((int)a1, (int)a2);
        break;
    case BEAR_OSIF_NR_nanosleep:
        result = (long)nanosleep((const struct timespec *)a1,
            (struct timespec *)a2);
        break;
    case BEAR_OSIF_NR_getpid:
        return (long)getpid();
    case BEAR_OSIF_NR_socket:
        result = (long)socket((int)a1, (int)a2, (int)a3);
        break;
    case BEAR_OSIF_NR_connect:
        result = (long)connect((int)a1, (const struct sockaddr *)a2,
            (socklen_t)a3);
        break;
    case BEAR_OSIF_NR_accept:
        result = (long)accept((int)a1, (struct sockaddr *)a2,
            (socklen_t *)a3);
        break;
    case BEAR_OSIF_NR_sendto:
        result = (long)sendto((int)a1, (const void *)a2, (size_t)a3,
            (int)a4, (const struct sockaddr *)a5, (socklen_t)a6);
        break;
    case BEAR_OSIF_NR_recvfrom:
        result = (long)recvfrom((int)a1, (void *)a2, (size_t)a3,
            (int)a4, (struct sockaddr *)a5, (socklen_t *)a6);
        break;
    case BEAR_OSIF_NR_sendmsg:
        result = (long)sendmsg((int)a1, (const struct msghdr *)a2,
            (int)a3);
        break;
    case BEAR_OSIF_NR_recvmsg:
        result = (long)recvmsg((int)a1, (struct msghdr *)a2, (int)a3);
        break;
    case BEAR_OSIF_NR_shutdown:
        result = (long)shutdown((int)a1, (int)a2);
        break;
    case BEAR_OSIF_NR_bind:
        result = (long)bind((int)a1, (const struct sockaddr *)a2,
            (socklen_t)a3);
        break;
    case BEAR_OSIF_NR_listen:
        result = (long)listen((int)a1, (int)a2);
        break;
    case BEAR_OSIF_NR_fork:
    case BEAR_OSIF_NR_vfork:
        result = (long)fork();
        break;
    case BEAR_OSIF_NR_execve:
        result = (long)execve((const char *)a1, (char *const *)a2,
            (char *const *)a3);
        break;
    case BEAR_OSIF_NR_exit:
    case BEAR_OSIF_NR_exit_group:
        _exit((int)a1);
        __builtin_unreachable();
    case BEAR_OSIF_NR_wait4:
        result = (long)wait4((pid_t)a1, (int *)a2, (int)a3,
            (struct rusage *)a4);
        break;
    case BEAR_OSIF_NR_kill:
        result = (long)kill((pid_t)a1, (int)a2);
        break;
    case BEAR_OSIF_NR_uname:
        result = (long)uname((struct utsname *)a1);
        break;
    case BEAR_OSIF_NR_fcntl:
        result = (long)fcntl((int)a1, (int)a2, (void *)a3);
        break;
    case BEAR_OSIF_NR_fsync:
        result = (long)fsync((int)a1);
        break;
    case BEAR_OSIF_NR_ftruncate:
        result = (long)ftruncate((int)a1, (off_t)a2);
        break;
    case BEAR_OSIF_NR_getcwd: {
        char *cwd = getcwd((char *)a1, (size_t)a2);
        if (cwd == NULL) return -errno;
        return (long)(intptr_t)cwd;
    }
    case BEAR_OSIF_NR_chdir:
        result = (long)chdir((const char *)a1);
        break;
    case BEAR_OSIF_NR_rename:
        result = (long)rename((const char *)a1, (const char *)a2);
        break;
    case BEAR_OSIF_NR_mkdir:
        result = (long)mkdir((const char *)a1, (mode_t)a2);
        break;
    case BEAR_OSIF_NR_rmdir:
        result = (long)rmdir((const char *)a1);
        break;
    case BEAR_OSIF_NR_creat:
        result = (long)creat((const char *)a1, (mode_t)a2);
        break;
    case BEAR_OSIF_NR_unlink:
        result = (long)unlink((const char *)a1);
        break;
    case BEAR_OSIF_NR_readlink:
        result = (long)readlink((const char *)a1, (char *)a2,
            (size_t)a3);
        break;
    case BEAR_OSIF_NR_chmod:
        result = (long)chmod((const char *)a1, (mode_t)a2);
        break;
    case BEAR_OSIF_NR_getuid:
        return (long)getuid();
    case BEAR_OSIF_NR_getgid:
        return (long)getgid();
    case BEAR_OSIF_NR_geteuid:
        return (long)geteuid();
    case BEAR_OSIF_NR_getegid:
        return (long)getegid();
    case BEAR_OSIF_NR_setpgid:
        result = (long)setpgid((pid_t)a1, (pid_t)a2);
        break;
    case BEAR_OSIF_NR_getppid:
        return (long)getppid();
    case BEAR_OSIF_NR_setsid:
        result = (long)setsid();
        break;
    case BEAR_OSIF_NR_sigaltstack:
        /* MINIX lib/libc/sys/MISSING_SYSCALLS lists sigaltstack as unsupported. */
        return -BEAR_OSIF_ENOSYS;
    case BEAR_OSIF_NR_getrlimit:
        result = (long)getrlimit((int)a1, (struct rlimit *)a2);
        break;
    case BEAR_OSIF_NR_getrusage:
        result = (long)getrusage((int)a1, (struct rusage *)a2);
        break;
    case BEAR_OSIF_NR_getgroups:
        result = (long)getgroups((int)a1, (gid_t *)a2);
        break;
    case BEAR_OSIF_NR_setgroups:
        result = (long)setgroups((int)a1, (const gid_t *)a2);
        break;
    case BEAR_OSIF_NR_setuid:
        result = (long)setuid((uid_t)a1);
        break;
    case BEAR_OSIF_NR_setgid:
        result = (long)setgid((gid_t)a1);
        break;
    case BEAR_OSIF_NR_clock_gettime:
        result = (long)clock_gettime((clockid_t)a1, (struct timespec *)a2);
        break;
    case BEAR_OSIF_NR_clock_getres:
        result = (long)clock_getres((clockid_t)a1, (struct timespec *)a2);
        break;
    case BEAR_OSIF_NR_sched_yield:
        result = (long)sched_yield();
        break;
    case BEAR_OSIF_NR_pipe:
        result = (long)pipe((int *)a1);
        break;
    case BEAR_OSIF_NR_select:
        result = (long)select((int)a1, (fd_set *)a2, (fd_set *)a3,
            (fd_set *)a4, (struct timeval *)a5);
        break;
    case BEAR_OSIF_NR_link:
        result = (long)link((const char *)a1, (const char *)a2);
        break;
    case BEAR_OSIF_NR_symlink:
        result = (long)symlink((const char *)a1, (const char *)a2);
        break;
    case BEAR_OSIF_NR_chown:
        result = (long)chown((const char *)a1, (uid_t)a2, (gid_t)a3);
        break;
    case BEAR_OSIF_NR_mknod:
        result = (long)mknod((const char *)a1, (mode_t)a2, (dev_t)a3);
        break;
    case BEAR_OSIF_NR_setrlimit:
        result = (long)setrlimit((int)a1, (const struct rlimit *)a2);
        break;
    case BEAR_OSIF_NR_socketpair:
        result = (long)socketpair((int)a1, (int)a2, (int)a3, (int *)a4);
        break;
    case BEAR_OSIF_NR_getsockname:
        result = (long)getsockname((int)a1, (struct sockaddr *)a2,
            (socklen_t *)a3);
        break;
    case BEAR_OSIF_NR_getpeername:
        result = (long)getpeername((int)a1, (struct sockaddr *)a2,
            (socklen_t *)a3);
        break;
    case BEAR_OSIF_NR_setsockopt:
        result = (long)setsockopt((int)a1, (int)a2, (int)a3,
            (const void *)a4, (socklen_t)a5);
        break;
    case BEAR_OSIF_NR_getsockopt:
        result = (long)getsockopt((int)a1, (int)a2, (int)a3,
            (void *)a4, (socklen_t *)a5);
        break;
    case BEAR_OSIF_NR_openat:
        result = (long)openat((int)a1, (const char *)a2, (int)a3,
            (mode_t)a4);
        break;
    default:
        return -BEAR_OSIF_ENOSYS;
    }

    return minix_result(result);
}

void
bear_osif_exit(int code)
{
    _exit(code);
    __builtin_unreachable();
}
