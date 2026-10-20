/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int selected(const char* name) {
    const char* mode = getenv("JASH_TEST_RECIPE_FAULT"); return mode && !strcmp(mode, name);
}
static int descriptor(int fd, const char* suffix) {
    const char* home = getenv("HOME"); if (!home) return 0;
    char link[64], path[PATH_MAX]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(link, path, sizeof(path) - 1u);
    if (size < 0 || (size_t)size == sizeof(path) - 1u) return 0;
    path[size] = 0;
    return !strncmp(path, home, strlen(home)) && !strcmp(path + strlen(home), suffix);
}
#define NEXT(symbol, value) do { void* address = dlsym(RTLD_NEXT, symbol); memcpy(&(value), &address, sizeof(value)); } while (0)
static int recipe_published, checkpoint_published;
int renameat(int source_fd, const char* source, int destination_fd, const char* destination) {
    int (*next)(int, const char*, int, const char*); NEXT("renameat", next);
    int recipe = descriptor(destination_fd, "/.jash/recipes") && strstr(destination, ".jashrc");
    int checkpoint = descriptor(destination_fd, "/.jash") && !strncmp(destination, "checkpoints", 11);
    if ((recipe && selected("rename-recipe")) || (checkpoint && selected("rename-checkpoint"))) { errno = EIO; return -1; }
    int result = next(source_fd, source, destination_fd, destination);
    if (!result && recipe) recipe_published = 1;
    if (!result && checkpoint) checkpoint_published = 1;
    return result;
}
int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if ((selected("sync-recipe") && recipe_published && descriptor(fd, "/.jash/recipes")) ||
        (selected("sync-checkpoint") && checkpoint_published && descriptor(fd, "/.jash"))) { errno = EIO; return -1; }
    return next(fd);
}
ssize_t write(int fd, const void* bytes, size_t size) {
    ssize_t (*next)(int, const void*, size_t); NEXT("write", next);
    const char* marker = getenv("JASH_TEST_RECIPE_FIRST_EFFECT");
    if (fd == STDOUT_FILENO && selected("sink-after-first") && marker && !access(marker, F_OK)) { errno = EPIPE; return -1; }
    return next(fd, bytes, size);
}
