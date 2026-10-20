/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* linux01_syscall.h — J01: raw Linux 0.01 (i386, `int $0x80`) syscall layer.
 *
 * Part of the Jash OSIF family, target "linux01". It mirrors the contract of
 * `c/osif/osif.h` (negative-errno returns) but speaks the 1991 ABI:
 * 32-bit `int $0x80`, historical syscall numbers 0..66. The fork adds native
 * versioned process metadata at 67 (include/linux/procinfo.h).
 *
 * syscall.c maps supported BEAR_OSIF_NR_* requests (x86_64 numbers in
 * `../osif.h`) to its 0.01 __NR_* twin. Anything the 1991 kernel never had
 * (mmap, sockets, epoll, inotify, threads, ...) returns -ENOSYS (-38, the
 * same value as OSIF_ENOSYS — not a coincidence, the same errno table).
 *
 * Freestanding: no libc, no heap. Compile -m32 -ffreestanding -nostdlib.
 * Author: F E R M I ∞ H A R T <contact@fermihart.com>
 */
#ifndef JASH_LINUX01_SYSCALL_H
#define JASH_LINUX01_SYSCALL_H

/* ── Linux 0.01 syscall numbers (include/unistd.h) ─────────────────────── */
#define L01_NR_exit     1
#define L01_NR_fork     2
#define L01_NR_read     3
#define L01_NR_write    4
#define L01_NR_open     5
#define L01_NR_close    6
#define L01_NR_waitpid  7
#define L01_NR_creat    8
#define L01_NR_link     9
#define L01_NR_unlink   10
#define L01_NR_execve   11
#define L01_NR_chdir    12
#define L01_NR_time     13
#define L01_NR_mknod    14
#define L01_NR_chmod    15
#define L01_NR_chown    16
#define L01_NR_stat     18
#define L01_NR_lseek    19
#define L01_NR_getpid   20
#define L01_NR_mount    21
#define L01_NR_umount   22
#define L01_NR_setuid   23
#define L01_NR_getuid   24
#define L01_NR_alarm    27
#define L01_NR_fstat    28
#define L01_NR_pause    29
#define L01_NR_sync     36
#define L01_NR_kill     37
#define L01_NR_rename   38
#define L01_NR_mkdir    39
#define L01_NR_rmdir    40
#define L01_NR_dup      41
#define L01_NR_pipe     42
#define L01_NR_brk      45
#define L01_NR_setgid   46
#define L01_NR_getgid   47
#define L01_NR_geteuid  49
#define L01_NR_ioctl    54
#define L01_NR_fcntl    55
#define L01_NR_setpgid  57
#define L01_NR_uname    59
#define L01_NR_umask    60
#define L01_NR_dup2     63
#define L01_NR_getppid  64
#define L01_NR_setsid   66

#define L01_ENOSYS 38

/* ── Raw traps: kernel returns >=0 or -errno ───────────────────────────── */
/* NOTE: every trap carries a "memory" clobber. The kernel writes through
 * user pointers (pipe fds, stat bufs, wait status); without the clobber
 * gcc may read stale stack slots after the syscall (found 2026-09-30). */
static inline long l01_trap0(long nr)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a" (r) : "0" (nr) : "memory");
    return r;
}
static inline long l01_trap1(long nr, long a)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a" (r) : "0" (nr), "b" (a) : "memory");
    return r;
}
static inline long l01_trap2(long nr, long a, long b)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a" (r)
        : "0" (nr), "b" (a), "c" (b) : "memory");
    return r;
}
static inline long l01_trap3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a" (r)
        : "0" (nr), "b" (a), "c" (b), "d" (c) : "memory");
    return r;
}

