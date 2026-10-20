// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "ls_collector.h"

#include "osif/osif_stat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef struct {
    unsigned char* name;
    size_t len;
} ls_name_t;

typedef struct {
    size_t limit;
    size_t current;
    size_t peak;
    size_t retained_current;
    size_t retained_peak;
} ls_memory_t;

typedef struct {
    jash_ls_collect_options_t options;
    jash_ls_item_callback_t callback;
    void* context;
    jash_ls_collect_summary_t summary;
    ls_memory_t memory;
} ls_collector_t;

static int add_size(size_t a, size_t b, size_t* result) {
    if (a > SIZE_MAX - b) return -1;
    *result = a + b;
    return 0;
}

static int increment_size(size_t* value) {
    if (*value == SIZE_MAX) return -1;
    (*value)++;
    return 0;
}

static void set_terminal(ls_collector_t* collector,
                         jash_ls_terminal_state_t state, int error_number) {
    collector->summary.state = state;
    collector->summary.error_number = error_number;
}

static int producer_error(ls_collector_t* collector, int error_number) {
    if (!error_number) error_number = EIO;
    set_terminal(collector, JASH_LS_TERMINAL_PRODUCER_ERROR, error_number);
    errno = error_number;
    return -1;
}

static int sink_error(ls_collector_t* collector, int error_number) {
    if (!error_number) error_number = EIO;
    set_terminal(collector, JASH_LS_TERMINAL_SINK_ERROR, error_number);
    errno = error_number;
    return -1;
}

static void memory_record(ls_collector_t* collector) {
    if (collector->memory.current > collector->memory.peak)
        collector->memory.peak = collector->memory.current;
    if (collector->memory.retained_current > collector->memory.retained_peak)
        collector->memory.retained_peak = collector->memory.retained_current;
}

static void* collector_alloc(ls_collector_t* collector, size_t size,
                             int retained) {
    if (!size || size > collector->memory.limit - collector->memory.current) {
        errno = size ? ENOMEM : EINVAL;
        return NULL;
    }
    void* allocation = malloc(size);
    if (!allocation) return NULL;
    collector->memory.current += size;
    if (retained) collector->memory.retained_current += size;
    memory_record(collector);
    return allocation;
}

static void collector_free(ls_collector_t* collector, void* allocation,
                           size_t size, int retained) {
    if (!allocation) return;
    free(allocation);
    collector->memory.current -= size;
    if (retained) collector->memory.retained_current -= size;
}

static int unsigned_name_compare(const void* left, const void* right) {
    const ls_name_t* a = left;
    const ls_name_t* b = right;
    size_t common = a->len < b->len ? a->len : b->len;
    int result = memcmp(a->name, b->name, common);
    if (result) return result;
    return (a->len > b->len) - (a->len < b->len);
}

static int raw_name_compare(const unsigned char* left, size_t left_len,
                            const ls_name_t* right) {
    ls_name_t value;
    value.name = (unsigned char*)left;
    value.len = left_len;
    return unsigned_name_compare(&value, right);
}

static size_t larger_child(const ls_name_t* names, size_t left, size_t count) {
    size_t right = left + 1;
    if (right < count && unsigned_name_compare(&names[left], &names[right]) < 0)
        return right;
    return left;
}

static void heap_sift_up(ls_name_t* names, size_t index) {
    while (index) {
        size_t parent = (index - 1) / 2;
        if (unsigned_name_compare(&names[parent], &names[index]) >= 0) break;
        ls_name_t temporary = names[parent];
        names[parent] = names[index];
        names[index] = temporary;
        index = parent;
    }
}

static void heap_sift_down(ls_name_t* names, size_t count) {
    size_t index = 0;
    while (index <= (count - 1) / 2) {
        size_t left = index * 2 + 1;
        if (left >= count) break;
        size_t child = larger_child(names, left, count);
        if (unsigned_name_compare(&names[index], &names[child]) >= 0) break;
        ls_name_t temporary = names[index];
        names[index] = names[child];
        names[child] = temporary;
        index = child;
    }
}

static jash_ls_type_t file_type(mode_t mode) {
    if (S_ISREG(mode)) return JASH_LS_TYPE_REGULAR;
    if (S_ISDIR(mode)) return JASH_LS_TYPE_DIRECTORY;
    if (S_ISLNK(mode)) return JASH_LS_TYPE_SYMLINK;
    if (S_ISFIFO(mode)) return JASH_LS_TYPE_FIFO;
    if (S_ISSOCK(mode)) return JASH_LS_TYPE_SOCKET;
    if (S_ISBLK(mode)) return JASH_LS_TYPE_BLOCK;
    if (S_ISCHR(mode)) return JASH_LS_TYPE_CHARACTER;
    return JASH_LS_TYPE_UNKNOWN;
}

