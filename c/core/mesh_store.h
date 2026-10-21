// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked local mesh storage. Native observations never create a store and
 * publication reports separately whether rename applied and fsync completed. */
#ifndef JASH_CORE_MESH_STORE_H
#define JASH_CORE_MESH_STORE_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int home, base, root, workers, tasks, lock;
    unsigned created;
    int durable;
} jash_mesh_store_t;

typedef struct {
    int applied, durable, verified, error;
    uint64_t bytes_written;
} jash_mesh_io_t;

static inline int jash_mesh_private_fd(int fd, int directory) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    if ((directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) || (!directory && st.st_nlink != 1)) {
        errno = EINVAL; return -1;
    }
    return 0;
}

static inline int jash_mesh_directory(int parent, const char* name, int create,
                                      unsigned* created, int* durable) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        if (mkdirat(parent, name, 0700)) {
            if (errno != EEXIST) return -1;
        } else {
            (*created)++;
            if (fsync(parent)) { *durable = 0; return -1; }
        }
        fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) return -1;
    if (jash_mesh_private_fd(fd, 1)) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}

/* 0 opened; 1 absent in a read-only operation; -1 an explicit source error.
 * On failure the caller still closes the partially opened store. */
static inline int jash_mesh_store_open_poll(jash_mesh_store_t* store, int create,
                                            int (*poll)(void*), void* context) {
    *store = (jash_mesh_store_t){.home=-1, .base=-1, .root=-1,
        .workers=-1, .tasks=-1, .lock=-1, .durable=1};
    const char* home = getenv("HOME");
    if (!home || home[0] != '/' || strlen(home) >= PATH_MAX) { errno = EINVAL; return -1; }
    store->home = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (store->home < 0) return -1;
    store->base = jash_mesh_directory(store->home, ".jash", create, &store->created, &store->durable);
    if (store->base < 0) return !create && errno == ENOENT ? 1 : -1;
    store->root = jash_mesh_directory(store->base, "mesh", create, &store->created, &store->durable);
    if (store->root < 0) return !create && errno == ENOENT ? 1 : -1;
    store->workers = jash_mesh_directory(store->root, "workers", create, &store->created, &store->durable);
    if (store->workers < 0 && (create || errno != ENOENT)) return -1;
    store->tasks = jash_mesh_directory(store->root, "tasks", create, &store->created, &store->durable);
    if (store->tasks < 0 && (create || errno != ENOENT)) return -1;
    store->lock = openat(store->root, ".lock", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (store->lock < 0 && errno == ENOENT && create) {
        store->lock = openat(store->root, ".lock", O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (store->lock >= 0) {
            store->created++;
            if (fsync(store->lock) || fsync(store->root)) { store->durable = 0; return -1; }
        } else if (errno == EEXIST)
            store->lock = openat(store->root, ".lock", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    }
    if (store->lock < 0) {
        /* Legacy registration did not create a lock. Per-file observations
         * still verify their inode and timestamps before and after reading. */
        if (!create && errno == ENOENT) return 0;
        return -1;
    }
    if (jash_mesh_private_fd(store->lock, 0)) return -1;
    if (!poll) {
        int rc;
        do { rc = flock(store->lock, create ? LOCK_EX : LOCK_SH); } while (rc && errno == EINTR);
        return rc;
    }
    for (;;) {
        if (poll(context)) { errno = ECANCELED; return -1; }
        if (!flock(store->lock, (create ? LOCK_EX : LOCK_SH) | LOCK_NB)) return 0;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        struct timespec pause = {.tv_nsec=20000000};
        while (nanosleep(&pause, &pause) && errno == EINTR)
            if (poll(context)) { errno = ECANCELED; return -1; }
    }
}

static inline int jash_mesh_store_open(jash_mesh_store_t* store, int create) {
    return jash_mesh_store_open_poll(store, create, NULL, NULL);
}

static inline int jash_mesh_store_close(jash_mesh_store_t* store) {
    int error = 0;
    int* fds[] = {&store->lock, &store->tasks, &store->workers, &store->root, &store->base, &store->home};
    for (size_t i = 0; i < sizeof fds / sizeof *fds; i++) if (*fds[i] >= 0) {
        if (close(*fds[i]) && !error) error = errno;
        *fds[i] = -1;
    }
    if (error) { errno = error; return -1; }
    return 0;
}

static inline int jash_mesh_same_file(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_size == right->st_size && left->st_mode == right->st_mode &&
#ifdef __APPLE__
        left->st_mtimespec.tv_sec == right->st_mtimespec.tv_sec &&
        left->st_mtimespec.tv_nsec == right->st_mtimespec.tv_nsec &&
        left->st_ctimespec.tv_sec == right->st_ctimespec.tv_sec &&
        left->st_ctimespec.tv_nsec == right->st_ctimespec.tv_nsec;
#else
        left->st_mtim.tv_sec == right->st_mtim.tv_sec && left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
        left->st_ctim.tv_sec == right->st_ctim.tv_sec && left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
#endif
}

/* One complete immutable regular-file observation. No short successful reads,
 * no FIFO blocking, no symlink traversal and no prefix mistaken for a file. */
static inline int jash_mesh_read_at(int directory, const char* name, size_t limit,
                                    char** bytes, size_t* length) {
    *bytes = NULL; *length = 0;
    if (directory < 0) return 0;
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat before, after, named;
    int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_uid != geteuid() || (before.st_mode & 0022) || before.st_nlink != 1)
        error = EINVAL;
    else if (before.st_size < 0 || (uintmax_t)before.st_size > limit) error = E2BIG;
    char* data = error ? NULL : malloc((size_t)before.st_size + 1u);
    if (!error && !data) error = ENOMEM;
    size_t used = 0;
    while (!error && used < (size_t)before.st_size) {
        ssize_t n = read(fd, data + used, (size_t)before.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n ? errno : EIO; break; }
        used += (size_t)n;
    }
    if (!error) {
        unsigned char extra;
        ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n < 0) error = errno;
        else if (n || fstat(fd, &after) || fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW)) error = n ? ESTALE : errno;
        else if (!jash_mesh_same_file(&before, &after) || !jash_mesh_same_file(&after, &named)) error = ESTALE;
    }
    if (close(fd) && !error) error = errno;
    if (error) { free(data); errno = error; return -1; }
    data[used] = 0; *bytes = data; *length = used; return 1;
}