/* ── Friendly wrappers (negative errno, OSIF style) ────────────────────── */
static inline long l01_exit(int s) { return l01_trap1(L01_NR_exit, s); }
static inline long l01_fork(void) { return l01_trap0(L01_NR_fork); }
static inline long l01_read(int f, void *b, long n)
{ return l01_trap3(L01_NR_read, f, (long)b, n); }
static inline long l01_write(int f, const void *b, long n)
{ return l01_trap3(L01_NR_write, f, (long)b, n); }
static inline long l01_open(const char *p, int fl, int mo)
{ return l01_trap3(L01_NR_open, (long)p, fl, mo); }
static inline long l01_close(int f) { return l01_trap1(L01_NR_close, f); }
static inline long l01_waitpid(int p, int *s, int o)
{ return l01_trap3(L01_NR_waitpid, p, (long)s, o); }
static inline long l01_creat(const char *p, int m)
{ return l01_trap2(L01_NR_creat, (long)p, m); }
static inline long l01_link(const char *a, const char *b)
{ return l01_trap2(L01_NR_link, (long)a, (long)b); }
static inline long l01_unlink(const char *p)
{ return l01_trap1(L01_NR_unlink, (long)p); }
static inline long l01_execve(const char *p, char **a, char **e)
{ return l01_trap3(L01_NR_execve, (long)p, (long)a, (long)e); }
static inline long l01_chdir(const char *p)
{ return l01_trap1(L01_NR_chdir, (long)p); }
static inline long l01_time(long *t)
{ return l01_trap1(L01_NR_time, (long)t); }
static inline long l01_mknod(const char *p, int m, int d)
{ return l01_trap3(L01_NR_mknod, (long)p, m, d); }
static inline long l01_chmod(const char *p, int m)
{ return l01_trap2(L01_NR_chmod, (long)p, m); }
static inline long l01_stat(const char *p, void *s)
{ return l01_trap2(L01_NR_stat, (long)p, (long)s); }
static inline long l01_lseek(int f, long o, int w)
{ return l01_trap3(L01_NR_lseek, f, o, w); }
static inline long l01_getpid(void) { return l01_trap0(L01_NR_getpid); }
static inline long l01_getppid(void) { return l01_trap0(L01_NR_getppid); }
static inline long l01_getuid(void) { return l01_trap0(L01_NR_getuid); }
static inline long l01_geteuid(void) { return l01_trap0(L01_NR_geteuid); }
static inline long l01_getgid(void) { return l01_trap0(L01_NR_getgid); }
static inline long l01_sync(void) { return l01_trap0(L01_NR_sync); }
static inline long l01_kill(int p, int s)
{ return l01_trap2(L01_NR_kill, p, s); }
static inline long l01_rename(const char *a, const char *b)
{ return l01_trap2(L01_NR_rename, (long)a, (long)b); }
static inline long l01_mkdir(const char *p, int m)
{ return l01_trap2(L01_NR_mkdir, (long)p, m); }
static inline long l01_rmdir(const char *p)
{ return l01_trap1(L01_NR_rmdir, (long)p); }
static inline long l01_dup(int f) { return l01_trap1(L01_NR_dup, f); }
static inline long l01_dup2(int o, int n)
{ return l01_trap2(L01_NR_dup2, o, n); }
static inline long l01_pipe(int *f)
{ return l01_trap1(L01_NR_pipe, (long)f); }
static inline long l01_brk(void *e) { return l01_trap1(L01_NR_brk, (long)e); }
static inline long l01_ioctl(int f, unsigned long r, void *a)
{ return l01_trap3(L01_NR_ioctl, f, (long)r, (long)a); }
static inline long l01_fcntl(int f, int c, long a)
{ return l01_trap3(L01_NR_fcntl, f, c, a); }
static inline long l01_setpgid(int p, int g)
{ return l01_trap2(L01_NR_setpgid, p, g); }
static inline long l01_setsid(void) { return l01_trap0(L01_NR_setsid); }
static inline long l01_uname(void *u)
{ return l01_trap1(L01_NR_uname, (long)u); }
static inline long l01_umask(int m) { return l01_trap1(L01_NR_umask, m); }
static inline long l01_pause(void) { return l01_trap0(L01_NR_pause); }
static inline long l01_alarm(int s) { return l01_trap1(L01_NR_alarm, s); }

#endif /* JASH_LINUX01_SYSCALL_H */
