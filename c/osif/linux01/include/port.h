/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Linux 0.01 ILP32 sysroot for the selected canonical Jash/Bear profile.
 */
#ifndef JASH_LINUX01_PORT_H
#define JASH_LINUX01_PORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
typedef long ssize_t;
typedef long time_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned short mode_t;
typedef unsigned short uid_t;
typedef unsigned short gid_t;
struct bear_stat {
    unsigned short st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev;
    long st_size, st_atime, st_mtime, st_ctime;
};
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
                 tm_wday, tm_yday, tm_isdst; };
typedef struct { int fd, error, eof; } FILE;
struct dirent { unsigned short d_ino; char d_name[15]; };
typedef struct { int fd; struct dirent entry; } DIR;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_CLOEXEC 02000000
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_ISDIR(m) (((m)&S_IFMT)==S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT)==S_IFREG)
#define S_IXUSR 0100
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#define F_GETFL 3
#define F_SETFL 4
#define EINTR 4
#define ENOENT 2
#define ENOMEM 12
#define EINVAL 22
#define ENOSYS 38
#define EIO 5
#define ENAMETOOLONG 36
extern int bear_errno;
#define errno bear_errno
#define memcpy bear_memcpy
#define memmove bear_memmove
#define memset bear_memset
#define memcmp bear_memcmp
#define memchr bear_memchr
#define strlen bear_strlen
#define strnlen bear_strnlen
#define strcmp bear_strcmp
#define strncmp bear_strncmp
#define strchr bear_strchr
#define strrchr bear_strrchr
#define strncpy bear_strncpy
#define strlcpy bear_strlcpy
#define strlcat bear_strlcat
#define strdup bear_strdup
#define strstr bear_strstr
#define strtok bear_strtok
#define malloc bear_malloc
#define free bear_free
#define realloc bear_realloc
#define calloc bear_calloc
#define snprintf bear_snprintf
#define vsnprintf bear_vsnprintf
#define printf bear_printf
#define fprintf bear_fprintf
#define fopen bear_fopen
#define fclose bear_fclose
#define fgets bear_fgets
#define fflush bear_fflush
#define fread bear_fread
#define fwrite bear_fwrite
#define getenv bear_getenv
#define read bear_read
#define write bear_write
#define open bear_open
#define close bear_close
#define stat bear_stat
#define fstat bear_fstat
#define chdir bear_chdir
#define getcwd bear_getcwd
#define mkdir bear_mkdir
#define unlink bear_unlink
#define opendir bear_opendir
#define readdir bear_readdir
#define closedir bear_closedir
#define time bear_time
#define localtime bear_localtime
#define difftime bear_difftime
#define lseek bear_lseek
#define isatty bear_isatty
#define getpid bear_getpid
#define uname bear_uname
#define ioctl bear_ioctl
#define fcntl bear_fcntl
#define exit bear_exit
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strncpy(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
char *strdup(const char *);
char *strstr(const char *, const char *);
char *strtok(char *, const char *);
void *malloc(size_t);
void free(void *);
void *realloc(void *, size_t);
void *calloc(size_t, size_t);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
char *fgets(char *, int, FILE *);
int fflush(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
extern FILE *stdout, *stderr, *stdin;
char *getenv(const char *);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int open(const char *, int, ...);
int close(int);
int stat(const char *, struct stat *);
int fstat(int, struct stat *);
int chdir(const char *);
char *getcwd(char *, size_t);
int mkdir(const char *, mode_t);
int unlink(const char *);
off_t lseek(int, off_t, int);
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
time_t time(time_t *);
struct tm *localtime(const time_t *);
double difftime(time_t, time_t);
int isatty(int);
pid_t getpid(void);
int ioctl(int, unsigned long, void *);
int fcntl(int, int, ...);
void exit(int) __attribute__((noreturn));
/* Unselected history/Obsidian code is removed by --gc-sections. */
int sscanf(const char *, const char *, ...);
size_t strftime(char *, size_t, const char *, const struct tm *);
void bear_port_init(char **envp);
struct utsname;
int uname(struct utsname *);
/* Linux01 has no symbolic links or /proc/self/exe. Canonical version.c
 * already omits binary-path/hash fields when this capability is unavailable.
 * Keeping the unsupported leaf inline also excludes its unreachable hashing
 * branch from this small profile, without changing canonical source bytes. */
static inline ssize_t readlink(const char *path,char *buf,size_t cap)
{ (void)path; (void)buf; (void)cap; errno=ENOSYS; return -1; }
unsigned long long __udivdi3(unsigned long long, unsigned long long);
unsigned long long __umoddi3(unsigned long long, unsigned long long);
unsigned long long __udivmoddi4(unsigned long long, unsigned long long, unsigned long long *);
#endif