static int fill_timestamp(jash_ls_timestamp_t* destination,
                          const struct timespec* source) {
    int64_t seconds = (int64_t)source->tv_sec;
    if ((time_t)seconds != source->tv_sec ||
        (seconds < 0 && source->tv_sec > (time_t)0) ||
        source->tv_nsec < 0 || source->tv_nsec >= 1000000000L)
        return -1;
    destination->seconds = seconds;
    destination->nanoseconds = (uint32_t)source->tv_nsec;
    destination->available = 1;
    return 0;
}

static int fill_metadata(jash_ls_item_t* item, const struct stat* st) {
    int conversion_error = 0;
    uint64_t value;

    item->type = file_type(st->st_mode);
    value = (uint64_t)st->st_mode;
    if ((mode_t)value == st->st_mode) {
        item->metadata.mode = value;
        item->metadata.available |= JASH_LS_META_MODE;
    } else conversion_error = 1;
    value = (uint64_t)st->st_uid;
    if ((uid_t)value == st->st_uid) {
        item->metadata.uid = value;
        item->metadata.available |= JASH_LS_META_UID;
    } else conversion_error = 1;
    value = (uint64_t)st->st_gid;
    if ((gid_t)value == st->st_gid) {
        item->metadata.gid = value;
        item->metadata.available |= JASH_LS_META_GID;
    } else conversion_error = 1;
    value = (uint64_t)st->st_ino;
    if ((ino_t)value == st->st_ino) {
        item->metadata.inode = value;
        item->metadata.available |= JASH_LS_META_INODE;
    } else conversion_error = 1;
    value = (uint64_t)st->st_dev;
    if ((dev_t)value == st->st_dev) {
        item->metadata.device = value;
        item->metadata.available |= JASH_LS_META_DEVICE;
    } else conversion_error = 1;
    value = (uint64_t)st->st_nlink;
    if ((nlink_t)value == st->st_nlink) {
        item->metadata.nlink = value;
        item->metadata.available |= JASH_LS_META_NLINK;
    } else conversion_error = 1;
    if (st->st_size >= 0) {
        value = (uint64_t)st->st_size;
        if ((off_t)value == st->st_size) {
            item->metadata.size = value;
            item->metadata.available |= JASH_LS_META_SIZE;
        } else conversion_error = 1;
    }

    if (fill_timestamp(&item->metadata.modified, &BEAR_ST_MTIM(*st)) != 0)
        conversion_error = 1;
    if (fill_timestamp(&item->metadata.changed, &BEAR_ST_CTIM(*st)) != 0)
        conversion_error = 1;
    if (fill_timestamp(&item->metadata.accessed, &BEAR_ST_ATIM(*st)) != 0)
        conversion_error = 1;
    item->metadata.created.available = 0;
    return conversion_error ? -1 : 0;
}

static int same_identity(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
        left->st_mode == right->st_mode &&
        BEAR_ST_CTIM(*left).tv_sec == BEAR_ST_CTIM(*right).tv_sec &&
        BEAR_ST_CTIM(*left).tv_nsec == BEAR_ST_CTIM(*right).tv_nsec;
}

static void item_error(ls_collector_t* collector, jash_ls_item_t* item,
                       jash_ls_item_error_op_t operation, int error_number) {
    if (item->error.operation != JASH_LS_ITEM_ERROR_NONE) return;
    item->error.operation = operation;
    item->error.error_number = error_number ? error_number : EIO;
    if (increment_size(&collector->summary.errors_seen) != 0)
        collector->summary.errors_seen = SIZE_MAX;
}

static int path_metadata(ls_collector_t* collector, int directory_fd,
                         const unsigned char* metadata_name,
                         jash_ls_item_t* item, struct stat* initial) {
    int flags = collector->options.symlink_policy == JASH_LS_SYMLINK_NOFOLLOW
        ? AT_SYMLINK_NOFOLLOW : 0;
    item->consistency = JASH_LS_CONSISTENCY_PATH_FALLBACK;
    if (fstatat(directory_fd, (const char*)metadata_name, initial, flags) != 0) {
        item_error(collector, item, JASH_LS_ITEM_ERROR_METADATA, errno);
        return -1;
    }
    if (fill_metadata(item, initial) != 0)
        item_error(collector, item, JASH_LS_ITEM_ERROR_METADATA, EOVERFLOW);
    return 0;
}

