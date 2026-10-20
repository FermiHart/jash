// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Project-local change observations. Every path is relative to one pinned
 * project directory; symlinks in stores are refused. Bounds are failures, not
 * successful truncation. A directory flock serializes cooperating native
 * change commands without creating a lock file during a query. */
#ifndef JASH_BUILTINS_CHANGE_STORE_H
#define JASH_BUILTINS_CHANGE_STORE_H
#include "semantic_events.h"
#include "recovery_output.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <limits.h>

#define CN_FILE_BYTES (512u * 1024u)
#define CN_TOTAL_BYTES (16u * 1024u * 1024u)
#define CN_ENTRIES 128u
#define CN_STEPS 256u
#define CN_PATH 1024u

typedef struct {
    char* bytes;
    size_t size;
    int exists;
    struct stat identity;
} cn_file_t;
typedef struct {
    int root;
    size_t observed_bytes, entries_seen;
    char cwd[PATH_MAX];
    const char* error;
    int error_number;
} cn_store_t;
typedef struct { char* values[CN_ENTRIES]; size_t count; } cn_names_t;
typedef enum { CN_MKDIR, CN_PUBLISH, CN_MOVE, CN_CHECKPOINT } cn_step_kind_t;
typedef struct {
    cn_step_kind_t kind;
    char path[CN_PATH], source[CN_PATH];
    char* bytes;
    size_t size;
    int before_exists;
    struct stat before;
    jash_checkpoint_result_t checkpoint;
} cn_step_t;
typedef struct {
    cn_step_t steps[CN_STEPS];
    size_t count;
} cn_plan_t;
typedef struct {
    int applied, durable, error_number, cleanup_error;
    const char* error;
    char temporary[80];
} cn_effect_t;

