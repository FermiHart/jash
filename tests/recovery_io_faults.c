/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Linux-only deterministic IO faults, limited to exact private fixture paths. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NEXT(name, function) do { \
    void* symbol = dlsym(RTLD_NEXT, (name)); \
    _Static_assert(sizeof(function) == sizeof(symbol), "POSIX dlsym pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
} while (0)

static const char* fault(void) {
    const char* mode = getenv("JASH_TEST_RECOVERY_FAULT");
    return mode ? mode : "";
}

static unsigned gc_removed;

static int fixture_fd(int fd, const char* suffix, int prefix) {
    const char* root = getenv("JASH_TEST_RECOVERY_ROOT");
    if (!root) return 0;
    char descriptor[64], path[PATH_MAX];
    snprintf(descriptor, sizeof descriptor, "/proc/self/fd/%d", fd);
    ssize_t size = readlink(descriptor, path, sizeof path - 1u);
    if (size < 0 || (size_t)size == sizeof path - 1u) return 0;
    path[size] = 0;
    size_t root_size = strlen(root);
    if (strncmp(path, root, root_size)) return 0;
    return prefix ? !strncmp(path + root_size, suffix, strlen(suffix)) : !strcmp(path + root_size, suffix);
}

ssize_t read(int fd, void* bytes, size_t size) {
    ssize_t (*next)(int, void*, size_t); NEXT("read", next);
    if (!next) { errno = ENOSYS; return -1; }
    static unsigned calls;
    if (fixture_fd(fd, "/verify-input", 0)) {
        calls++;
        const char* mode = fault();
        if (!strcmp(mode, "verify_read") && calls > 1) { errno = EIO; return -1; }
        if (!strcmp(mode, "verify_short_eof") && calls > 1) return 0;
        if (!strcmp(mode, "verify_eintr_short")) {
            if (calls == 1) { errno = EINTR; return -1; }
            if (size > 17) size = 17;
        }
    }
    return next(fd, bytes, size);
}

int fstat(int fd, struct stat* state) {
    int (*next)(int, struct stat*); NEXT("fstat", next);
    if (!next) { errno = ENOSYS; return -1; }
    int rc = next(fd, state);
    static unsigned calls;
    if (!rc && fixture_fd(fd, "/verify-input", 0)) {
        calls++;
        if (!strcmp(fault(), "verify_changed") && calls > 1) state->st_mtim.tv_nsec ^= 1;
    }
    return rc;
}

int close(int fd) {
    int (*next)(int); NEXT("close", next);
    if (!next) { errno = ENOSYS; return -1; }
    int inject = !strcmp(fault(), "verify_close") && fixture_fd(fd, "/verify-input", 0);
    int rc = next(fd);
    if (!rc && inject) { errno = EIO; return -1; }
    return rc;
}

int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if (!next) { errno = ENOSYS; return -1; }
    if ((!strcmp(fault(), "checkpoint_file_sync") && fixture_fd(fd, "/.jash/.jash-checkpoint-", 1)) ||
        (!strcmp(fault(), "checkpoint_directory_sync") && fixture_fd(fd, "/.jash", 0))) {
        errno = EIO; return -1;
    }
    return next(fd);
}

int renameat(int source_fd, const char* source, int destination_fd, const char* destination) {
    int (*next)(int, const char*, int, const char*); NEXT("renameat", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (!strcmp(fault(), "checkpoint_rename") && !strcmp(destination, "checkpoints.tsv") &&
        fixture_fd(destination_fd, "/.jash", 0)) { errno = EACCES; return -1; }
    return next(source_fd, source, destination_fd, destination);
}

int unlinkat(int directory, const char* name, int flags) {
    int (*next)(int, const char*, int); NEXT("unlinkat", next);
    if (!next) { errno = ENOSYS; return -1; }
    int observed = (!strcmp(fault(), "gc_receipt") && !strncmp(name, ".gc-", 4) &&
                    fixture_fd(directory, "/.jash/recovery-v1", 0)) ||
                   (!strcmp(fault(), "trash_gc_receipt") && !flags &&
                    fixture_fd(directory, "/.jash/trash/.jash-trash-gc-", 1));
    int result = next(directory, name, flags);
    if (!result && observed) gc_removed++;
    return result;
}

ssize_t write(int fd, const void* bytes, size_t size) {
    ssize_t (*next)(int, const void*, size_t); NEXT("write", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (fd == STDOUT_FILENO && gc_removed && (!strcmp(fault(), "gc_receipt") || !strcmp(fault(), "trash_gc_receipt"))) {
        errno = EPIPE; return -1;
    }
    return next(fd, bytes, size);
}
