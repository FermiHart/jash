/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* POSIX surface entries that compile cleanly before their BethOS providers
 * exist. Each entry fails closed rather than claiming host-kernel behavior. */
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/times.h>
#include <termios.h>

#include "osif/osif.h"
#include "osif/bethos/surface.h"
#include "abi-linux/syscalls/syscalls.h"

#undef tcsetpgrp
#undef setpgid
#undef getpgrp
#undef setsid
#undef isatty

unsigned char *bear_at_random;
char **environ;

static unsigned long bethos_rand_state = 1;

double bear_atof(const char *text)
{
    double value = 0.0;
    double scale = 0.1;
    int negative = 0;
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '-') { negative = 1; text++; }
    else if (*text == '+') text++;
    while (*text >= '0' && *text <= '9') value = value * 10.0 + (*text++ - '0');
    if (*text == '.') {
        for (text++; *text >= '0' && *text <= '9'; text++) {
            value += (*text++ - '0') * scale;
            scale *= 0.1;
        }
    }
    return negative ? -value : value;
}

int bear_rand(void)
{
    bethos_rand_state = bethos_rand_state * 1103515245u + 12345u;
    return (int)((bethos_rand_state >> 16) & 0x7fff);
}

int bear_srand(unsigned int seed)
{
    bethos_rand_state = seed ? seed : 1;
    return 0;
}

static int bethos_enosys(void)
{
    errno = ENOSYS;
    return -1;
}

long jash_bethos_surface_emit(uint32_t cell_type, const void *data,
                              size_t length)
{
    long result = bear_osif_syscall6(300, 1, cell_type, (long)data, length,
                                     0, 0);
    if (result < 0) errno = (int)-result;
    return result;
}

int jash_bethos_surface_clear(void)
{
    const uint32_t control = JASH_BETHOS_SURFACE_CONTROL_CLEAR;
    return jash_bethos_surface_emit(JASH_BETHOS_SURFACE_CELL_CONTROL,
                                     &control, sizeof(control)) < 0 ? -1 : 0;
}

int jash_bethos_surface_prompt(const char *text, size_t length)
{
    return jash_bethos_surface_emit(JASH_BETHOS_SURFACE_CELL_PROMPT,
                                    text, length) < 0 ? -1 : 0;
}

int openat(int dirfd, const char *path, int flags, ...);
int unlinkat(int dirfd, const char *path, int flags);
int linkat(int oldfd, const char *oldpath, int newfd, const char *newpath,
           int flags);
int symlinkat(const char *target, int dirfd, const char *path);
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size);
int flock(int fd, int operation);
int setsid(void);

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    va_list args;
    long result;

    if (flags & 0100) {
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    result = bear_osif_call6(__NR_openat, dirfd, path, flags, mode, 0, 0,
                             &errno);
    return result < 0 ? -1 : (int)result;
}
int mkdirat(int dirfd, const char *path, mode_t mode)
{
    return (int)bear_osif_call3(__NR_mkdirat, dirfd, path, mode, &errno);
}
int unlinkat(int dirfd, const char *path, int flags)
{
    return (int)bear_osif_call3(__NR_unlinkat, dirfd, path, flags, &errno);
}
int renameat(int oldfd, const char *oldpath, int newfd, const char *newpath)
{
    return (int)bear_osif_call4(__NR_renameat, oldfd, oldpath, newfd, newpath,
                                &errno);
}
int linkat(int oldfd, const char *oldpath, int newfd, const char *newpath,
           int flags)
{ (void)oldfd; (void)oldpath; (void)newfd; (void)newpath; (void)flags; return bethos_enosys(); }
int symlinkat(const char *target, int dirfd, const char *path)
{ (void)target; (void)dirfd; (void)path; return bethos_enosys(); }
ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size)
{ (void)dirfd; (void)path; (void)buf; (void)size; return bethos_enosys(); }
int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{ (void)dirfd; (void)path; (void)mode; (void)flags; return bethos_enosys(); }
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{ (void)dirfd; (void)path; (void)times; (void)flags; return bethos_enosys(); }
int flock(int fd, int operation)
{ (void)fd; (void)operation; return bethos_enosys(); }
int statvfs(const char *path, struct statvfs *buf)
{ (void)path; (void)buf; return bethos_enosys(); }
clock_t times(struct tms *buf)
{ (void)buf; errno = ENOSYS; return (clock_t)-1; }

int bear_isatty(int fd)
{
    /* A BethOS surface is not a POSIX terminal. Jash's Beyond path provides
     * line input without termios, cursor state, or a controlling tty. */
    (void)fd;
    errno = ENOTTY;
    return 0;
}
int bear_tcgetattr(int fd, struct bear_termios *termios)
{ (void)fd; (void)termios; return bethos_enosys(); }
int bear_tcsetattr(int fd, int actions, const struct bear_termios *termios)
{ (void)fd; (void)actions; (void)termios; return bethos_enosys(); }
int bear_tcsetpgrp(int fd, int pgrp)
{ (void)fd; (void)pgrp; return bethos_enosys(); }
int bear_setpgid(int pid, int pgid)
{ (void)pid; (void)pgid; return bethos_enosys(); }
int bear_setsid(void)
{ return bethos_enosys(); }
int bear_getpgrp(void)
{ return bethos_enosys(); }

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (!attr) return EINVAL;
    attr->__flags = flags;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr,
                                  const sigset_t *set)
{
    if (!attr || !set) return EINVAL;
    attr->__sigdefault = *set;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    if (!attr) return EINVAL;
    *attr = (posix_spawnattr_t){0};
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    (void)attr;
    return 0;
}

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{
    if (!actions) return EINVAL;
    *actions = (posix_spawn_file_actions_t){0};
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)
{
    (void)actions;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                     int fd, const char *path, int oflag,
                                     mode_t mode)
{
    (void)actions; (void)fd; (void)path; (void)oflag; (void)mode;
    return ENOSYS;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                     int oldfd, int newfd)
{
    (void)actions; (void)oldfd; (void)newfd;
    return ENOSYS;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[])
{
    (void)pid; (void)path; (void)actions; (void)attr; (void)argv; (void)envp;
    return ENOSYS;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[])
{
    (void)pid; (void)file; (void)actions; (void)attr; (void)argv; (void)envp;
    return ENOSYS;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    (void)fd;
    (void)pgrp;
    errno = ENOSYS;
    return -1;
}

int setsid(void)
{
    return bear_setsid();
}

int setpgid(pid_t pid, pid_t pgid)
{
    (void)pid;
    (void)pgid;
    errno = ENOSYS;
    return -1;
}

int getpgrp(void)
{
    errno = ENOSYS;
    return -1;
}