static inline int cn_fail(cn_store_t* store, const char* error, int number) {
    store->error = error; store->error_number = number ? number : EIO;
    errno = store->error_number; return -1;
}
static inline int cn_format(char* buffer, size_t capacity, const char* format, ...) {
    va_list args; va_start(args, format);
    int size = vsnprintf(buffer, capacity, format, args); va_end(args);
    if (size < 0 || (size_t)size >= capacity) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
static inline int cn_relative(const char* path) {
    if (!path || !*path || *path == '/' || strlen(path) >= CN_PATH) return 0;
    const char* start = path;
    for (const char* p = path;; p++) {
        if (*p && *p != '/') continue;
        size_t size = (size_t)(p - start);
        if (!size || size > NAME_MAX || (size == 1 && *start == '.') ||
            (size == 2 && start[0] == '.' && start[1] == '.')) return 0;
        if (!*p) return 1;
        start = p + 1;
    }
}
static inline int cn_name(const char* name) {
    return name && strlen(name) < CHG_MAX_NAME && chg_name_valid(name);
}
static inline int cn_identity(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_mode == b->st_mode && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static inline int cn_open(cn_store_t* store, int write) {
    memset(store, 0, sizeof(*store)); store->root = -1;
    if (!getcwd(store->cwd, sizeof store->cwd)) return cn_fail(store, "project_path_unavailable", errno);
    store->root = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (store->root < 0) return cn_fail(store, "project_open_failed", errno);
    if (flock(store->root, (write ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        int saved = errno; close(store->root); store->root = -1;
        return cn_fail(store, "project_lock_failed", saved);
    }
    return 0;
}
static inline int cn_close(cn_store_t* store) {
    if (store->root < 0) return 0;
    int fd = store->root; store->root = -1;
    return close(fd) ? cn_fail(store, "project_close_failed", errno) : 0;
}
static inline int cn_directory(cn_store_t* store, const char* path) {
    if (path && *path && !cn_relative(path)) { errno = EINVAL; return -1; }
    int current = fcntl(store->root, F_DUPFD_CLOEXEC, 3);
    if (current < 0 || !path || !*path) return current;
    char copy[CN_PATH]; memcpy(copy, path, strlen(path) + 1u);
    char* save = NULL;
    for (char* part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        int child = openat(current, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int saved = child < 0 ? errno : 0;
        if (close(current) && !saved) saved = errno;
        if (saved) { if (child >= 0) close(child); errno = saved; return -1; }
        current = child;
    }
    return current;
}
static inline int cn_parent(cn_store_t* store, const char* path, char name[NAME_MAX + 1]) {
    if (!cn_relative(path)) { errno = EINVAL; return -1; }
    char parent[CN_PATH]; memcpy(parent, path, strlen(path) + 1u);
    char* slash = strrchr(parent, '/');
    const char* leaf = slash ? slash + 1 : parent;
    memcpy(name, leaf, strlen(leaf) + 1u);
    if (slash) *slash = 0;
    return cn_directory(store, slash ? parent : "");
}
static inline int cn_stat(cn_store_t* store, const char* path, struct stat* st, int* exists) {
    *exists = 0; char name[NAME_MAX + 1];
    int parent = cn_parent(store, path, name);
    if (parent < 0) return errno == ENOENT ? 0 : cn_fail(store, "path_parent_unavailable", errno);
    int saved = 0;
    if (fstatat(parent, name, st, AT_SYMLINK_NOFOLLOW)) {
        if (errno != ENOENT) saved = errno;
    } else *exists = 1;
    if (close(parent) && !saved) saved = errno;
    return saved ? cn_fail(store, "path_observation_failed", saved) : 0;
}
static inline void cn_file_free(cn_file_t* file) { free(file->bytes); memset(file, 0, sizeof(*file)); }
static inline int cn_read(cn_store_t* store, const char* path, cn_file_t* file) {
    memset(file, 0, sizeof(*file)); char name[NAME_MAX + 1];
    int parent = cn_parent(store, path, name);
    if (parent < 0) return errno == ENOENT ? 0 : cn_fail(store, "file_parent_unavailable", errno);
    int fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        int saved = errno == ENOENT ? 0 : errno;
        if (close(parent) && !saved) saved = errno;
        return saved ? cn_fail(store, "file_open_failed", saved) : 0;
    }
    file->exists = 1; int saved = 0; struct stat after, named;
    if (fstat(fd, &file->identity)) saved = errno;
    else if (!S_ISREG(file->identity.st_mode)) saved = EINVAL;
    else if (file->identity.st_size < 0 || (uint64_t)file->identity.st_size > CN_FILE_BYTES ||
             (uint64_t)file->identity.st_size > CN_TOTAL_BYTES - store->observed_bytes) saved = E2BIG;
    if (!saved) {
        file->bytes = malloc((size_t)file->identity.st_size + 2u);
        if (!file->bytes) saved = ENOMEM;
    }
    while (!saved) {
        size_t capacity = (size_t)file->identity.st_size + 1u;
        ssize_t count = read(fd, file->bytes + file->size, capacity - file->size);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { saved = errno; break; }
        if (!count) break;
        file->size += (size_t)count;
        if (file->size > (size_t)file->identity.st_size) saved = ESTALE;
    }
    if (!saved && (fstat(fd, &after) || fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW))) saved = errno;
    if (!saved && (!cn_identity(&file->identity, &after) || !cn_identity(&after, &named) ||
                   file->size != (size_t)after.st_size)) saved = ESTALE;
    if (close(fd) && !saved) saved = errno;
    if (close(parent) && !saved) saved = errno;
    if (saved) { cn_file_free(file); return cn_fail(store, "file_snapshot_failed", saved); }
    file->bytes[file->size] = 0; store->observed_bytes += file->size;
    return 0;
}
static inline int cn_compare_names(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
static inline void cn_names_free(cn_names_t* names) {
    for (size_t i = 0; i < names->count; i++) free(names->values[i]);
    memset(names, 0, sizeof(*names));
}
static inline int cn_list(cn_store_t* store, const char* path, cn_names_t* names, int* exists) {
    memset(names, 0, sizeof(*names)); *exists = 0;
    int fd = cn_directory(store, path);
    if (fd < 0) return errno == ENOENT ? 0 : cn_fail(store, "directory_open_failed", errno);
    *exists = 1; struct stat before, after; int saved = fstat(fd, &before) ? errno : 0;
    DIR* directory = !saved ? fdopendir(fd) : NULL;
    if (!directory) { if (!saved) saved = errno; close(fd); return cn_fail(store, "directory_open_failed", saved); }
    while (!saved) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { saved = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++store->entries_seen > 4096u) { saved = E2BIG; break; }
        if (names->count == CN_ENTRIES) { saved = E2BIG; break; }
        char* copy = strdup(entry->d_name);
        if (!copy) { saved = ENOMEM; break; }
        names->values[names->count++] = copy;
    }
    if (!saved && fstat(dirfd(directory), &after)) saved = errno;
    if (!saved && !cn_identity(&before, &after)) saved = ESTALE;
    if (closedir(directory) && !saved) saved = errno;
    if (saved) { cn_names_free(names); return cn_fail(store, "directory_snapshot_failed", saved); }
    qsort(names->values, names->count, sizeof(names->values[0]), cn_compare_names); return 0;
}
static inline void cn_plan_free(cn_plan_t* plan) {
    for (size_t i = 0; i < plan->count; i++) free(plan->steps[i].bytes);
    free(plan);
}
static inline cn_step_t* cn_plan_add(cn_store_t* store, cn_plan_t* plan, cn_step_kind_t kind, const char* path) {
    if (plan->count == CN_STEPS) { cn_fail(store, "operation_step_limit", E2BIG); return NULL; }
    cn_step_t* step = &plan->steps[plan->count++]; memset(step, 0, sizeof(*step)); step->kind = kind;
    if (path && cn_format(step->path, sizeof step->path, "%s", path)) {
        plan->count--; cn_fail(store, "operation_path_limit", errno); return NULL;
    }
    return step;
}
static inline int cn_plan_directory(cn_store_t* store, cn_plan_t* plan, const char* path, int exclusive) {
    for (size_t i = 0; i < plan->count; i++)
        if (plan->steps[i].kind == CN_MOVE && !strcmp(path, plan->steps[i].path))
            return exclusive ? cn_fail(store, "target_exists", EEXIST) : 0;
    struct stat st; int exists;
    if (cn_stat(store, path, &st, &exists)) return -1;
    if (exists) {
        if (!S_ISDIR(st.st_mode) || exclusive) return cn_fail(store, exclusive ? "target_exists" : "path_not_directory", exclusive ? EEXIST : ENOTDIR);
        return 0;
    }
    for (size_t i = 0; i < plan->count; i++)
        if (plan->steps[i].kind == CN_MKDIR && !strcmp(path, plan->steps[i].path))
            return exclusive ? cn_fail(store, "target_exists", EEXIST) : 0;
    char parent[CN_PATH]; if (cn_format(parent, sizeof parent, "%s", path)) return cn_fail(store, "operation_path_limit", errno);
    char* slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (cn_plan_directory(store, plan, parent, 0)) return -1; }
    return cn_plan_add(store, plan, CN_MKDIR, path) ? 0 : -1;
}
static inline int cn_plan_publish(cn_store_t* store, cn_plan_t* plan, const char* path,
                                   const void* bytes, size_t size, const cn_file_t* before) {
    if (size > CN_FILE_BYTES || !cn_relative(path)) return cn_fail(store, "publication_limit", E2BIG);
    if (!before) {
        struct stat current; int exists;
        if (cn_stat(store, path, &current, &exists)) return -1;
        if (exists) return cn_fail(store, "publication_target_exists", EEXIST);
    }
    char parent[CN_PATH]; if (cn_format(parent, sizeof parent, "%s", path)) return cn_fail(store, "operation_path_limit", errno);
    char* slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (cn_plan_directory(store, plan, parent, 0)) return -1; }
    cn_step_t* step = cn_plan_add(store, plan, CN_PUBLISH, path); if (!step) return -1;
    step->bytes = malloc(size + 1u);
    if (!step->bytes) return cn_fail(store, "allocation_failed", ENOMEM);
    memcpy(step->bytes, bytes, size); step->bytes[size] = 0; step->size = size;
    if (before) { step->before_exists = before->exists; step->before = before->identity; }
    return 0;
}
static inline int cn_expected(int parent, const char* name, const cn_step_t* step) {
    struct stat current;
    if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT && !step->before_exists) return 0;
        return -1;
    }
    if (!step->before_exists || !cn_identity(&step->before, &current)) { errno = ESTALE; return -1; }
    return 0;
}
static inline cn_effect_t cn_publish(cn_store_t* store, const cn_step_t* step) {
    cn_effect_t result = {0}; char name[NAME_MAX + 1], temporary[80];
    int parent = cn_parent(store, step->path, name);
    if (parent < 0) { result.error_number = errno; result.error = "publication_parent_failed"; return result; }
    if (cn_expected(parent, name, step)) { result.error_number = errno; result.error = "publication_source_changed"; goto finished; }
    static unsigned long serial;
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        if (cn_format(temporary, sizeof temporary, ".change-native-%ld-%lu", (long)getpid(), ++serial)) break;
        fd = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { result.error_number = errno; result.error = "publication_open_failed"; goto finished; }
    size_t offset = 0;
    while (offset < step->size) {
        ssize_t count = write(fd, step->bytes + offset, step->size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result.error_number = count ? errno : EIO; result.error = "publication_write_failed"; break; }
        offset += (size_t)count;
    }
    if (!result.error && fchmod(fd, step->before_exists ? step->before.st_mode & 0777 : 0644)) {
        result.error_number = errno; result.error = "publication_metadata_failed";
    }
    if (!result.error && fsync(fd)) { result.error_number = errno; result.error = "publication_fsync_failed"; }
    if (close(fd) && !result.error) { result.error_number = errno; result.error = "publication_close_failed"; }
    if (!result.error && cn_expected(parent, name, step)) { result.error_number = errno; result.error = "publication_source_changed"; }
    if (!result.error) {
        int failed = step->before_exists ? renameat(parent, temporary, parent, name) : linkat(parent, temporary, parent, name, 0);
        if (failed) { result.error_number = errno; result.error = "publication_commit_failed"; }
        else result.applied = 1;
    }
    if ((!result.applied || !step->before_exists) && unlinkat(parent, temporary, 0)) {
        result.cleanup_error = errno;
        memcpy(result.temporary, temporary, strlen(temporary) + 1u);
        if (!result.error) { result.error_number = errno; result.error = "publication_cleanup_failed"; }
    }
    if (result.applied && !result.error) {
        if (fsync(parent)) { result.error_number = errno; result.error = "publication_directory_fsync_failed"; }
        else result.durable = 1;
    }
finished:
    if (close(parent) && !result.error) { result.error_number = errno; result.error = "publication_parent_close_failed"; }
    return result;
}
static inline cn_effect_t cn_directory_create(cn_store_t* store, const cn_step_t* step) {
    cn_effect_t result = {0}; char name[NAME_MAX + 1];
    int parent = cn_parent(store, step->path, name);
    if (parent < 0) { result.error_number = errno; result.error = "directory_parent_failed"; return result; }
    if (mkdirat(parent, name, 0755)) { result.error_number = errno; result.error = "directory_create_failed"; }
    else {
        result.applied = 1;
        if (fsync(parent)) { result.error_number = errno; result.error = "directory_fsync_failed"; }
        else result.durable = 1;
    }
    if (close(parent) && !result.error) { result.error_number = errno; result.error = "directory_parent_close_failed"; }
    return result;
}
static inline cn_effect_t cn_move(cn_store_t* store, const cn_step_t* step) {
    cn_effect_t result = {0}; char source[NAME_MAX + 1], target[NAME_MAX + 1];
    int from = cn_parent(store, step->source, source), to = -1;
    if (from >= 0) to = cn_parent(store, step->path, target);
    if (from < 0 || to < 0) { result.error_number = errno; result.error = "move_parent_failed"; goto finished; }
    if (cn_expected(from, source, step)) { result.error_number = errno; result.error = "move_source_changed"; goto finished; }
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, from, source, to, target, 1u)) { result.error_number = errno; result.error = "move_commit_failed"; }
#else
    result.error_number = ENOTSUP; result.error = "no_replace_move_unavailable";
#endif
    if (!result.error) {
        result.applied = 1;
        if (fsync(from) || fsync(to)) { result.error_number = errno; result.error = "move_directory_fsync_failed"; }
        else result.durable = 1;
    }
finished:
    if (from >= 0 && close(from) && !result.error) { result.error_number = errno; result.error = "move_parent_close_failed"; }
    if (to >= 0 && close(to) && !result.error) { result.error_number = errno; result.error = "move_parent_close_failed"; }
    return result;
}
#endif
