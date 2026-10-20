// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Selected model-store writes fail at fclose after real buffered writes. Reads
 * and every unrelated file use libc unchanged. A full device provides the
 * independent ENOSPC failure; no production code checks this test variable. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

FILE* fopen(const char* path, const char* mode) {
    static FILE* (*real_open)(const char*, const char*);
    if (!real_open) *(void**)(&real_open) = dlsym(RTLD_NEXT, "fopen");
    const char* target = getenv("JASH_TEST_MODEL_FAIL_WRITE");
    const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
    if (target && !strcmp(name, target) && (strchr(mode, 'w') || strchr(mode, 'a'))) return real_open("/dev/full", mode);
    return real_open(path, mode);
}

FILE* fdopen(int fd, const char* mode) {
    static FILE* (*real_fdopen)(int, const char*);
    static FILE* (*real_open)(const char*, const char*);
    if (!real_fdopen) *(void**)(&real_fdopen) = dlsym(RTLD_NEXT, "fdopen");
    if (!real_open) *(void**)(&real_open) = dlsym(RTLD_NEXT, "fopen");
    const char* target = getenv("JASH_TEST_MODEL_FAIL_WRITE");
    if (target && (strchr(mode, 'w') || strchr(mode, 'a'))) {
        char link[64], path[PATH_MAX];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t length = readlink(link, path, sizeof(path) - 1);
        if (length > 0) {
            path[length] = 0;
            const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
            if (!strcmp(name, target)) { close(fd); return real_open("/dev/full", mode); }
        }
    }
    return real_fdopen(fd, mode);
}
