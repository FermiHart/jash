/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Linux-only deterministic faults, confined to a caller-owned fixture root. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* fault(void) {
    const char* value = getenv("JASH_TEST_MANAGEMENT_FAULT");
    return value ? value : "";
}

static int fixture_path(const char* path, const char* suffix, int prefix) {
    const char* root = getenv("JASH_TEST_MANAGEMENT_ROOT");
    if (!root || !path) return 0;
    size_t length = strlen(root);
    if (strncmp(path, root, length)) return 0;
    return prefix ? !strncmp(path + length, suffix, strlen(suffix)) : !strcmp(path + length, suffix);
}

static int fixture_fd(int fd, const char* suffix, int prefix) {
    char source[64], path[PATH_MAX];
    snprintf(source, sizeof source, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(source, path, sizeof path - 1u);
    if (n < 0 || (size_t)n == sizeof path - 1u) return 0;
    path[n] = 0;
    return fixture_path(path, suffix, prefix);
}

int fsync(int fd) {
    int (*real_fsync)(int);
    *(void**)(&real_fsync) = dlsym(RTLD_NEXT, "fsync");
    const char* mode = fault();
    if ((!strcmp(mode, "state_file_fsync") && fixture_fd(fd, "/.jash_state.tmp.", 1)) ||
        (!strcmp(mode, "state_dir_fsync") && fixture_fd(fd, "", 0)) ||
        (!strcmp(mode, "config_file_fsync") && fixture_fd(fd, "/.config/jash/groq.key.tmp.", 1)) ||
        (!strcmp(mode, "config_dir_fsync") && fixture_fd(fd, "/.config/jash", 0))) {
        errno = EIO; return -1;
    }
    return real_fsync(fd);
}

int rename(const char* source, const char* destination) {
    int (*real_rename)(const char*, const char*);
    *(void**)(&real_rename) = dlsym(RTLD_NEXT, "rename");
    const char* mode = fault();
    if ((!strcmp(mode, "state_rename") && fixture_path(destination, "/.jash_state", 0)) ||
        (!strcmp(mode, "config_rename") && fixture_path(destination, "/.config/jash/groq.key", 0))) {
        errno = EACCES; return -1;
    }
    return real_rename(source, destination);
}

int unlink(const char* path) {
    int (*real_unlink)(const char*);
    *(void**)(&real_unlink) = dlsym(RTLD_NEXT, "unlink");
    if (!strcmp(fault(), "state_unlink") && fixture_path(path, "/.jash_state", 0)) {
        errno = EACCES; return -1;
    }
    return real_unlink(path);
}

ssize_t read(int fd, void* buffer, size_t bytes) {
    ssize_t (*real_read)(int, void*, size_t);
    *(void**)(&real_read) = dlsym(RTLD_NEXT, "read");
    if (!strcmp(fault(), "state_read") && fixture_fd(fd, "/.jash_state", 0)) {
        errno = EIO; return -1;
    }
    return real_read(fd, buffer, bytes);
}

int close(int fd) {
    int (*real_close)(int);
    *(void**)(&real_close) = dlsym(RTLD_NEXT, "close");
    int injected = !strcmp(fault(), "state_close") && fixture_fd(fd, "/.jash_state.tmp.", 1);
    int rc = real_close(fd);
    if (!rc && injected) { errno = EIO; return -1; }
    return rc;
}
