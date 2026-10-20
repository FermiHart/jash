// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Small local-memory stores. Reads never create state; native writers serialize
 * through one private lock and atomically publish complete bounded files. The
 * receipt distinguishes namespace publication from directory durability. */
#ifndef JASH_BUILTINS_MEMORY_STORE_H
#define JASH_BUILTINS_MEMORY_STORE_H

#include "semantic_builtin.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MEMORY_STORE_MAX_BYTES (512u * 1024u)

typedef struct { int directory; int lock; } memory_store_t;
typedef struct { int applied; int durable; int error; } memory_write_t;

static inline int memory_store_trusted(int descriptor, int directory) {
    struct stat st;
    if (fstat(descriptor, &st)) return -1;
    if ((directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) || (!directory && st.st_nlink != 1)) {
        errno = EPERM; return -1;
    }
    return 0;
}

static inline int memory_store_close(memory_store_t* store) {
    int failed = 0;
    if (store->lock >= 0) {
        struct flock lock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
        if (fcntl(store->lock, F_SETLK, &lock)) failed = errno;
        if (close(store->lock) && !failed) failed = errno;
        store->lock = -1;
    }
    if (store->directory >= 0) {
        if (close(store->directory) && !failed) failed = errno;
        store->directory = -1;
    }
    if (failed) { errno = failed; return -1; }
    return 0;
}

static inline int memory_store_child(int parent, const char* name, int create) {
    int made = 0;
    if (create) {
        if (!mkdirat(parent, name, 0700)) made = 1;
        else if (errno != EEXIST) return -1;
    }
    int child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) return -1;
    if (memory_store_trusted(child, 1) || (made && fsync(parent))) {
        int saved = errno; close(child); errno = saved; return -1;
    }
    return child;
}

/* Missing read-only directory is a successful empty store (directory == -1).
 * Callers must admit their output before create=1. The lock protects native
 * writers; older unlocked writers are detected by stable file observations. */
static inline int memory_store_open_controlled(memory_store_t* store, const char* child, int create,
                                               int (*poll)(void*), void* context) {
    *store = (memory_store_t){-1, -1};
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') { errno = EINVAL; return -1; }
    int homefd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (homefd < 0) return -1;
    if (memory_store_trusted(homefd, 1)) { int saved = errno; close(homefd); errno = saved; return -1; }
    int directory = memory_store_child(homefd, ".jash", create);
    int failed = directory < 0 ? errno : 0;
    if (close(homefd) && !failed) failed = errno;
    if (failed) {
        if (directory >= 0) close(directory);
        if (!create && failed == ENOENT) return 0;
        errno = failed; return -1;
    }
    if (child && *child) {
        int nested = memory_store_child(directory, child, create);
        failed = nested < 0 ? errno : 0;
        if (close(directory) && !failed) failed = errno;
        if (failed) {
            if (nested >= 0) close(nested);
            if (!create && failed == ENOENT) return 0;
            errno = failed; return -1;
        }
        directory = nested;
    }
    store->directory = directory;
    if (!create) return 0;
    store->lock = openat(directory, ".memory-lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (store->lock < 0 || memory_store_trusted(store->lock, 0)) goto fail;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (!poll) {
        while (fcntl(store->lock, F_SETLKW, &lock)) { if (errno != EINTR) goto fail; }
    } else {
        for (;;) {
            if (poll(context)) { errno = ECANCELED; goto fail; }
            if (!fcntl(store->lock, F_SETLK, &lock)) break;
            if (errno == EINTR) continue;
            if (errno != EACCES && errno != EAGAIN) goto fail;
            struct timespec pause = {0, 10000000};
            (void)nanosleep(&pause, NULL);
        }
    }
    return 0;
fail:
    failed = errno; (void)memory_store_close(store); errno = failed; return -1;
}

static inline int memory_store_open(memory_store_t* store, const char* child, int create) {
    return memory_store_open_controlled(store, child, create, NULL, NULL);
}

/* The callback must retain its signal-specific status in context when needed.
 * Cancellation is returned as ECANCELED; it never unlocks or aborts the writer
 * that currently owns the store. Directory creation still requires admission. */
