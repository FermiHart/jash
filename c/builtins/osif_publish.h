// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
/* Shared publication engine for the report and scaffold adapters. Receipts
 * describe the named target; temporary cleanup is a separate observation.
 * A checked observer failure stops the engine before the next effect. */
#ifndef JASH_BUILTINS_OSIF_PUBLISH_H
#define JASH_BUILTINS_OSIF_PUBLISH_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OSIF_PUBLISH_PATH 1024u
typedef struct {
    const char* phase;
    const char* action;
    const char* path;
    const char* error;
    int error_number;
    int applied;
    int durable;                 /* -1: pre-existing target, not established */
    uint64_t expected_bytes;
    uint64_t written_bytes;
    int existing_size_known;
    uint64_t existing_bytes;
    int temporary_created;
    const char* temporary_path;   /* non-NULL only when cleanup failed */
    int cleanup_error;
} osif_publish_receipt_t;
typedef int (*osif_publish_observer_t)(void*, const osif_publish_receipt_t*);
typedef struct {
    int descriptors[4];
    char paths[4][OSIF_PUBLISH_PATH];
    unsigned depth;
    osif_publish_observer_t observe;
    void* context;
    int delivery_failed;
    unsigned applied;
    unsigned durable;
    unsigned preserved;
} osif_publish_store_t;

static inline int osif_publish_observe(osif_publish_store_t* store,
                                       const osif_publish_receipt_t* receipt) {
    if (receipt->applied) store->applied++;
    if (receipt->durable == 1) store->durable++;
    if (!strcmp(receipt->action, "preserved")) store->preserved++;
    if (store->delivery_failed) return 125;
    if (store->observe && store->observe(store->context, receipt)) {
        store->delivery_failed = 1; return 125;
    }
    return receipt->error ? 1 : 0;
}

static inline int osif_publish_error(osif_publish_store_t* store, const char* phase,
                                     const char* path, const char* error, int number) {
    osif_publish_receipt_t receipt = {.phase = phase, .action = "failed", .path = path,
        .error = error, .error_number = number, .durable = -1};
    return osif_publish_observe(store, &receipt);
}

