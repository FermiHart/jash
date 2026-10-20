// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

static int reached;
static void barrier(const char* point) {
    const char* configured = getenv("JASH_TEST_QUERY_POINT");
    if (reached || !configured || strcmp(configured, point)) return;
    const char* ready = getenv("JASH_TEST_QUERY_READY");
    const char* release = getenv("JASH_TEST_QUERY_RELEASE");
    if (!ready || !release) return;
    reached = 1;
    char byte = '1';
    if (write(atoi(ready), &byte, 1) != 1) return;
    while (read(atoi(release), &byte, 1) < 0 && errno == EINTR) {}
}

static int input_descriptor(int fd) {
    char descriptor[64], path[4096];
    int size = snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
    if (size < 0 || (size_t)size >= sizeof(descriptor)) return 0;
    ssize_t count = readlink(descriptor, path, sizeof(path) - 1u);
    if (count < 0) return 0;
    path[count] = 0;
    const char* base = strrchr(path, '/');
    return base && !strcmp(base + 1, "cancel-file");
}

int flock(int fd, int operation) {
    int (*next)(int, int);
    void* symbol=dlsym(RTLD_NEXT,"flock");
    _Static_assert(sizeof(next)==sizeof(symbol),"POSIX dlsym pointer size");
    memcpy(&next,&symbol,sizeof(next));
    if(!next) { errno=ENOSYS; return -1; }
    char descriptor[64],path[4096];
    int n=snprintf(descriptor,sizeof descriptor,"/proc/self/fd/%d",fd);
    ssize_t length=n>0&&(size_t)n<sizeof descriptor?readlink(descriptor,path,sizeof path-1):-1;
    if(length>0) {
        path[length]=0; static const char ending[]="/mesh/.lock";
        if((size_t)length>=sizeof ending-1&&!memcmp(path+length-(sizeof ending-1),ending,sizeof ending-1)) barrier("mesh-lock");
    }
    return next(fd,operation);
}

#define NEXT(name, function) do { \
    void* symbol = dlsym(RTLD_NEXT, (name)); \
    _Static_assert(sizeof(function) == sizeof(symbol), "POSIX dlsym pointer size"); \
    memcpy(&(function), &symbol, sizeof(function)); \
} while (0)

DIR* opendir(const char* path) {
    DIR* (*next)(const char*); NEXT("opendir", next);
    if (!next) { errno = ENOSYS; return NULL; }
    const char* base = strrchr(path, '/');
    if (!strcmp(base ? base + 1 : path, "cancel-root")) barrier("directory");
    return next(path);
}

int lstat(const char* path, struct stat* result) {
    int (*next)(const char*, struct stat*); NEXT("lstat", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (!strcmp(path, "cancel-file")) barrier("metadata");
    return next(path, result);
}

ssize_t pread(int fd, void* bytes, size_t count, off_t offset) {
    ssize_t (*next)(int, void*, size_t, off_t); NEXT("pread", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (input_descriptor(fd)) barrier("page");
    return next(fd, bytes, count, offset);
}

ssize_t read(int fd, void* bytes, size_t count) {
    ssize_t (*next)(int, void*, size_t); NEXT("read", next);
    if (!next) { errno = ENOSYS; return -1; }
    if (input_descriptor(fd)) barrier("count");
    return next(fd, bytes, count);
}

size_t fread(void* bytes, size_t size, size_t count, FILE* file) {
    size_t (*next)(void*, size_t, size_t, FILE*); NEXT("fread", next);
    if (!next) { errno = ENOSYS; return 0; }
    if (input_descriptor(fileno(file))) barrier("count");
    return next(bytes, size, count, file);
}

FILE* fopen(const char* path, const char* mode) {
    FILE* (*next)(const char*, const char*); NEXT("fopen", next);
    if (!next) { errno = ENOSYS; return NULL; }
    if (!strcmp(path, "/proc/mounts")) barrier("mounts");
    return next(path, mode);
}
