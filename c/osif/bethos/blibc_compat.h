/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* BethOS Bear libc compatibility definitions. */
#ifndef JASH_BETHOS_BLIBC_COMPAT_H
#define JASH_BETHOS_BLIBC_COMPAT_H

#ifdef JASH_BLIBC
#include <time.h>
#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif
#ifndef _CS_PATH
#define _CS_PATH 0
#endif
double bear_atof(const char *s);
long long bear_atoll(const char *s);
int bear_rand(void);
int bear_srand(unsigned int seed);
int bear_fscanf(void *stream, const char *format, ...);
int bear_sscanf(const char *text, const char *format, ...);
#define atof bear_atof
#define atoll bear_atoll
#define rand bear_rand
#define srand bear_srand
#define fscanf bear_fscanf
#define sscanf bear_sscanf

extern int bear_fchmod(int fd, int mode);
extern unsigned int bear_geteuid(void);
extern unsigned int bear_getegid(void);
extern unsigned int bear_getuid(void);
extern int bear_pipe2(int fds[2], int flags);
extern int bear_sigtimedwait(const void *set, void *info, const void *timeout);
extern int bear_umask(int mask);
extern char *bear_mkdtemp(char *template);
extern char *bear_realpath(const char *path, char *resolved);
extern int bear_usleep(unsigned long usec);
extern int tcsetpgrp(int fd, int pgrp);
extern int setpgid(int pid, int pgid);
extern int getpgrp(void);
extern int renameat(int oldfd, const char *oldpath, int newfd,
                    const char *newpath);
#define fchmod bear_fchmod
#define geteuid bear_geteuid
#define getegid bear_getegid
#define getuid bear_getuid
#define pipe2 bear_pipe2
#define sigtimedwait bear_sigtimedwait
#define umask bear_umask
#define mkdtemp bear_mkdtemp
#define realpath bear_realpath
#define usleep bear_usleep
static inline int jash_bethos_nanosleep(const struct timespec *request,
                                        struct timespec *remaining)
{
    return bear_nanosleep((const struct bear_timespec *)request,
                          (struct bear_timespec *)remaining);
}
#define nanosleep jash_bethos_nanosleep

static inline char *jash_bethos_strsep(char **stringp, const char *delim)
{
    char *start;
    char *cursor;
    const char *separator;
    int is_separator;
    if (!stringp || !(start = *stringp)) return NULL;
    cursor = start;
    while (*cursor) {
        is_separator = 0;
        for (separator = delim; separator && *separator; separator++)
            if (*separator == *cursor) { is_separator = 1; break; }
        if (is_separator) break;
        cursor++;
    }
    if (*cursor) *cursor++ = '\0';
    *stringp = cursor;
    return start;
}
#define strsep jash_bethos_strsep

static inline void *jash_bethos_memmem(const void *haystack, size_t haystack_len,
                                       const void *needle, size_t needle_len)
{
    const unsigned char *hay = (const unsigned char *)haystack;
    const unsigned char *want = (const unsigned char *)needle;
    size_t i;
    size_t j;
    if (!needle_len) return (void *)haystack;
    if (needle_len > haystack_len) return NULL;
    for (i = 0; i + needle_len <= haystack_len; i++) {
        for (j = 0; j < needle_len && hay[i + j] == want[j]; j++) {}
        if (j == needle_len) return (void *)(hay + i);
    }
    return NULL;
}
#define memmem jash_bethos_memmem

static inline int jash_bethos_clock_gettime(
    bear_clockid_t id, struct timespec *tp)
{
    return bear_clock_gettime(id, (struct bear_timespec *)tp);
}
#undef clock_gettime
#define clock_gettime jash_bethos_clock_gettime
#endif

#endif