static inline int osif_publish_open(osif_publish_store_t* store,
                                    osif_publish_observer_t observe, void* context) {
    memset(store, 0, sizeof *store); store->observe = observe; store->context = context;
    int fd = open(".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return osif_publish_error(store, "open-directory", ".", "directory_open_failed", errno);
    store->descriptors[0] = fd; if (jash_copy_cstr(store->paths[0], sizeof(store->paths[0]), ".")) { int saved = errno; close(fd); store->descriptors[0] = -1; return osif_publish_error(store, "open-directory", ".", "path_limit", saved); } store->depth = 1;
    return 0;
}

static inline int osif_publish_path(const osif_publish_store_t* store, const char* name,
                                    char path[OSIF_PUBLISH_PATH]) {
    if (!store->depth || !name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL; return -1;
    }
    const char* parent = store->paths[store->depth - 1];
    int n = !strcmp(parent, ".") ? snprintf(path, OSIF_PUBLISH_PATH, "%s", name) :
        snprintf(path, OSIF_PUBLISH_PATH, "%s/%s", parent, name);
    if (n < 0 || (size_t)n >= OSIF_PUBLISH_PATH) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static inline int osif_publish_enter(osif_publish_store_t* store, const char* name, int create) {
    if (store->delivery_failed) return 125;
    char path[OSIF_PUBLISH_PATH];
    if (store->depth == 4 || osif_publish_path(store, name, path))
        return osif_publish_error(store, "open-directory", name, "invalid_directory", EINVAL);
    int parent = store->descriptors[store->depth - 1], created = 0;
    if (create) {
        if (!mkdirat(parent, name, 0755)) created = 1;
        else if (errno != EEXIST)
            return osif_publish_error(store, "mkdir", path, "directory_create_failed", errno);
    }
    int fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    osif_publish_receipt_t receipt = {.phase = created ? "mkdir" : "open-directory",
        .action = created ? "created" : "failed", .path = path, .applied = created,
        .durable = created ? 0 : -1};
    if (fd < 0) {
        receipt.error = "directory_open_failed"; receipt.error_number = errno;
        return osif_publish_observe(store, &receipt);
    }
    store->descriptors[store->depth] = fd;
    memcpy(store->paths[store->depth], path, strlen(path) + 1); store->depth++;
    if (!created) return 0;
    if (fsync(fd) || fsync(parent)) {
        receipt.error = "directory_sync_failed"; receipt.error_number = errno;
    } else receipt.durable = 1;
    return osif_publish_observe(store, &receipt);
}

static inline int osif_publish_close(osif_publish_store_t* store, int status) {
    while (store->depth) {
        unsigned index = --store->depth;
        if (close(store->descriptors[index])) {
            int rc = osif_publish_error(store, "close-directory", store->paths[index], "directory_close_failed", errno);
            if (!status || rc == 125) status = rc;
        }
    }
    return store->delivery_failed ? 125 : status;
}

static inline int osif_publish_file(osif_publish_store_t* store, const char* name,
                                    const void* bytes, size_t size, int replace) {
    if (store->delivery_failed) return 125;
    char path[OSIF_PUBLISH_PATH], temporary_path[OSIF_PUBLISH_PATH];
    if (osif_publish_path(store, name, path))
        return osif_publish_error(store, "publish", name, "invalid_path", errno);
    int parent = store->descriptors[store->depth - 1];
    osif_publish_receipt_t receipt = {.phase = "publish", .action = "failed", .path = path,
        .expected_bytes = size, .durable = 0};
    struct stat existing;
    int found = fstatat(parent, name, &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (!found && errno != ENOENT) {
        receipt.error = "target_stat_failed"; receipt.error_number = errno;
        return osif_publish_observe(store, &receipt);
    }
    if (found && !S_ISREG(existing.st_mode)) {
        receipt.error = "target_not_regular"; receipt.error_number = EINVAL;
        return osif_publish_observe(store, &receipt);
    }
    if (found) {
        receipt.existing_size_known = existing.st_size >= 0;
        receipt.existing_bytes = existing.st_size >= 0 ? (uint64_t)existing.st_size : 0;
        if (!replace) {
            receipt.action = "preserved"; receipt.durable = -1;
            return osif_publish_observe(store, &receipt);
        }
    }
    static unsigned sequence;
    char temporary[80]; int fd = -1;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        (void)snprintf(temporary, sizeof temporary, ".osif-tmp-%ld-%u", (long)getpid(), ++sequence);
        fd = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        receipt.error = "temporary_open_failed"; receipt.error_number = errno;
        return osif_publish_observe(store, &receipt);
    }
    receipt.temporary_created = 1;
    while (receipt.written_bytes < size) {
        ssize_t n = write(fd, (const char*)bytes + receipt.written_bytes, size - (size_t)receipt.written_bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            receipt.error = "file_write_failed"; receipt.error_number = n < 0 && errno ? errno : EIO; break;
        }
        receipt.written_bytes += (uint64_t)n;
    }
    if (!receipt.error && fsync(fd)) { receipt.error = "file_sync_failed"; receipt.error_number = errno; }
    if (close(fd) && !receipt.error) { receipt.error = "file_close_failed"; receipt.error_number = errno; }
    int temporary_exists = 1;
    if (!receipt.error) {
        int rc = replace ? renameat(parent, temporary, parent, name) : linkat(parent, temporary, parent, name, 0);
        if (rc) { receipt.error = "file_publish_failed"; receipt.error_number = errno; }
        else {
            receipt.applied = 1; receipt.action = found ? "replaced" : "created";
            if (replace) temporary_exists = 0;
        }
    }
    if (temporary_exists && unlinkat(parent, temporary, 0)) {
        receipt.cleanup_error = errno;
        if (!receipt.error) { receipt.error = "temporary_cleanup_failed"; receipt.error_number = errno; }
        if (!osif_publish_path(store, temporary, temporary_path)) receipt.temporary_path = temporary_path;
    }
    if (receipt.applied) {
        if (fsync(parent)) {
            if (!receipt.error) { receipt.error = "directory_sync_failed"; receipt.error_number = errno; }
        } else receipt.durable = 1;
    }
    return osif_publish_observe(store, &receipt);
}
#endif
