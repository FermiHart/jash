// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "builtins/ls_collector.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char* test_name;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", test_name, __LINE__, #c, errno); \
    exit(EXIT_FAILURE); \
} } while (0)

enum { CAPTURE_MAX = 800 };

typedef struct {
    unsigned char names[CAPTURE_MAX][NAME_MAX + 1];
    size_t lengths[CAPTURE_MAX];
    size_t logical_bytes[CAPTURE_MAX];
    size_t count;
    int saw_hidden;
    int saw_dot;
    int saw_non_utf8;
    int saw_broken_link;
    int saw_non_utf8_target;
    int saw_directory;
    int saw_fifo;
    int saw_socket;
    int saw_regular_metadata;
} capture_t;

static int byte_compare(const unsigned char* a, size_t an,
                        const unsigned char* b, size_t bn) {
    size_t n = an < bn ? an : bn;
    int result = memcmp(a, b, n);
    if (result) return result;
    return (an > bn) - (an < bn);
}

static jash_ls_callback_result_t capture(void* context,
                                         const jash_ls_item_t* item) {
    capture_t* out = context;
    CHECK(out->count < CAPTURE_MAX);
    CHECK(item->name.len <= NAME_MAX);
    memcpy(out->names[out->count], item->name.data, item->name.len);
    out->lengths[out->count] = item->name.len;
    out->logical_bytes[out->count] =
        item->name.len + item->path.len + item->symlink_target.len;
    out->count++;

    CHECK(item->consistency == JASH_LS_CONSISTENCY_UNAVAILABLE ||
          item->consistency == JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND ||
          item->consistency == JASH_LS_CONSISTENCY_PATH_FALLBACK ||
          item->consistency == JASH_LS_CONSISTENCY_UNSTABLE);
    if (item->name.len && item->name.data[0] == '.') out->saw_hidden++;
    if ((item->name.len == 1 && item->name.data[0] == '.') ||
        (item->name.len == 2 && !memcmp(item->name.data, "..", 2)))
        out->saw_dot++;
    if (item->name.len == 6 && item->name.data[0] == 0xff &&
        !memcmp(item->name.data + 1, "-name", 5)) {
        CHECK(item->path.len >= item->name.len);
        CHECK(!memcmp(item->path.data + item->path.len - item->name.len,
                      item->name.data, item->name.len));
        out->saw_non_utf8++;
    }
    if (item->name.len == 6 && !memcmp(item->name.data, "broken", 6)) {
        CHECK(item->type == JASH_LS_TYPE_SYMLINK);
        CHECK(item->symlink_target_available);
        CHECK(!item->symlink_target_truncated);
        CHECK(item->symlink_target.len == strlen("missing-target"));
        CHECK(!memcmp(item->symlink_target.data, "missing-target",
                      item->symlink_target.len));
        out->saw_broken_link++;
    }
    if (item->name.len == 9 && !memcmp(item->name.data, "byte-link", 9)) {
        const unsigned char expected[] = {0xfe, '-', 't', 'a', 'r', 'g', 'e', 't'};
        CHECK(item->type == JASH_LS_TYPE_SYMLINK);
        CHECK(item->symlink_target_available);
        CHECK(item->symlink_target.len == sizeof(expected));
        CHECK(!memcmp(item->symlink_target.data, expected, sizeof(expected)));
        out->saw_non_utf8_target++;
    }
    if (item->name.len == 4 && !memcmp(item->name.data, "pipe", 4)) {
        CHECK(item->type == JASH_LS_TYPE_FIFO);
        out->saw_fifo++;
    }
    if (item->name.len == 6 && !memcmp(item->name.data, "subdir", 6)) {
        CHECK(item->type == JASH_LS_TYPE_DIRECTORY);
        out->saw_directory++;
    }
    if (item->name.len == 4 && !memcmp(item->name.data, "sock", 4)) {
        CHECK(item->type == JASH_LS_TYPE_SOCKET);
        out->saw_socket++;
    }
    if (item->type == JASH_LS_TYPE_REGULAR) {
        uint32_t all = JASH_LS_META_SIZE | JASH_LS_META_MODE | JASH_LS_META_UID |
                       JASH_LS_META_GID | JASH_LS_META_INODE |
                       JASH_LS_META_DEVICE | JASH_LS_META_NLINK;
        CHECK((item->metadata.available & all) == all);
        CHECK(item->metadata.modified.available);
        CHECK(item->metadata.changed.available);
        CHECK(item->metadata.accessed.available);
        CHECK(!item->metadata.created.available);
        CHECK(item->metadata.modified.nanoseconds < 1000000000u);
        out->saw_regular_metadata++;
    }
    CHECK(item->error.operation == JASH_LS_ITEM_ERROR_NONE);
    return JASH_LS_CALLBACK_CONTINUE;
}