static inline int memory_store_open_cooperative(memory_store_t* store, const char* child, int create,
                                                int (*poll)(void*), void* context) {
    if (!poll) { errno = EINVAL; return -1; }
    return memory_store_open_controlled(store, child, create, poll, context);
}

static inline int memory_store_same(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_mode == b->st_mode && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime
#if defined(__APPLE__)
           && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec
#else
           && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec
#endif
           ;
}

static inline int memory_store_read(memory_store_t* store, const char* name, size_t limit,
                                     char** bytes, size_t* length, int* exists) {
    *bytes = NULL; *length = 0; *exists = 0;
    if (store->directory < 0) return 0;
    int fd = openat(store->directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    *exists = 1;
    struct stat before, after; int failed = 0;
    if (memory_store_trusted(fd, 0) || fstat(fd, &before)) failed = errno;
    if (!failed && (before.st_size < 0 || (uint64_t)before.st_size > limit)) failed = E2BIG;
    size_t size = failed ? 0 : (size_t)before.st_size;
    char* data = failed ? NULL : malloc(size + 1);
    if (!failed && !data) failed = ENOMEM;
    size_t used = 0;
    while (!failed && used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { failed = n ? errno : ESTALE; break; }
        used += (size_t)n;
    }
    if (!failed) {
        char extra; ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n) failed = n < 0 ? errno : ESTALE;
        else if (fstat(fd, &after)) failed = errno;
        else if (!memory_store_same(&before, &after)) failed = ESTALE;
    }
    if (close(fd) && !failed) failed = errno;
    if (failed) { free(data); errno = failed; return -1; }
    data[size] = 0; *bytes = data; *length = size; return 0;
}

static inline int memory_store_write_all(int fd, const void* data, size_t length) {
    const char* bytes = data;
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        bytes += n; length -= (size_t)n;
    }
    return 0;
}

static inline memory_write_t memory_store_publish(memory_store_t* store, const char* name,
                                                   const void* bytes, size_t length) {
    memory_write_t result = {0};
    if (store->directory < 0 || store->lock < 0 || length > MEMORY_STORE_MAX_BYTES) {
        result.error = length > MEMORY_STORE_MAX_BYTES ? E2BIG : EINVAL; return result;
    }
    char temporary[80]; int fd = -1;
    static unsigned serial;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        snprintf(temporary, sizeof(temporary), ".memory-%ld-%u.tmp", (long)getpid(), ++serial);
        fd = openat(store->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { result.error = errno; return result; }
    if (memory_store_write_all(fd, bytes, length) || fsync(fd)) result.error = errno;
    if (close(fd) && !result.error) result.error = errno;
    if (!result.error && renameat(store->directory, temporary, store->directory, name)) result.error = errno;
    else if (!result.error) {
        result.applied = 1;
        if (fsync(store->directory)) result.error = errno;
        else result.durable = 1;
    }
    if (!result.applied) (void)unlinkat(store->directory, temporary, 0);
    return result;
}

static inline memory_write_t memory_store_remove(memory_store_t* store, const char* name) {
    memory_write_t result = {0};
    if (store->directory < 0 || store->lock < 0) { result.error = EINVAL; return result; }
    if (unlinkat(store->directory, name, 0)) {
        if (errno != ENOENT) result.error = errno;
        else result.durable = 1;
        return result;
    }
    result.applied = 1;
    if (fsync(store->directory)) result.error = errno;
    else result.durable = 1;
    return result;
}

static inline const char* memory_store_error(int error) {
    return error == E2BIG ? "store_limit" : error == EINVAL ? "invalid_store" :
           error == ESTALE ? "store_changed" : "store_io_failed";
}

static inline int memory_unsigned(const char* text, uint64_t maximum, uint64_t* result) {
    if (!text || !*text) return -1;
    uint64_t value = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p < '0' || *p > '9' || value > maximum / 10 ||
            (value == maximum / 10 && (uint64_t)(*p - '0') > maximum % 10)) return -1;
        value = value * 10 + (uint64_t)(*p - '0');
    }
    *result = value; return 0;
}
#endif