static int descriptor_metadata(ls_collector_t* collector, int directory_fd,
                               const unsigned char* metadata_name,
                               jash_ls_item_t* item, struct stat* initial,
                               int* object_fd) {
#if defined(__linux__) && defined(O_PATH)
    int flags = O_PATH | O_CLOEXEC;
    if (collector->options.symlink_policy == JASH_LS_SYMLINK_NOFOLLOW)
        flags |= O_NOFOLLOW;
    int fd = openat(directory_fd, (const char*)metadata_name, flags);
    if (fd >= 0) {
        item->consistency = JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND;
        if (fstat(fd, initial) != 0) {
            int saved = errno;
            close(fd);
            item_error(collector, item, JASH_LS_ITEM_ERROR_METADATA, saved);
            return -1;
        }
        if (fill_metadata(item, initial) != 0)
            item_error(collector, item, JASH_LS_ITEM_ERROR_METADATA, EOVERFLOW);
        *object_fd = fd;
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
        item_error(collector, item, JASH_LS_ITEM_ERROR_METADATA, errno);
        return -1;
    }
#else
    (void)object_fd;
#endif
    return path_metadata(collector, directory_fd, metadata_name, item, initial);
}

static int verify_path_identity(ls_collector_t* collector, int directory_fd,
                                const unsigned char* metadata_name,
                                const struct stat* initial,
                                jash_ls_item_t* item) {
    struct stat current;
    if (fstatat(directory_fd, (const char*)metadata_name, &current,
                AT_SYMLINK_NOFOLLOW) != 0) {
        int saved = errno;
        item->consistency = JASH_LS_CONSISTENCY_UNSTABLE;
        item_error(collector, item, JASH_LS_ITEM_ERROR_UNSTABLE, saved);
        return -1;
    }
    if (!same_identity(initial, &current)) {
        item->consistency = JASH_LS_CONSISTENCY_UNSTABLE;
        item_error(collector, item, JASH_LS_ITEM_ERROR_UNSTABLE, ESTALE);
        return -1;
    }
    return 0;
}

static int read_symlink_target(ls_collector_t* collector, int directory_fd,
                               const unsigned char* metadata_name, int object_fd,
                               const struct stat* initial, jash_ls_item_t* item,
                               unsigned char** allocation, size_t* allocation_size) {
    if (item->type != JASH_LS_TYPE_SYMLINK ||
        collector->options.symlink_policy != JASH_LS_SYMLINK_NOFOLLOW ||
        !collector->options.max_symlink_bytes)
        return 0;

    size_t capacity = collector->options.max_symlink_bytes + 1;
    if (capacity > collector->memory.limit - collector->memory.current)
        return 1;
    unsigned char* target = collector_alloc(collector, capacity, 0);
    if (!target) return producer_error(collector, errno ? errno : ENOMEM);
    *allocation = target;
    *allocation_size = capacity;

    ssize_t length = -1;
#if defined(__linux__) && defined(O_PATH)
    if (object_fd >= 0)
        length = readlinkat(object_fd, "", (char*)target, capacity);
#else
    (void)object_fd;
#endif
    if (length < 0) {
        if (item->consistency == JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND &&
            errno != EINVAL && errno != ENOENT && errno != EOPNOTSUPP) {
            item_error(collector, item, JASH_LS_ITEM_ERROR_READLINK, errno);
            return 0;
        }
        item->consistency = JASH_LS_CONSISTENCY_PATH_FALLBACK;
        length = readlinkat(directory_fd, (const char*)metadata_name,
                            (char*)target, capacity);
        if (length < 0) {
            item_error(collector, item, JASH_LS_ITEM_ERROR_READLINK, errno);
            return 0;
        }
        (void)verify_path_identity(collector, directory_fd, metadata_name,
                                   initial, item);
    }

    size_t actual = (size_t)length;
    item->symlink_target_available = 1;
    item->symlink_target_truncated =
        actual > collector->options.max_symlink_bytes;
    item->symlink_target.data = target;
    item->symlink_target.len = item->symlink_target_truncated
        ? collector->options.max_symlink_bytes : actual;
    return 0;
}