static inline void jash_mesh_cleanup_owned(int directory, const char* name, const struct stat* owned) {
    struct stat named;
    if (!fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) &&
        named.st_dev == owned->st_dev && named.st_ino == owned->st_ino)
        (void)unlinkat(directory, name, 0);
}

static inline int jash_mesh_write_at(int directory, const char* name,
                                      const void* bytes, size_t length, jash_mesh_io_t* result) {
    memset(result, 0, sizeof *result);
    struct stat prior;
    if (!fstatat(directory, name, &prior, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(prior.st_mode) || prior.st_uid != geteuid() || prior.st_nlink != 1 || (prior.st_mode & 0022)) {
            result->error = EINVAL; errno = EINVAL; return -1;
        }
    } else if (errno != ENOENT) { result->error = errno; return -1; }
    static unsigned long sequence;
    char temporary[96]; int fd = -1;
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        int n = snprintf(temporary, sizeof temporary, ".mesh.tmp.%ld.%lu", (long)getpid(), ++sequence);
        if (n < 0 || (size_t)n >= sizeof temporary) { errno = ENAMETOOLONG; break; }
        fd = openat(directory, temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { result->error = errno; return -1; }
    struct stat owned = {0}, named;
    int error = fstat(fd, &owned) ? errno : 0;
    size_t used = 0;
    while (!error && used < length) {
        ssize_t n = write(fd, (const char*)bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n ? errno : EIO; break; }
        used += (size_t)n;
    }
    if (!error && fsync(fd)) error = errno;
    if (!error && fstat(fd, &owned)) error = errno;
    if (!error && (owned.st_size < 0 || (uintmax_t)owned.st_size != length)) error = EIO;
    if (!error && fstatat(directory, temporary, &named, AT_SYMLINK_NOFOLLOW)) error = errno;
    if (!error && (named.st_dev != owned.st_dev || named.st_ino != owned.st_ino)) error = ESTALE;
    if (!error && renameat(directory, temporary, directory, name)) error = errno;
    else if (!error) {
        result->applied = 1; result->bytes_written = length;
        if (fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW)) error = errno;
        else if (named.st_dev != owned.st_dev || named.st_ino != owned.st_ino || named.st_size != owned.st_size) error = ESTALE;
        else result->verified = 1;
        if (fsync(directory)) { if (!error) error = errno; }
        else if (result->verified) result->durable = 1;
    }
    if (close(fd) && !error) error = errno;
    if (!result->applied) jash_mesh_cleanup_owned(directory, temporary, &owned);
    result->error = error;
    if (error) { errno = error; return -1; }
    return 0;
}

