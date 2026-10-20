// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int published;
static int fault(const char* code) {
    const char* requested = getenv("JASH_TEST_DF_FAULT");
    return requested && !strcmp(requested, code);
}
static int temporary_descriptor(int fd) {
    char name[64], path[4096];
    int n = snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    if (n < 0 || (size_t)n >= sizeof(name)) return 0;
    ssize_t count = readlink(name, path, sizeof(path) - 1u);
    if (count < 0) return 0;
    path[count] = 0;
    const char* base = strrchr(path, '/');
    return base && !strncmp(base + 1, ".jash-df-", 9);
}
#define NEXT(symbol_name, function) do { \
    void* symbol = dlsym(RTLD_NEXT, (symbol_name)); \
    _Static_assert(sizeof(symbol) == sizeof(function), "POSIX dlsym pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
} while (0)

ssize_t write(int fd, const void* bytes, size_t count) {
    ssize_t (*next)(int, const void*, size_t); NEXT("write", next);
    if (!next) { errno = ENOSYS; return -1; }
    if ((fault("write") && temporary_descriptor(fd)) ||
        (fault("sink-after-publish") && published && fd == STDOUT_FILENO)) {
        errno = ENOSPC; return -1;
    }
    return next(fd, bytes, count);
}

int linkat(int old_dir, const char* old_name, int new_dir, const char* new_name, int flags) {
    int (*next)(int, const char*, int, const char*, int); NEXT("linkat", next);
    if (!next) { errno = ENOSYS; return -1; }
    int baseline = !strncmp(old_name, "/proc/self/fd/", 14) && !strcmp(new_name, "baseline.bin");
    if (baseline && fault("publish")) { errno = ENOSPC; return -1; }
    int result = next(old_dir, old_name, new_dir, new_name, flags);
    if (!result && baseline) published = 1;
    return result;
}

int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if (!next) { errno = ENOSYS; return -1; }
    struct stat info;
    if (published && fault("directory-sync") && !fstat(fd, &info) && S_ISDIR(info.st_mode)) {
        errno = EIO; return -1;
    }
    return next(fd);
}

int unlinkat(int fd, const char* name, int flags) {
    int (*next)(int, const char*, int); NEXT("unlinkat", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (published && fault("cleanup") && !strncmp(name, ".jash-df-", 9)) { errno = EACCES; return -1; }
    return next(fd, name, flags);
}
