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
    const char* mode = getenv("JASH_TEST_GAP_FAULT"); return mode && !strcmp(mode, name);
}
static int descriptor(int fd, const char* suffix) {
    const char* home = getenv("HOME"); if (!home) return 0;
    char link[64], path[PATH_MAX]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t count = readlink(link, path, sizeof(path) - 1u);
    if (count < 0 || (size_t)count == sizeof(path) - 1u) return 0;
    path[count] = 0; size_t size = strlen(home);
    return !strncmp(path, home, size) && !strcmp(path + size, suffix);
}
#define NEXT(symbol, value) do { void* address = dlsym(RTLD_NEXT, symbol); memcpy(&(value), &address, sizeof(value)); } while (0)
static int local_published;

int renameat(int source_fd, const char* source, int destination_fd, const char* destination) {
    int (*next)(int, const char*, int, const char*); NEXT("renameat", next);
    int local = !strcmp(destination, "gaps.tsv") && descriptor(destination_fd, "/.jash");
    int fleet = !strcmp(destination, "gaps.tsv") && descriptor(destination_fd, "/.jash/mesh");
    if ((local && selected("rename-local")) || (fleet && selected("rename-fleet"))) { errno = EIO; return -1; }
    int result = next(source_fd, source, destination_fd, destination);
    if (!result && local) local_published = 1;
    return result;
}
int fsync(int fd) {
    int (*next)(int); NEXT("fsync", next);
    if (selected("directory-sync-local") && local_published && descriptor(fd, "/.jash")) { errno = EIO; return -1; }
    return next(fd);
}
ssize_t write(int fd, const void* bytes, size_t size) {
    ssize_t (*next)(int, const void*, size_t); NEXT("write", next);
    const char* marker = getenv("JASH_TEST_GAP_FIRST_PROOF");
    if (fd == STDOUT_FILENO && selected("sink-after-proof") && marker && !access(marker, F_OK)) { errno = EPIPE; return -1; }
    return next(fd, bytes, size);
}
