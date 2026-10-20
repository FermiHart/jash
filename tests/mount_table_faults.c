// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Test-only preload: unrelated shell/runtime file reads retain libc behavior. */
FILE* fopen(const char* path, const char* mode) {
    if (!strcmp(path, "/proc/mounts") || !strcmp(path, "/etc/mtab")) {
        errno = EIO;
        return NULL;
    }
    FILE* (*next)(const char*, const char*);
    void* symbol = dlsym(RTLD_NEXT, "fopen");
    _Static_assert(sizeof(next) == sizeof(symbol), "POSIX dlsym pointer size");
    memcpy(&next, &symbol, sizeof(next));
    if (!next) { errno = ENOSYS; return NULL; }
    return next(path, mode);
}