static jash_ls_callback_result_t capture_bounded_link(
    void* context, const jash_ls_item_t* item) {
    int* saw = context;
    if (item->type != JASH_LS_TYPE_SYMLINK) return JASH_LS_CALLBACK_CONTINUE;
    CHECK(item->symlink_target_available);
    CHECK(item->symlink_target_truncated);
    CHECK(item->symlink_target.len == 1);
    (*saw)++;
    return JASH_LS_CALLBACK_CONTINUE;
}

static jash_ls_callback_result_t capture_missing(
    void* context, const jash_ls_item_t* item) {
    int* saw = context;
    CHECK(item->type == JASH_LS_TYPE_UNKNOWN);
    CHECK(item->metadata.available == 0);
    CHECK(item->error.operation == JASH_LS_ITEM_ERROR_METADATA);
    CHECK(item->error.error_number == ENOENT);
    (*saw)++;
    return JASH_LS_CALLBACK_CONTINUE;
}

static jash_ls_callback_result_t capture_followed(
    void* context, const jash_ls_item_t* item) {
    int* saw = context;
    CHECK(item->type == JASH_LS_TYPE_REGULAR);
    CHECK(!item->symlink_target_available);
    CHECK(item->error.operation == JASH_LS_ITEM_ERROR_NONE);
    (*saw)++;
    return JASH_LS_CALLBACK_CONTINUE;
}

typedef struct {
    size_t calls;
    size_t stop_after;
    jash_ls_callback_result_t result;
    int error_number;
} stopping_sink_t;

static jash_ls_callback_result_t stopping_sink(
    void* context, const jash_ls_item_t* item) {
    stopping_sink_t* sink = context;
    (void)item;
    sink->calls++;
    if (sink->calls < sink->stop_after) return JASH_LS_CALLBACK_CONTINUE;
    errno = sink->error_number;
    return sink->result;
}

typedef struct {
    capture_t captured;
    jash_ls_collect_options_t* options;
    jash_ls_collect_summary_t* external_summary;
    unsigned char* root_source;
    int mutated;
} mutation_sink_t;

static jash_ls_callback_result_t mutation_sink(
    void* context, const jash_ls_item_t* item) {
    mutation_sink_t* sink = context;
    if (!sink->mutated) {
        sink->options->mode = JASH_LS_COLLECT_OPERAND;
        sink->options->max_items = 1;
        sink->options->max_logical_bytes = 1;
        sink->options->max_memory_bytes = 1;
        sink->external_summary->state = JASH_LS_TERMINAL_SINK_ERROR;
        sink->external_summary->items_seen = SIZE_MAX;
        sink->root_source[0] = 'X';
        sink->mutated = 1;
    }
    return capture(&sink->captured, item);
}

static void make_file(const char* directory, const char* name) {
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "x", 1) == 1);
    CHECK(close(fd) == 0);
}

static jash_ls_collect_options_t directory_options(const char* directory) {
    jash_ls_collect_options_t options;
    memset(&options, 0, sizeof(options));
    options.root.data = (const unsigned char*)directory;
    options.root.len = strlen(directory);
    options.mode = JASH_LS_COLLECT_DIRECTORY;
    options.symlink_policy = JASH_LS_SYMLINK_NOFOLLOW;
    options.max_items = 2000;
    options.max_logical_bytes = 4 * 1024 * 1024;
    options.max_memory_bytes = 4 * 1024 * 1024;
    options.max_symlink_bytes = 1024;
    return options;
}

