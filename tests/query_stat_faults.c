// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

/* Deterministic large integers and timestamp boundaries without requiring a
 * multi-petabyte filesystem or changing the machine's clock. */
int lstat(const char* path, struct stat* result) {
    int (*next)(const char*, struct stat*);
    void* symbol = dlsym(RTLD_NEXT, "lstat");
    _Static_assert(sizeof(next) == sizeof(symbol), "POSIX dlsym pointer size");
    memcpy(&next, &symbol, sizeof(next));
    if (!next) { errno = ENOSYS; return -1; }
    int status = next(path, result);
    if (status) return status;
    if (!strcmp(path, "synthetic-large")) {
        result->st_size = INT64_C(9007199254740993);
        result->st_ino = UINT64_MAX;
        result->st_mtim.tv_sec = INT64_MAX / INT64_C(1000000000) + 1;
        result->st_mtim.tv_nsec = 987654321;
    } else if (!strcmp(path, "timestamp-min")) {
        result->st_mtim.tv_sec = INT64_MIN / INT64_C(1000000000) - 1;
        result->st_mtim.tv_nsec = INT64_C(1000000000) + INT64_MIN % INT64_C(1000000000);
    } else if (!strcmp(path, "timestamp-max")) {
        result->st_mtim.tv_sec = INT64_MAX / INT64_C(1000000000);
        result->st_mtim.tv_nsec = INT64_MAX % INT64_C(1000000000);
    } else if (!strcmp(path, "negative-size")) result->st_size = -1;
    return 0;
}
