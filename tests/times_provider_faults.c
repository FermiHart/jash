// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Test-only libc seam. No environment-controlled fault path enters Jash. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <unistd.h>

static const char* mode(void) {
    const char* value = getenv("JASH_TEST_TIMES_MODE");
    return value ? value : "valid";
}

long sysconf(int name) {
    if (name == _SC_CLK_TCK) {
        if (!strcmp(mode(), "rate-zero")) return 0;
        if (!strcmp(mode(), "rate-negative")) { errno = EINVAL; return -1; }
        return 250;
    }
    void* symbol = dlsym(RTLD_NEXT, "sysconf");
    long (*next)(int);
    _Static_assert(sizeof next == sizeof symbol, "test host function pointer ABI");
    memcpy(&next, &symbol, sizeof next);
    if (!next) { errno = ENOSYS; return -1; }
    return next(name);
}

clock_t times(struct tms* value) {
    /* A missing tick rate must be rejected before consulting CPU counters. */
    if (!strncmp(mode(), "rate-", 5)) _exit(91);
    if (!strcmp(mode(), "fail") || !strcmp(mode(), "unsupported")) {
        errno = !strcmp(mode(), "unsupported") ? ENOTSUP : EIO;
        return (clock_t)-1; /* Deliberately leave the output untouched. */
    }
    if (!value) { errno = EFAULT; return (clock_t)-1; }
    value->tms_utime = 18750;
    value->tms_stime = 563;
    value->tms_cutime = 30000;
    value->tms_cstime = 31;
    return 123456;
}
