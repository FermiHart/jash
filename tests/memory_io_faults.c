/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Linux-only faults scoped to this test's private memory store. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NEXT(name, function) do { \
    void* symbol = dlsym(RTLD_NEXT, (name)); \
    _Static_assert(sizeof(function) == sizeof(symbol), "POSIX dlsym pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
} while (0)

static const char* fault(void) {
    const char* mode = getenv("JASH_TEST_MEMORY_FAULT"); return mode ? mode : "";
}
static const char* fixture_file(void) {
    const char* file = getenv("JASH_TEST_MEMORY_FILE");
    return file && *file && !strchr(file, '/') ? file : "kv.db";
}
static int fixture_fd(int fd, const char* suffix, int prefix) {
    const char* root = getenv("JASH_TEST_MEMORY_ROOT");
    if (!root) return 0;
    char descriptor[64], path[PATH_MAX];
    snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    ssize_t length = readlink(descriptor, path, sizeof(path) - 1);
    if (length < 0 || (size_t)length == sizeof(path) - 1) return 0;
    path[length] = 0; size_t size = strlen(root);
    if (strncmp(path, root, size)) return 0;
    return prefix ? !strncmp(path + size, suffix, strlen(suffix)) : !strcmp(path + size, suffix);
}
static int published;

int renameat(int source_fd, const char* source, int destination_fd, const char* destination) {
    int (*next)(int, const char*, int, const char*); NEXT("renameat", next);
    if (!next) { errno = ENOSYS; return -1; }
    int selected = fixture_fd(destination_fd, "/.jash", 0) && !strcmp(destination, fixture_file());
    if (selected && !strcmp(fault(), "rename")) { errno = EIO; return -1; }
    int result = next(source_fd, source, destination_fd, destination);
    if (!result && selected) published = 1;
    return result;
}
int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if (!next) { errno = ENOSYS; return -1; }
    if ((!strcmp(fault(), "file_sync") && fixture_fd(fd, "/.jash/.memory-", 1)) ||
        (!strcmp(fault(), "directory_sync") && published && fixture_fd(fd, "/.jash", 0))) {
        errno = EIO; return -1;
    }
    return next(fd);
}
int close(int fd) {
    int (*next)(int); NEXT("close", next);
    if (!next) { errno = ENOSYS; return -1; }
    int fail = !strcmp(fault(), "file_close") && fixture_fd(fd, "/.jash/.memory-", 1);
    int result = next(fd);
    if (!result && fail) { errno = EIO; return -1; }
    return result;
}
ssize_t read(int fd, void* bytes, size_t length) {
    ssize_t (*next)(int, void*, size_t); NEXT("read", next);
    if (!next) { errno = ENOSYS; return -1; }
    char suffix[PATH_MAX];
    int size = snprintf(suffix, sizeof(suffix), "/.jash/%s", fixture_file());
    if (!strcmp(fault(), "read") && size > 0 && (size_t)size < sizeof(suffix) && fixture_fd(fd, suffix, 0)) {
        errno = EIO; return -1;
    }
    return next(fd, bytes, length);
}