static inline int jash_mesh_remove_at(int directory, const char* name, jash_mesh_io_t* result) {
    memset(result, 0, sizeof *result);
    struct stat st;
    if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT) { result->verified = 1; return 0; }
        result->error = errno; return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1) { errno = EINVAL; result->error = errno; return -1; }
    if (unlinkat(directory, name, 0)) { result->error = errno; return -1; }
    result->applied = result->verified = 1;
    if (fsync(directory)) { result->error = errno; return -1; }
    result->durable = 1; return 0;
}

/* Encoded records preserve field positions used by legacy tooling. A final
 * @jash-mesh-v2 field opts into escaping, so legacy literal backslashes remain
 * literal. Values may include every non-NUL argv byte, including TAB/newline. */
static inline int jash_mesh_escape(char* output, size_t capacity, size_t* used, const char* input) {
    for (const unsigned char* p = (const unsigned char*)input; *p; p++) {
        const char* escaped = *p == '\\' ? "\\\\" : *p == '\t' ? "\\t" : *p == '\n' ? "\\n" : *p == '\r' ? "\\r" : NULL;
        size_t n = escaped ? 2u : 1u;
        if (*used >= capacity || n >= capacity - *used) { errno = E2BIG; return -1; }
        if (escaped) memcpy(output + *used, escaped, n);
        else output[*used] = (char)*p;
        *used += n;
    }
    output[*used] = 0; return 0;
}

static inline int jash_mesh_unescape(const char* input, char* output, size_t capacity, int encoded) {
    size_t used = 0;
    for (const char* p = input; *p; p++) {
        char byte = *p;
        if (encoded && byte == '\\') {
            switch (*++p) {
                case '\\': byte = '\\'; break;
                case 't': byte = '\t'; break;
                case 'n': byte = '\n'; break;
                case 'r': byte = '\r'; break;
                default: errno = EINVAL; return -1;
            }
        }
        if (used + 1 >= capacity) { errno = E2BIG; return -1; }
        output[used++] = byte;
    }
    output[used] = 0; return 0;
}

static inline int jash_mesh_number(const char* text, uint64_t maximum, uint64_t* number) {
    if (!text || !*text) return -1;
    uint64_t value = 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return -1;
        value = value * 10 + digit;
    }
    *number = value; return 0;
}

static inline int jash_mesh_fields(char* line, char** fields, size_t capacity) {
    size_t count = 0;
    fields[count++] = line;
    for (char* p = line; *p; p++) if (*p == '\t') {
        if (count == capacity) return -1;
        *p = 0; fields[count++] = p + 1;
    }
    return (int)count;
}
#endif
