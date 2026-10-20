// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "builtins/ls_data.h"
#include "core/data_filter.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "data filter check failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

static const jdv_value_t* field(const jdv_value_t* record, const char* name) {
    for (size_t i = 0; i < record->as.record.len; i++) {
        const jdv_record_field_t* entry = &record->as.record.fields[i];
        if (entry->key.len == strlen(name) &&
            !memcmp(entry->key.data, name, entry->key.len))
            return &entry->value;
    }
    return NULL;
}

int main(void) {
    char root[] = "/tmp/jash-data-filter-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/a", root) > 0);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/d", root) > 0);
    CHECK(mkdir(path, 0700) == 0);
    jash_ls_collect_options_t options = {0};
    options.root.data = (const unsigned char*)root;
    options.root.len = strlen(root);
    options.mode = JASH_LS_COLLECT_DIRECTORY;
    options.symlink_policy = JASH_LS_SYMLINK_NOFOLLOW;
    options.max_items = 10;
    options.max_logical_bytes = 65536;
    options.max_memory_bytes = 1024 * 1024;
    unsigned char storage[16384];
    jash_ls_data_t source = {0};
    CHECK(jash_ls_data_open(&source, &options, storage,
                            sizeof(storage), NULL) == 0);
    jdv_value_t expected = {0};
    expected.kind = JDV_SYMBOL;
    expected.as.slice = JDV_LITERAL("file");
    jdv_filter_t filter = {0};
    CHECK(jdv_filter_equal_open(&filter, &source.cursor,
                                JDV_LITERAL("not_a_field"), &expected) < 0);
    CHECK(errno == EDOM);
    CHECK(jdv_filter_equal_open(&filter, &source.cursor,
                                JDV_LITERAL("size"), &expected) < 0);
    CHECK(errno == EDOM);
    CHECK(jdv_filter_equal_open(&filter, &source.cursor,
                                JDV_LITERAL("type"), &expected) == 0);
    jdv_stream_record_t record;
    CHECK(jdv_stream_pull(&filter.cursor, &record) == JDV_PULL_ITEM);
    const jdv_value_t* name = field(record.item, "name");
    CHECK(name && name->kind == JDV_BYTES && name->as.slice.len == 1);
    CHECK(name->as.slice.data[0] == 'a');
    CHECK(jdv_stream_item_valid(&filter.cursor, &record));
    jdv_stream_pull_result_t end_result = jdv_stream_pull(&filter.cursor, &record);
    if (end_result != JDV_PULL_END)
        fprintf(stderr, "filter terminal: result=%d state=%d items=%llu bytes=%llu transform=%d errno=%d\n",
                end_result, record.terminal ? (int)record.terminal->state : -1,
                record.terminal ? (unsigned long long)record.terminal->items_seen : 0,
                record.terminal ? (unsigned long long)record.terminal->bytes_seen : 0,
                record.terminal ? record.terminal->transform_status : -1, errno);
    CHECK(end_result == JDV_PULL_END);
    CHECK(record.terminal && record.terminal->state == JDV_STREAM_COMPLETE);
    CHECK(record.terminal->items_seen == 1 && record.terminal->final_hash_present);
    CHECK(jdv_filter_close(&filter) == 0);
    CHECK(source.active == 0);
    CHECK(jdv_filter_close(&filter) == 0);

    CHECK(jash_ls_data_open(&source, &options, storage,
                            sizeof(storage), NULL) == 0);
    expected.as.slice = JDV_LITERAL("socket");
    CHECK(jdv_filter_equal_open(&filter, &source.cursor,
                                JDV_LITERAL("type"), &expected) == 0);
    CHECK(jdv_stream_pull(&filter.cursor, &record) == JDV_PULL_END);
    CHECK(record.terminal && record.terminal->items_seen == 0);
    CHECK(jdv_filter_close(&filter) == 0 && source.active == 0);

    CHECK(rmdir(path) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/a", root) > 0);
    CHECK(unlink(path) == 0 && rmdir(root) == 0);
    puts("data filter: ok");
    return 0;
}
