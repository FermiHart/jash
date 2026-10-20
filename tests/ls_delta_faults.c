// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int published;
static int selected(const char* mode) {
    const char* value = getenv("JASH_TEST_LS_DELTA_FAULT"); return value && !strcmp(value, mode);
}
static int directory(int fd) {
    const char* home = getenv("HOME"); if (!home) return 0;
    char link[64], path[PATH_MAX]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t count = readlink(link, path, sizeof(path) - 1u);
    if (count < 0 || (size_t)count == sizeof(path) - 1u) return 0;
    path[count] = 0;
    return !strncmp(path, home, strlen(home)) && !strcmp(path + strlen(home), "/.jash/gold-delta/ls");
}
#define NEXT(name, value) do { void* symbol = dlsym(RTLD_NEXT, name); memcpy(&(value), &symbol, sizeof(value)); } while (0)
int openat(int fd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args); }
    int (*next)(int, const char*, int, ...); NEXT("openat", next);
    if (selected("metadata") && !strcmp(path, "vanish") && (flags & O_PATH)) { errno = ENOENT; return -1; }
    return next(fd, path, flags, mode);
}
int __openat_2(int fd, const char* path, int flags) {
    int (*next)(int, const char*, int); NEXT("__openat_2", next);
    if (selected("metadata") && !strcmp(path, "vanish") && (flags & O_PATH)) { errno = ENOENT; return -1; }
    return next(fd, path, flags);
}
int fstatat(int fd, const char* path, struct stat* metadata, int flags) {
    int (*next)(int, const char*, struct stat*, int); NEXT("fstatat", next);
    if (selected("metadata") && !strcmp(path, "vanish")) { errno = ENOENT; return -1; }
    return next(fd, path, metadata, flags);
}
int renameat(int old_fd, const char* old_path, int new_fd, const char* new_path) {
    int (*next)(int, const char*, int, const char*); NEXT("renameat", next);
    int snapshot = directory(new_fd) && strstr(new_path, ".snap");
    if (snapshot && selected("rename")) { errno = EIO; return -1; }
    int result = next(old_fd, old_path, new_fd, new_path);
    if (!result && snapshot) published = 1;
    return result;
}
int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if (published && selected("directory-sync") && directory(fd)) { errno = EIO; return -1; }
    return next(fd);
}
ssize_t write(int fd, const void* bytes, size_t count) {
    ssize_t (*next)(int, const void*, size_t); NEXT("write", next);
    if (fd == STDOUT_FILENO && published && selected("sink-after-publish")) { errno = EPIPE; return -1; }
    return next(fd, bytes, count);
}