#if defined(__linux__)
static size_t descriptor_count(void) {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) { perror("opendir /proc/self/fd"); abort(); }
    size_t count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    }
    if (closedir(directory) != 0) { perror("closedir /proc/self/fd"); abort(); }
    return count;
}
#endif

static void compare_prefix(const capture_t* prefix, const capture_t* complete) {
    CHECK(prefix->count <= complete->count);
    for (size_t i = 0; i < prefix->count; i++) {
        CHECK(prefix->lengths[i] == complete->lengths[i]);
        CHECK(!memcmp(prefix->names[i], complete->names[i], prefix->lengths[i]));
    }
}

static void remove_directory_contents(const char* directory) {
    DIR* dir = opendir(directory);
    CHECK(dir != NULL);
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        CHECK(fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(unlinkat(dirfd(dir), entry->d_name,
                       S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) == 0);
    }
    CHECK(closedir(dir) == 0);
}

int main(void) {
    char directory[] = "/tmp/jash-ls-collector.XXXXXX";
    test_name = "fixture";
    CHECK(mkdtemp(directory) != NULL);

    make_file(directory, "zeta");
    make_file(directory, "alpha");
    make_file(directory, ".hidden");
    for (int i = 599; i >= 0; i--) {
        char name[32];
        CHECK(snprintf(name, sizeof(name), "entry-%03d", i) > 0);
        make_file(directory, name);
    }
    char path[PATH_MAX];
    unsigned char non_utf8_name[] = {0xff, '-', 'n', 'a', 'm', 'e', 0};
    CHECK(snprintf(path, sizeof(path), "%s/", directory) > 0);
    size_t prefix = strlen(path);
    memcpy(path + prefix, non_utf8_name, sizeof(non_utf8_name));
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && close(fd) == 0);

    CHECK(snprintf(path, sizeof(path), "%s/broken", directory) > 0);
    CHECK(symlink("missing-target", path) == 0);
    unsigned char byte_target[] = {0xfe, '-', 't', 'a', 'r', 'g', 'e', 't', 0};
    CHECK(snprintf(path, sizeof(path), "%s/byte-link", directory) > 0);
    CHECK(symlink((const char*)byte_target, path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/follow-link", directory) > 0);
    CHECK(symlink("alpha", path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/pipe", directory) > 0);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/subdir", directory) > 0);
    CHECK(mkdir(path, 0700) == 0);

    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(socket_fd >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "%s/sock", directory) > 0);
    CHECK(bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) == 0);

    test_name = "default ordering, bytes, and types";
    jash_ls_collect_options_t options = directory_options(directory);
    jash_ls_collect_summary_t summary;
    capture_t complete = {0};
    CHECK(jash_ls_collect(&options, capture, &complete, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(summary.items_seen == 609);
    CHECK(summary.items_emitted == complete.count && complete.count == 609);
    CHECK(summary.errors_seen == 0 && summary.logical_bytes_emitted > 0);
    CHECK(summary.symlink_targets_truncated == 0);
    CHECK(summary.retained_bytes_peak <= summary.collector_bytes_peak);
    CHECK(summary.collector_bytes_peak <= options.max_memory_bytes);
    CHECK(!complete.saw_hidden && !complete.saw_dot);
    CHECK(complete.saw_non_utf8 == 1 && complete.saw_broken_link == 1);
    CHECK(complete.saw_non_utf8_target == 1);
    CHECK(complete.saw_directory == 1 && complete.saw_fifo == 1);
    CHECK(complete.saw_socket == 1 && complete.saw_regular_metadata >= 602);
    for (size_t i = 1; i < complete.count; i++)
        CHECK(byte_compare(complete.names[i - 1], complete.lengths[i - 1],
                           complete.names[i], complete.lengths[i]) < 0);

    test_name = "resumable cursor yields sorted names and a terminal";
    jash_ls_cursor_t cursor = {0};
    CHECK(jash_ls_cursor_open(&cursor, &options) == 0);
    const jash_ls_item_t* borrowed = NULL;
    for (size_t i = 0; i < complete.count; i++) {
        CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_ITEM);
        CHECK(borrowed && borrowed->name.len == complete.lengths[i]);
        CHECK(memcmp(borrowed->name.data, complete.names[i], borrowed->name.len) == 0);
    }
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_END);
    CHECK(borrowed == NULL);
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_END);
    CHECK(jash_ls_cursor_summary(&cursor, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(summary.items_emitted == complete.count);
    CHECK(summary.collector_bytes_peak <= options.max_memory_bytes);
    CHECK(summary.retained_bytes_peak <= summary.collector_bytes_peak);
    CHECK(jash_ls_cursor_close(&cursor) == 0);
    CHECK(jash_ls_cursor_close(&cursor) == 0);

#if defined(__linux__)
    test_name = "early release does not retain directory descriptors";
    size_t descriptors_before = descriptor_count();
    for (int attempt = 0; attempt < 32; attempt++) {
        CHECK(jash_ls_cursor_open(&cursor, &options) == 0);
        CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_ITEM);
        CHECK(jash_ls_cursor_close(&cursor) == 0);
    }
    CHECK(descriptor_count() == descriptors_before);
#endif

    test_name = "resumable cursor early cancellation";
    options.max_items = 10;
    CHECK(jash_ls_cursor_open(&cursor, &options) == 0);
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_ITEM);
    CHECK(jash_ls_cursor_cancel(&cursor) == 0);
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_END);
    CHECK(jash_ls_cursor_summary(&cursor, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_CANCELLED);
    CHECK(summary.items_emitted == 1);
    CHECK(jash_ls_cursor_close(&cursor) == 0);

    test_name = "resumable cursor truncation is not EOF completion";
    CHECK(jash_ls_cursor_open(&cursor, &options) == 0);
    for (size_t i = 0; i < options.max_items; i++) {
        CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_ITEM);
        CHECK(memcmp(borrowed->name.data, complete.names[i], borrowed->name.len) == 0);
    }
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_END);
    CHECK(jash_ls_cursor_summary(&cursor, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(summary.items_seen == complete.count && summary.items_emitted == 10);
    CHECK(jash_ls_cursor_close(&cursor) == 0);

    test_name = "resumable cursor borrows non-UTF8 target through next";
    CHECK(snprintf(path, sizeof(path), "%s/byte-link", directory) > 0);
    options = directory_options(path);
    options.mode = JASH_LS_COLLECT_OPERAND;
    CHECK(jash_ls_cursor_open(&cursor, &options) == 0);
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_ITEM);
    CHECK(borrowed->type == JASH_LS_TYPE_SYMLINK);
    CHECK(borrowed->symlink_target.len == sizeof(byte_target) - 1);
    CHECK(memcmp(borrowed->symlink_target.data, byte_target,
                 sizeof(byte_target) - 1) == 0);
    CHECK(jash_ls_cursor_next(&cursor, &borrowed) == JASH_LS_NEXT_END);
    CHECK(jash_ls_cursor_close(&cursor) == 0);

    test_name = "sorted prefix item truncation";
    options = directory_options(directory);
    options.max_items = 10;
    capture_t limited = {0};
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(summary.items_seen == complete.count && limited.count == 10);
    compare_prefix(&limited, &complete);
    CHECK(summary.collector_bytes_peak <= options.max_memory_bytes);

    test_name = "logical byte boundaries";
    options = directory_options(directory);
    options.max_logical_bytes = complete.logical_bytes[0];
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(limited.count == 1);
    CHECK(summary.logical_bytes_emitted == options.max_logical_bytes);
    compare_prefix(&limited, &complete);
    options.max_logical_bytes--;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(limited.count == 0 && summary.logical_bytes_emitted == 0);

    test_name = "memory bounded prefix";
    options = directory_options(directory);
    options.max_items = 2000;
    options.max_memory_bytes = options.root.len + 1 +
        5 * (sizeof(void*) + sizeof(size_t) + NAME_MAX + 1) +
        options.root.len + NAME_MAX + options.max_symlink_bytes + 4;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(limited.count == 5);
    compare_prefix(&limited, &complete);
    CHECK(summary.collector_bytes_peak <= options.max_memory_bytes);

    test_name = "empty directory";
    CHECK(snprintf(path, sizeof(path), "%s/subdir", directory) > 0);
    options = directory_options(path);
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(summary.items_seen == 0 && summary.items_emitted == 0);

    test_name = "explicit directory root symlink policy";
    char linked_root[PATH_MAX];
    CHECK(snprintf(linked_root, sizeof(linked_root), "%s.link", directory) > 0);
    CHECK(symlink(path, linked_root) == 0);
    options = directory_options(linked_root);
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == -1);
    CHECK(summary.state == JASH_LS_TERMINAL_PRODUCER_ERROR);
    options.follow_root_symlink = 1;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(limited.count == 0);
    CHECK(unlink(linked_root) == 0);

    test_name = "hidden policy";
    options = directory_options(directory);
    options.include_hidden = 1;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(limited.count == 610 && limited.saw_hidden == 1 && !limited.saw_dot);

    test_name = "explicit dot operands";
    const char* dot_suffixes[] = {"/.", "/.."};
    const char* dot_names[] = {".", ".."};
    for (size_t i = 0; i < 2; i++) {
        CHECK(snprintf(path, sizeof(path), "%s%s", directory, dot_suffixes[i]) > 0);
        options = directory_options(path);
        options.mode = JASH_LS_COLLECT_OPERAND;
        memset(&limited, 0, sizeof(limited));
        CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
        CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
        CHECK(limited.count == 1 && limited.saw_dot == 1);
        CHECK(limited.lengths[0] == strlen(dot_names[i]));
        CHECK(!memcmp(limited.names[0], dot_names[i], limited.lengths[0]));
    }

    test_name = "missing operand item error";
    CHECK(snprintf(path, sizeof(path), "%s/missing", directory) > 0);
    options = directory_options(path);
    options.mode = JASH_LS_COLLECT_OPERAND;
    int saw_missing = 0;
    CHECK(jash_ls_collect(&options, capture_missing, &saw_missing, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(summary.items_seen == 1 && summary.items_emitted == 1);
    CHECK(summary.errors_seen == 1 && saw_missing == 1);

    test_name = "bounded readlink aggregate";
    options = directory_options(directory);
    options.max_symlink_bytes = 1;
    int saw_bounded_links = 0;
    CHECK(jash_ls_collect(&options, capture_bounded_link, &saw_bounded_links,
                           &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(saw_bounded_links == 3);
    CHECK(summary.symlink_targets_truncated == 3);

    test_name = "follow policy";
    CHECK(snprintf(path, sizeof(path), "%s/follow-link", directory) > 0);
    options = directory_options(path);
    options.mode = JASH_LS_COLLECT_OPERAND;
    options.symlink_policy = JASH_LS_SYMLINK_FOLLOW;
    int saw_followed = 0;
    CHECK(jash_ls_collect(&options, capture_followed, &saw_followed, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE && saw_followed == 1);

    test_name = "callback option and summary mutation";
    unsigned char mutable_root[PATH_MAX];
    size_t directory_len = strlen(directory);
    CHECK(directory_len + 1 <= sizeof(mutable_root));
    memcpy(mutable_root, directory, directory_len + 1);
    options = directory_options((const char*)mutable_root);
    mutation_sink_t mutation;
    memset(&mutation, 0, sizeof(mutation));
    mutation.options = &options;
    mutation.external_summary = &summary;
    mutation.root_source = mutable_root;
    CHECK(jash_ls_collect(&options, mutation_sink, &mutation, &summary) == 0);
    CHECK(mutation.mutated);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(summary.items_seen == complete.count);
    CHECK(mutation.captured.count == complete.count);
    compare_prefix(&mutation.captured, &complete);

    test_name = "callback cancellation";
    options = directory_options(directory);
    stopping_sink_t stopping = {0, 3, JASH_LS_CALLBACK_CANCEL, 0};
    CHECK(jash_ls_collect(&options, stopping_sink, &stopping, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_CANCELLED);
    CHECK(summary.items_emitted == 3 && stopping.calls == 3);
    CHECK(summary.error_number == 0);

    test_name = "callback sink error";
    stopping.calls = 0;
    stopping.stop_after = 2;
    stopping.result = JASH_LS_CALLBACK_ERROR;
    stopping.error_number = EPIPE;
    errno = 0;
    CHECK(jash_ls_collect(&options, stopping_sink, &stopping, &summary) == -1);
    CHECK(errno == EPIPE);
    CHECK(summary.state == JASH_LS_TERMINAL_SINK_ERROR);
    CHECK(summary.error_number == EPIPE && summary.items_emitted == 1);

    test_name = "producer error";
    CHECK(snprintf(path, sizeof(path), "%s/no-directory", directory) > 0);
    options = directory_options(path);
    errno = 0;
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == -1);
    CHECK(errno == ENOENT);
    CHECK(summary.state == JASH_LS_TERMINAL_PRODUCER_ERROR);
    CHECK(summary.error_number == ENOENT);

    test_name = "scalar validation before access";
    static const unsigned char one_byte = 'x';
    options = directory_options(directory);
    options.root.data = &one_byte;
    options.root.len = SIZE_MAX;
    errno = 0;
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == -1);
    CHECK(errno == EINVAL);
    CHECK(summary.state == JASH_LS_TERMINAL_PRODUCER_ERROR);
    options = directory_options(directory);
    options.max_symlink_bytes = SIZE_MAX;
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == -1);
    CHECK(errno == EINVAL);
    options = directory_options(directory);
    options.max_memory_bytes = options.root.len;
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == -1);
    CHECK(errno == EINVAL);

    test_name = "operand transient memory boundary";
    CHECK(snprintf(path, sizeof(path), "%s/broken", directory) > 0);
    options = directory_options(path);
    options.mode = JASH_LS_COLLECT_OPERAND;
    options.max_memory_bytes = options.root.len + 1;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_TRUNCATED);
    CHECK(summary.items_seen == 1 && summary.items_emitted == 0);
    CHECK(summary.collector_bytes_peak <= options.max_memory_bytes);

    test_name = "non-UTF8 root";
    unsigned char byte_root[PATH_MAX];
    CHECK(directory_len + 8 < sizeof(byte_root));
    memcpy(byte_root, directory, directory_len);
    byte_root[directory_len] = '/';
    const unsigned char byte_component[] = {0xfd, '-', 'r', 'o', 'o', 't'};
    memcpy(byte_root + directory_len + 1, byte_component, sizeof(byte_component));
    size_t byte_root_len = directory_len + 1 + sizeof(byte_component);
    byte_root[byte_root_len] = 0;
    CHECK(mkdir((const char*)byte_root, 0700) == 0);
    make_file((const char*)byte_root, "inside");
    options = directory_options((const char*)byte_root);
    options.root.len = byte_root_len;
    memset(&limited, 0, sizeof(limited));
    CHECK(jash_ls_collect(&options, capture, &limited, &summary) == 0);
    CHECK(summary.state == JASH_LS_TERMINAL_COMPLETE);
    CHECK(limited.count == 1 && limited.lengths[0] == strlen("inside"));
    remove_directory_contents((const char*)byte_root);
    CHECK(rmdir((const char*)byte_root) == 0);

    test_name = "cleanup";
    CHECK(close(socket_fd) == 0);
    remove_directory_contents(directory);
    CHECK(rmdir(directory) == 0);
    puts("ls collector tests passed");
    return 0;
}
