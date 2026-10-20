/* Deterministic corruption/read failure at the real cp --verify boundary. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int target_fd(int fd) {
    const char* target = getenv("AUDIT_CP_TARGET");
    char link[64], path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n < 0 || !target) return 0;
    path[n] = 0;
    return !strcmp(path, target);
}

static void mark(void) {
    const char* marker = getenv("AUDIT_CP_MARKER");
    if (!marker) return;
    int fd = open(marker, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) close(fd);
}

ssize_t write(int fd, const void* data, size_t n) {
    ssize_t (*real_write)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
    if (getenv("AUDIT_CP_CORRUPT") && target_fd(fd) && n == 2 && !memcmp(data, "Ab", 2)) {
        mark();
        return real_write(fd, "BA", 2); /* same djb2, different BLAKE3 */
    }
    return real_write(fd, data, n);
}

ssize_t pread(int fd, void* data, size_t n, off_t offset) {
    ssize_t (*real_pread)(int, void*, size_t, off_t) = dlsym(RTLD_NEXT, "pread");
    if (getenv("AUDIT_CP_READ_ERROR") && target_fd(fd)) {
        mark(); errno = EIO; return -1;
    }
    return real_pread(fd, data, n, offset);
}
