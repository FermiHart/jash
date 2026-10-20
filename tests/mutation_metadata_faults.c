// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>

static int atomic_publications;

static int matches(int fd, const char* kind) {
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    const char* path = getenv("JASH_TEST_MUTATION_PATH");
    if (!fault || !path || strcmp(fault, kind)) return 0;
    char link[64], name[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, name, sizeof(name) - 1);
    if (n < 0) return 0;
    name[n] = '\0';
    return !strcmp(name, path);
}

int futimens(int fd, const struct timespec times[2]) {
    if (matches(fd, "touch_times")) { errno = EIO; return -1; }
    int (*next)(int, const struct timespec[2]) = dlsym(RTLD_NEXT, "futimens");
    return next(fd, times);
}

int fsync(int fd) {
    if (matches(fd, "sed_parent_sync")) { errno = EIO; return -1; }
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (fault && !strcmp(fault, "checkpoint_file_sync")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, "/.jash-checkpoint-")) { errno = EIO; return -1; }
        }
    }
    if (fault && !strcmp(fault, "atomic_commit_sync") && atomic_publications >= 2) { errno = EIO; return -1; }
    int (*next)(int) = dlsym(RTLD_NEXT, "fsync");
    return next(fd);
}

int renameat2(int from, const char* source, int to, const char* target, unsigned flags) {
    int (*next)(int, const char*, int, const char*, unsigned) = dlsym(RTLD_NEXT, "renameat2");
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (fault && !strcmp(fault, "recovery_publish") && !strncmp(source, ".pending-", 9)) { errno = EIO; return -1; }
    int rc = next(from, source, to, target, flags);
    if (!rc && !strcmp(source, target) && (!strcmp(source, "first") || !strcmp(source, "second"))) {
        atomic_publications++;
        if (atomic_publications == 1 && fault) {
            if (!strcmp(fault, "atomic_cancel")) raise(SIGTERM);
            if (!strcmp(fault, "atomic_sink")) close(STDOUT_FILENO);
        }
    }
    return rc;
}

ssize_t write(int fd, const void* bytes, size_t length) {
    ssize_t (*next)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (fd == STDOUT_FILENO && length == 1 && *(const char*)bytes == ' ' &&
        atomic_publications == 1 && fault && !strcmp(fault, "atomic_rollback")) {
        int raced = open("first", O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (raced >= 0) { (void)next(raced, "raced", 5); close(raced); }
    }
    ssize_t rc = next(fd, bytes, length);
    static int paused;
    if (!paused && rc > 0 && fault && !strcmp(fault, "recovery_pause")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, "/recovery-v1/")) {
                paused = 1;
                const char* marker = getenv("JASH_TEST_MUTATION_PATH");
                int ready = marker ? open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600) : -1;
                if (ready >= 0) close(ready);
                raise(SIGSTOP);
            }
        }
    }
    return rc;
}

int unlinkat(int fd, const char* name, int flags) {
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (fault && !strcmp(fault, "recovery_gc_unlink") && !strncmp(name, ".gc-", 4)) { errno = EIO; return -1; }
    if (fault && !strcmp(fault, "atomic_cleanup") && atomic_publications >= 2 && !strcmp(name, "item")) { errno = EIO; return -1; }
    int (*next)(int, const char*, int) = dlsym(RTLD_NEXT, "unlinkat");
    return next(fd, name, flags);
}

int linkat(int from, const char* source, int to, const char* target, int flags) {
    int (*next)(int, const char*, int, const char*, int) = dlsym(RTLD_NEXT, "linkat");
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (fault && !strcmp(fault, "df_temp_swap") && from == AT_FDCWD && !strncmp(source, "/proc/self/fd/", 14)) {
        /* The production name includes a per-call collision sequence. Attack
         * the actual open inode's name, not an obsolete predictable spelling. */
        char opened_path[4096];
        ssize_t size = readlink(source, opened_path, sizeof(opened_path) - 1u);
        if (size >= 0) opened_path[size] = 0;
        const char* temporary = size >= 0 ? strrchr(opened_path, '/') : NULL;
        if (temporary) temporary++;
        if (temporary && !strncmp(temporary, ".jash-df-", 9) && !renameat(to, temporary, to, "owned-snapshot")) {
            int fd = openat(to, temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { (void)write(fd, "raced", 5); close(fd); }
        }
    }
    return next(from, source, to, target, flags);
}

int unlink(const char* path) {
    int (*next)(const char*) = dlsym(RTLD_NEXT, "unlink");
    int rc = next(path);
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (!rc && fault && !strcmp(fault, "cancel_after_first") && !strcmp(path, "first")) raise(SIGTERM);
    if (!rc && fault && !strcmp(fault, "checkpoint_prune_race") && strstr(path, "created")) {
        const char* store = getenv("JASH_TEST_MUTATION_PATH");
        int fd = store ? open(store, O_WRONLY | O_APPEND | O_NOFOLLOW) : -1;
        const char row[] = "concurrent\t0\t123\t/tmp\n";
        if (fd >= 0) { (void)write(fd, row, sizeof(row) - 1); close(fd); }
    }
    return rc;
}

int rename(const char* source, const char* target) {
    int (*next)(const char*, const char*) = dlsym(RTLD_NEXT, "rename");
    int rc = next(source, target);
    const char* fault = getenv("JASH_TEST_MUTATION_FAULT");
    if (!rc && fault && !strcmp(fault, "cancel_after_first") && !strcmp(source, "first")) raise(SIGTERM);
    return rc;
}