/* 0 continues, 1 truncates, 2 cancels, and -1 is terminal error. */
static int callback_item(ls_collector_t* collector, int metadata_fd,
                         const unsigned char* metadata_name,
                         const unsigned char* name, size_t name_len,
                         const unsigned char* path, size_t path_len) {
    size_t item_bytes;
    if (add_size(name_len, path_len, &item_bytes) != 0 ||
        item_bytes > collector->options.max_logical_bytes -
                     collector->summary.logical_bytes_emitted)
        return 1;

    jash_ls_item_t item;
    memset(&item, 0, sizeof(item));
    item.name.data = name;
    item.name.len = name_len;
    item.path.data = path;
    item.path.len = path_len;
    item.type = JASH_LS_TYPE_UNKNOWN;
    item.consistency = JASH_LS_CONSISTENCY_UNAVAILABLE;

    struct stat initial;
    memset(&initial, 0, sizeof(initial));
    int object_fd = -1;
    int have_metadata = descriptor_metadata(collector, metadata_fd,
                                            metadata_name, &item, &initial,
                                            &object_fd) == 0;
    unsigned char* target = NULL;
    size_t target_allocation = 0;
    int target_result = 0;
    if (have_metadata)
        target_result = read_symlink_target(collector, metadata_fd,
                                            metadata_name, object_fd, &initial,
                                            &item, &target,
                                            &target_allocation);
    if (object_fd >= 0) close(object_fd);
    if (target_result > 0) return 1;
    if (target_result < 0) {
        collector_free(collector, target, target_allocation, 0);
        return -1;
    }

    if (add_size(item_bytes, item.symlink_target.len, &item_bytes) != 0 ||
        item_bytes > collector->options.max_logical_bytes -
                     collector->summary.logical_bytes_emitted) {
        collector_free(collector, target, target_allocation, 0);
        return 1;
    }

    errno = 0;
    jash_ls_callback_result_t callback_result =
        collector->callback(collector->context, &item);
    int callback_errno = errno;
    if (callback_result == JASH_LS_CALLBACK_ERROR) {
        collector_free(collector, target, target_allocation, 0);
        return sink_error(collector, callback_errno ? callback_errno : EIO);
    }
    if (callback_result != JASH_LS_CALLBACK_CONTINUE &&
        callback_result != JASH_LS_CALLBACK_CANCEL) {
        collector_free(collector, target, target_allocation, 0);
        return sink_error(collector, EPROTO);
    }

    collector->summary.items_emitted++;
    collector->summary.logical_bytes_emitted += item_bytes;
    if (item.symlink_target_truncated)
        collector->summary.symlink_targets_truncated++;
    collector_free(collector, target, target_allocation, 0);
    if (callback_result == JASH_LS_CALLBACK_CANCEL) {
        set_terminal(collector, JASH_LS_TERMINAL_CANCELLED, 0);
        return 2;
    }
    return 0;
}

static void free_names(ls_collector_t* collector, ls_name_t* names,
                       size_t count, size_t slots) {
    for (size_t i = 0; i < count; i++)
        collector_free(collector, names[i].name, names[i].len + 1, 1);
    collector_free(collector, names, slots * sizeof(*names), 1);
}

static int retain_name(ls_collector_t* collector, ls_name_t* names,
                       size_t* count, size_t limit,
                       const unsigned char* name, size_t length) {
    if (!limit) return 0;
    if (*count == limit && raw_name_compare(name, length, &names[0]) >= 0)
        return 0;

    if (*count == limit) {
        collector_free(collector, names[0].name, names[0].len + 1, 1);
        names[0].name = NULL;
        names[0].len = 0;
        names[0].name = collector_alloc(collector, length + 1, 1);
        if (!names[0].name) return -1;
        memcpy(names[0].name, name, length);
        names[0].name[length] = 0;
        names[0].len = length;
        heap_sift_down(names, *count);
        return 0;
    }

    unsigned char* copy = collector_alloc(collector, length + 1, 1);
    if (!copy) return -1;
    memcpy(copy, name, length);
    copy[length] = 0;
    names[*count].name = copy;
    names[*count].len = length;
    heap_sift_up(names, *count);
    (*count)++;
    return 0;
}

static int collect_directory(ls_collector_t* collector,
                              const unsigned char* root) {
    int fd = open((const char*)root,
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                  (collector->options.follow_root_symlink ? 0 : O_NOFOLLOW));
    if (fd < 0) return producer_error(collector, errno);
    DIR* directory = fdopendir(fd);
    if (!directory) {
        int saved = errno;
        close(fd);
        return producer_error(collector, saved);
    }

    size_t root_allocation = collector->options.root.len + 1;
    size_t path_reserve, transient_reserve;
    if (add_size(root_allocation, NAME_MAX + 1, &path_reserve) != 0 ||
        add_size(path_reserve,
                 collector->options.max_symlink_bytes
                     ? collector->options.max_symlink_bytes + 1 : 0,
                 &transient_reserve) != 0) {
        closedir(directory);
        return producer_error(collector, EOVERFLOW);
    }
    size_t available = collector->memory.limit - root_allocation;
    size_t per_slot = sizeof(ls_name_t) + NAME_MAX + 1;
    size_t slots = available > transient_reserve
        ? (available - transient_reserve) / per_slot : 0;
    if (slots > collector->options.max_items) slots = collector->options.max_items;
    ls_name_t* names = slots
        ? collector_alloc(collector, slots * sizeof(*names), 1) : NULL;
    if (slots && !names) {
        int saved = errno ? errno : ENOMEM;
        closedir(directory);
        return producer_error(collector, saved);
    }

    size_t count = 0;
    int scan_error = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (!entry) {
            scan_error = errno;
            break;
        }
        const unsigned char* name = (const unsigned char*)entry->d_name;
        size_t length = strlen(entry->d_name);
        if (length > NAME_MAX) {
            scan_error = EOVERFLOW;
            break;
        }
        if ((length == 1 && name[0] == '.') ||
            (length == 2 && name[0] == '.' && name[1] == '.'))
            continue;
        if (!collector->options.include_hidden && length && name[0] == '.')
            continue;
        if (increment_size(&collector->summary.items_seen) != 0) {
            scan_error = EOVERFLOW;
            break;
        }
        if (retain_name(collector, names, &count, slots, name, length) != 0) {
            scan_error = errno ? errno : ENOMEM;
            break;
        }
    }

    if (scan_error) {
        free_names(collector, names, count, slots);
        closedir(directory);
        return producer_error(collector, scan_error);
    }
    if (count > 1) qsort(names, count, sizeof(*names), unsigned_name_compare);

    int result = 0;
    int truncated = collector->summary.items_seen > count;
    int directory_fd = dirfd(directory);
    for (size_t i = 0; i < count; i++) {
        size_t separator = collector->options.root.len &&
            root[collector->options.root.len - 1] == '/' ? 0 : 1;
        size_t with_separator, path_len;
        if (add_size(collector->options.root.len, separator,
                     &with_separator) != 0 ||
            add_size(with_separator, names[i].len, &path_len) != 0 ||
            path_len == SIZE_MAX) {
            result = producer_error(collector, EOVERFLOW);
            break;
        }
        unsigned char* path = collector_alloc(collector, path_len + 1, 0);
        if (!path) {
            result = producer_error(collector, errno ? errno : ENOMEM);
            break;
        }
        memcpy(path, root, collector->options.root.len);
        if (separator) path[collector->options.root.len] = '/';
        memcpy(path + with_separator, names[i].name, names[i].len);
        path[path_len] = 0;
        result = callback_item(collector, directory_fd, names[i].name,
                               names[i].name, names[i].len, path, path_len);
        collector_free(collector, path, path_len + 1, 0);
        if (result == 1) {
            truncated = 1;
            result = 0;
            break;
        }
        if (result != 0) break;
    }

    free_names(collector, names, count, slots);
    if (closedir(directory) != 0 && result >= 0)
        return producer_error(collector, errno);
    if (result < 0) return -1;
    if (result == 2) return 0;
    if (collector->summary.symlink_targets_truncated) truncated = 1;
    set_terminal(collector, truncated ? JASH_LS_TERMINAL_TRUNCATED
                                      : JASH_LS_TERMINAL_COMPLETE, 0);
    return 0;
}

static int collect_operand(ls_collector_t* collector,
                            const unsigned char* root) {
    size_t end = collector->options.root.len;
    while (end > 1 && root[end - 1] == '/') end--;
    size_t start = end;
    while (start && root[start - 1] != '/') start--;
    if (start == end) start = 0;
    size_t name_len = end - start;

    collector->summary.items_seen = 1;
    int result = callback_item(collector, AT_FDCWD, root, root + start,
                               name_len, root, collector->options.root.len);
    if (result == 1) {
        set_terminal(collector, JASH_LS_TERMINAL_TRUNCATED, 0);
        return 0;
    }
    if (result < 0) return -1;
    if (result == 2) return 0;
    set_terminal(collector, collector->summary.symlink_targets_truncated
        ? JASH_LS_TERMINAL_TRUNCATED : JASH_LS_TERMINAL_COMPLETE, 0);
    return 0;
}

/* Resumable traversal retains only the sorted names. Metadata is observed
 * separately on demand by the same callback_item() used by push collection. */
typedef struct {
    ls_collector_t collector;
    unsigned char* root;
    DIR* directory;
    ls_name_t* names;
    size_t slots, count, index;
    unsigned char* path;
    size_t path_capacity;
    unsigned char* target;
    size_t target_capacity;
    jash_ls_item_t current;
    int terminal;
} ls_cursor_state_t;

static int options_valid(const jash_ls_collect_options_t* options) {
    if (!options || !options->root.data || !options->root.len ||
        options->root.len > (size_t)SSIZE_MAX ||
        options->root.len == SIZE_MAX || !options->max_items ||
        !options->max_logical_bytes || !options->max_memory_bytes ||
        options->max_symlink_bytes == SIZE_MAX ||
        options->max_symlink_bytes > (size_t)SSIZE_MAX - 1 ||
        options->root.len + 1 > options->max_memory_bytes ||
        (options->mode != JASH_LS_COLLECT_DIRECTORY &&
         options->mode != JASH_LS_COLLECT_OPERAND) ||
        (options->symlink_policy != JASH_LS_SYMLINK_NOFOLLOW &&
         options->symlink_policy != JASH_LS_SYMLINK_FOLLOW) ||
        (options->follow_root_symlink != 0 &&
         options->follow_root_symlink != 1)) {
        errno = EINVAL;
        return -1;
    }
    if (memchr(options->root.data, 0, options->root.len)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static void cursor_cleanup(ls_cursor_state_t* state) {
    ls_collector_t* collector = &state->collector;
    if (state->directory) closedir(state->directory);
    free_names(collector, state->names, state->count, state->slots);
    collector_free(collector, state->target, state->target_capacity, 1);
    collector_free(collector, state->path, state->path_capacity, 1);
    collector_free(collector, state->root, collector->options.root.len + 1, 1);
    free(state);
}

static int cursor_scan(ls_cursor_state_t* state) {
    ls_collector_t* collector = &state->collector;
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC |
        (collector->options.follow_root_symlink ? 0 : O_NOFOLLOW);
    int fd = open((const char*)state->root, flags);
    if (fd < 0) return producer_error(collector, errno);
    state->directory = fdopendir(fd);
    if (!state->directory) {
        int saved = errno;
        close(fd);
        return producer_error(collector, saved);
    }
    size_t per_slot = sizeof(ls_name_t) + NAME_MAX + 1;
    state->slots = (collector->memory.limit - collector->memory.current) /
                   per_slot;
    if (state->slots > collector->options.max_items)
        state->slots = collector->options.max_items;
    if (state->slots) {
        state->names = collector_alloc(collector,
            state->slots * sizeof(*state->names), 1);
        if (!state->names)
            return producer_error(collector, errno ? errno : ENOMEM);
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(state->directory);
        if (!entry) {
            if (errno) return producer_error(collector, errno);
            break;
        }
        const unsigned char* name = (const unsigned char*)entry->d_name;
        size_t length = strlen(entry->d_name);
        if (length > NAME_MAX)
            return producer_error(collector, EOVERFLOW);
        if ((length == 1 && name[0] == '.') ||
            (length == 2 && name[0] == '.' && name[1] == '.')) continue;
        if (!collector->options.include_hidden && length && name[0] == '.')
            continue;
        if (increment_size(&collector->summary.items_seen) != 0)
            return producer_error(collector, EOVERFLOW);
        if (retain_name(collector, state->names, &state->count, state->slots,
                        name, length) != 0)
            return producer_error(collector, errno ? errno : ENOMEM);
    }
    if (state->count > 1)
        qsort(state->names, state->count, sizeof(*state->names),
              unsigned_name_compare);
    return 0;
}

static jash_ls_callback_result_t capture_cursor_item(void* context,
                                                      const jash_ls_item_t* item) {
    ls_cursor_state_t* state = context;
    state->current = *item;
    state->current.symlink_target.data = state->target;
    if (item->symlink_target.len) {
        if (item->symlink_target.len > state->target_capacity) {
            errno = EOVERFLOW;
            return JASH_LS_CALLBACK_ERROR;
        }
        memcpy(state->target, item->symlink_target.data, item->symlink_target.len);
        state->current.symlink_target.data = state->target;
    }
    return JASH_LS_CALLBACK_CONTINUE;
}

int jash_ls_cursor_open(jash_ls_cursor_t* cursor,
                        const jash_ls_collect_options_t* options) {
    if (!cursor || (cursor->_self && cursor->_self != cursor) ||
        cursor->_state || options_valid(options) != 0) {
        if (!cursor) errno = EINVAL;
        else if (cursor->_self && cursor->_self != cursor) errno = ESTALE;
        else if (cursor->_state) errno = EALREADY;
        return -1;
    }
    if (options->max_memory_bytes < sizeof(ls_cursor_state_t)) {
        errno = ENOMEM;
        return -1;
    }
    ls_cursor_state_t* state = calloc(1, sizeof(*state));
    if (!state) return -1;
    ls_collector_t* collector = &state->collector;
    collector->options = *options;
    collector->callback = capture_cursor_item;
    collector->context = state;
    collector->memory.limit = options->max_memory_bytes;
    collector->memory.current = sizeof(*state);
    collector->memory.retained_current = sizeof(*state);
    memory_record(collector);
    collector->summary.state = JASH_LS_TERMINAL_PENDING;

    size_t root_size = options->root.len + 1;
    state->root = collector_alloc(collector, root_size, 1);
    if (!state->root) goto error;
    memcpy(state->root, options->root.data, options->root.len);
    state->root[options->root.len] = 0;
    collector->options.root.data = state->root;
    if (options->mode == JASH_LS_COLLECT_DIRECTORY) {
        if (add_size(root_size, NAME_MAX + 1, &state->path_capacity) != 0) {
            errno = EOVERFLOW;
            goto error;
        }
        state->path = collector_alloc(collector, state->path_capacity, 1);
        if (!state->path) goto error;
    }
    if (options->max_symlink_bytes) {
        state->target_capacity = options->max_symlink_bytes;
        state->target = collector_alloc(collector, state->target_capacity, 1);
        if (!state->target) goto error;
    }
    if (options->mode == JASH_LS_COLLECT_DIRECTORY) {
        if (cursor_scan(state) != 0) goto error;
    } else {
        collector->summary.items_seen = 1;
    }
    cursor->_self = cursor;
    cursor->_state = state;
    return 0;
error:
    {
        int saved = errno;
        cursor_cleanup(state);
        errno = saved;
        return -1;
    }
}

static ls_cursor_state_t* cursor_state(const jash_ls_cursor_t* cursor) {
    if (!cursor) { errno = EINVAL; return NULL; }
    if (cursor->_self != cursor) { errno = ESTALE; return NULL; }
    if (!cursor->_state) { errno = EBADF; return NULL; }
    return cursor->_state;
}

jash_ls_next_result_t jash_ls_cursor_next(jash_ls_cursor_t* cursor,
                                          const jash_ls_item_t** item) {
    if (!item) { errno = EINVAL; return JASH_LS_NEXT_ERROR; }
    *item = NULL;
    ls_cursor_state_t* state = cursor_state(cursor);
    if (!state) return JASH_LS_NEXT_ERROR;
    ls_collector_t* collector = &state->collector;
    if (state->terminal) {
        if (collector->summary.state == JASH_LS_TERMINAL_PRODUCER_ERROR ||
            collector->summary.state == JASH_LS_TERMINAL_SINK_ERROR) {
            errno = collector->summary.error_number;
            return JASH_LS_NEXT_ERROR;
        }
        return JASH_LS_NEXT_END;
    }
    memset(&state->current, 0, sizeof(state->current));
    int result;
    if (collector->options.mode == JASH_LS_COLLECT_OPERAND) {
        if (state->index != 0) {
            result = 1;
        } else {
            size_t end = collector->options.root.len;
            while (end > 1 && state->root[end - 1] == '/') end--;
            size_t start = end;
            while (start && state->root[start - 1] != '/') start--;
            if (start == end) start = 0;
            result = callback_item(collector, AT_FDCWD, state->root,
                state->root + start, end - start,
                state->root, collector->options.root.len);
            state->index++;
        }
    } else if (state->index >= state->count) {
        result = 1;
    } else {
        ls_name_t* name = &state->names[state->index++];
        size_t root_len = collector->options.root.len;
        size_t separator = state->root[root_len - 1] == '/' ? 0 : 1;
        size_t path_len = root_len + separator + name->len;
        if (path_len >= state->path_capacity)
            result = producer_error(collector, EOVERFLOW);
        else {
            memcpy(state->path, state->root, root_len);
            if (separator) state->path[root_len] = '/';
            memcpy(state->path + root_len + separator, name->name, name->len);
            state->path[path_len] = 0;
            result = callback_item(collector, dirfd(state->directory), name->name,
                name->name, name->len, state->path, path_len);
        }
    }
    if (result == 0) { *item = &state->current; return JASH_LS_NEXT_ITEM; }
    state->terminal = 1;
    if (result == 1 && state->directory) {
        if (closedir(state->directory) != 0) {
            int saved = errno;
            state->directory = NULL;
            result = producer_error(collector, saved);
        } else state->directory = NULL;
    }
    if (result == 1)
        set_terminal(collector,
            (collector->summary.items_seen > collector->summary.items_emitted ||
             collector->summary.symlink_targets_truncated)
                ? JASH_LS_TERMINAL_TRUNCATED : JASH_LS_TERMINAL_COMPLETE, 0);
    if (result == 2)
        set_terminal(collector, JASH_LS_TERMINAL_CANCELLED, 0);
    return result < 0 ? JASH_LS_NEXT_ERROR : JASH_LS_NEXT_END;
}

int jash_ls_cursor_cancel(jash_ls_cursor_t* cursor) {
    ls_cursor_state_t* state = cursor_state(cursor);
    if (!state) return -1;
    if (!state->terminal) {
        state->terminal = 1;
        set_terminal(&state->collector, JASH_LS_TERMINAL_CANCELLED, 0);
    }
    return 0;
}

int jash_ls_cursor_summary(const jash_ls_cursor_t* cursor,
                           jash_ls_collect_summary_t* summary) {
    ls_cursor_state_t* state = cursor_state(cursor);
    if (!state || !summary) { if (!summary) errno = EINVAL; return -1; }
    *summary = state->collector.summary;
    summary->retained_bytes_peak = state->collector.memory.retained_peak;
    summary->collector_bytes_peak = state->collector.memory.peak;
    return 0;
}

int jash_ls_cursor_close(jash_ls_cursor_t* cursor) {
    if (!cursor || (cursor->_self && cursor->_self != cursor)) {
        errno = cursor ? ESTALE : EINVAL;
        return -1;
    }
    if (!cursor->_state) return 0;
    ls_cursor_state_t* state = cursor->_state;
    cursor->_state = NULL;
    cursor->_self = cursor;
    cursor_cleanup(state);
    return 0;
}

int jash_ls_collect(const jash_ls_collect_options_t* options,
                    jash_ls_item_callback_t callback, void* context,
                    jash_ls_collect_summary_t* summary) {
    if (!summary) {
        errno = EINVAL;
        return -1;
    }
    jash_ls_collect_summary_t initial_summary;
    memset(&initial_summary, 0, sizeof(initial_summary));
    initial_summary.state = JASH_LS_TERMINAL_PRODUCER_ERROR;
    initial_summary.error_number = EINVAL;
    *summary = initial_summary;

    if (!callback) {
        errno = EINVAL;
        return -1;
    }
    if (options_valid(options) != 0) return -1;
    jash_ls_collect_options_t snapshot = *options;

    ls_collector_t collector;
    memset(&collector, 0, sizeof(collector));
    collector.options = snapshot;
    collector.callback = callback;
    collector.context = context;
    collector.summary.state = JASH_LS_TERMINAL_PRODUCER_ERROR;
    collector.memory.limit = snapshot.max_memory_bytes;

    unsigned char* root = collector_alloc(&collector, snapshot.root.len + 1, 1);
    if (!root) {
        int saved = errno ? errno : ENOMEM;
        producer_error(&collector, saved);
        collector.summary.retained_bytes_peak = collector.memory.retained_peak;
        collector.summary.collector_bytes_peak = collector.memory.peak;
        *summary = collector.summary;
        errno = saved;
        return -1;
    }
    memcpy(root, snapshot.root.data, snapshot.root.len);
    root[snapshot.root.len] = 0;
    collector.options.root.data = root;

    int result = snapshot.mode == JASH_LS_COLLECT_DIRECTORY
        ? collect_directory(&collector, root)
        : collect_operand(&collector, root);
    int saved = errno;
    collector_free(&collector, root, snapshot.root.len + 1, 1);
    collector.summary.retained_bytes_peak = collector.memory.retained_peak;
    collector.summary.collector_bytes_peak = collector.memory.peak;
    *summary = collector.summary;
    errno = saved;
    return result;
}
