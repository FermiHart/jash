// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "builtins/ls_data.h"
#include "core/data_wire_import.h"

/* CMake Release defines NDEBUG; never compile out test calls or assertions. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const jdv_value_t* field(const jdv_value_t* record, const char* key) {
    assert(record->kind == JDV_RECORD);
    for (size_t i = 0; i < record->as.record.len; i++) {
        const jdv_record_field_t* entry = &record->as.record.fields[i];
        if (entry->key.len == strlen(key) &&
            !memcmp(entry->key.data, key, entry->key.len))
            return &entry->value;
    }
    assert(!"missing schema field");
    return NULL;
}

static jash_ls_collect_options_t options_for(const char* root) {
    jash_ls_collect_options_t options = {0};
    options.root.data = (const unsigned char*)root;
    options.root.len = strlen(root);
    options.mode = JASH_LS_COLLECT_DIRECTORY;
    options.symlink_policy = JASH_LS_SYMLINK_NOFOLLOW;
    options.max_items = 10;
    options.max_logical_bytes = 65536;
    options.max_memory_bytes = 1024 * 1024;
    options.max_symlink_bytes = 128;
    return options;
}

typedef struct { unsigned char* data; size_t capacity, used; } wire_buffer_t;
static long wire_sink(void* context, const void* bytes, size_t count) {
    wire_buffer_t* buffer = context;
    if (count > buffer->capacity - buffer->used) { errno = ENOSPC; return -1; }
    memcpy(buffer->data + buffer->used, bytes, count);
    buffer->used += count;
    return (long)count;
}

int main(void) {
    char root[] = "/tmp/jash-ls-data-XXXXXX";
    assert(mkdtemp(root));
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/a", root) > 0);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    unsigned char raw[] = {0xff, '-', 'b', 0};
    assert(snprintf(path, sizeof(path), "%s/", root) > 0);
    size_t prefix = strlen(path);
    memcpy(path + prefix, raw, sizeof(raw));
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    jash_ls_collect_options_t options = options_for(root);
    jash_ls_data_t source = {0};
    unsigned char scratch[65536];
    assert(jash_ls_data_open(&source, &options, scratch, sizeof(scratch), NULL) == 0);
    jdv_stream_record_t record;
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    assert(jdv_stream_item_valid(&source.cursor, &record));
    assert(jdv_schema_accepts(jash_ls_data_schema(), record.item));
    const jdv_value_t* name = field(record.item, "name");
    assert(name->kind == JDV_BYTES && name->as.slice.len == 1);
    assert(name->as.slice.data[0] == 'a');
    const jdv_value_t* typed_path = field(record.item, "path");
    assert(typed_path->kind == JDV_PATH);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    name = field(record.item, "name");
    assert(name->kind == JDV_BYTES && name->as.slice.len == sizeof(raw) - 1);
    assert(memcmp(name->as.slice.data, raw, sizeof(raw) - 1) == 0);
    assert(field(record.item, "error_op")->kind == JDV_NOTHING);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_END);
    assert(record.terminal && record.terminal->state == JDV_STREAM_COMPLETE);
    assert(record.terminal->items_seen == 2 && record.terminal->final_hash_present);
    assert(record.terminal->final_schema == jash_ls_data_schema());
    assert(jash_ls_data_close(&source) == 0);
    assert(jash_ls_data_close(&source) == 0);

    /* JTV2 retains the non-UTF8 path, field schema and per-item logical
     * credits when brought back through an independently owned D05 cursor. */
    assert(jash_ls_data_open(&source, &options, scratch,
                              sizeof(scratch), NULL) == 0);
    unsigned char encoded[32768], workspace[32768];
    wire_buffer_t serialized = {encoded, sizeof(encoded), 0};
    jdv_stream_end_t encoded_end;
    assert(jdv_stream_write_frames_v2(&source.cursor, wire_sink,
                                       &serialized, workspace,
                                       sizeof(workspace), &encoded_end) == 0);
    assert(!memcmp(encoded, "JTV2", 4) && encoded_end.items_seen == 2);
    assert(jash_ls_data_close(&source) == 0);
    jdv_wire_limits_t wire_limits;
    jdv_wire_limits_init(&wire_limits);
    jdv_arena_t validation;
    jdv_arena_init(&validation, scratch, sizeof(scratch));
    jdv_wire_info_t checked;
    assert(jdv_wire_validate_v2(encoded, serialized.used,
                                 &validation, &wire_limits, &checked) == 0);
    assert(checked.logical_verified && checked.items_seen == 2);
    unsigned char schema_scratch[16384], item_scratch[16384];
    jdv_wire_import_t imported = {0};
    assert(jdv_wire_import_v2_open(&imported, encoded, serialized.used,
            schema_scratch, sizeof(schema_scratch), item_scratch,
            sizeof(item_scratch), &wire_limits) == 0);
    jdv_stream_record_t imported_record;
    assert(jdv_stream_pull(&imported.cursor, &imported_record) == JDV_PULL_ITEM);
    name = field(imported_record.item, "name");
    assert(name->kind == JDV_BYTES && name->as.slice.len == 1 &&
           name->as.slice.data[0] == 'a');
    assert(jdv_stream_pull(&imported.cursor, &imported_record) == JDV_PULL_ITEM);
    name = field(imported_record.item, "name");
    assert(name->kind == JDV_BYTES && name->as.slice.len == sizeof(raw) - 1);
    assert(memcmp(name->as.slice.data, raw, sizeof(raw) - 1) == 0);
    typed_path = field(imported_record.item, "path");
    assert(typed_path->kind == JDV_PATH &&
           typed_path->as.slice.data[typed_path->as.slice.len - 3] == 0xff);
    assert(jdv_stream_pull(&imported.cursor, &imported_record) == JDV_PULL_END);
    assert(imported_record.terminal->bytes_seen == checked.bytes_seen);
    assert(imported_record.terminal->state == JDV_STREAM_COMPLETE);
    assert(jdv_wire_import_close(&imported) == 0);

    options.max_items = 1;
    assert(jash_ls_data_open(&source, &options, scratch, sizeof(scratch), NULL) == 0);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ERROR);
    assert(record.terminal && record.terminal->state == JDV_STREAM_INCOMPLETE);
    assert(record.terminal->truncated && record.terminal->items_seen == 1);
    assert(jash_ls_data_close(&source) == 0);

    options.max_items = 10;
    assert(jash_ls_data_open(&source, &options, scratch, sizeof(scratch), NULL) == 0);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    const jdv_stream_end_t* cancelled;
    assert(jash_ls_data_cancel(&source, JDV_LITERAL("early stop"), &cancelled) == 0);
    assert(cancelled && cancelled->state == JDV_STREAM_CANCELLED);
    assert(jash_ls_data_close(&source) == 0);

    char nested[512];
    assert(snprintf(nested, sizeof(nested), "%s/nested", root) > 0);
    assert(mkdir(nested, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/nested/inside", root) > 0);
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && close(fd) == 0);
    const char* roots[] = {root, nested};
    assert(jash_ls_data_open_many(&source, &options, roots, 2,
                                  scratch, sizeof(scratch), NULL) == 0);
    int rows = 0;
    while (jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM) rows++;
    assert(record.terminal && record.terminal->state == JDV_STREAM_COMPLETE);
    assert(record.terminal->items_seen == 4 && rows == 4);
    assert(jash_ls_data_close(&source) == 0);

    options.max_items = 1;
    const char* limited_roots[] = {nested, root};
    assert(jash_ls_data_open_many(&source, &options, limited_roots, 2,
                                  scratch, sizeof(scratch), NULL) == 0);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ERROR);
    assert(record.terminal && record.terminal->state != JDV_STREAM_COMPLETE);
    assert(record.terminal->items_seen == 1);
    assert(jash_ls_data_close(&source) == 0);
    options.max_items = 10;
    assert(unlink(path) == 0 && rmdir(nested) == 0);

    assert(snprintf(path, sizeof(path), "%s/missing", root) > 0);
    options = options_for(path);
    options.mode = JASH_LS_COLLECT_OPERAND;
    assert(jash_ls_data_open(&source, &options, scratch, sizeof(scratch), NULL) == 0);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ITEM);
    assert(field(record.item, "error_code")->kind == JDV_U64);
    assert(field(record.item, "error_op")->kind == JDV_SYMBOL);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_END);
    assert(record.terminal->state == JDV_STREAM_COMPLETE);
    assert(record.terminal->errors_seen == 1);
    assert(jash_ls_data_close(&source) == 0);

    options = options_for(root);
    unsigned char tiny[32];
    assert(jash_ls_data_open(&source, &options, tiny, sizeof(tiny), NULL) == 0);
    assert(jash_ls_data_pull(&source, &record) == JDV_PULL_ERROR);
    assert(record.terminal && record.terminal->state == JDV_STREAM_FAILED);
    assert(record.terminal->transform_status != 0);
    assert(jash_ls_data_close(&source) == 0);

    assert(snprintf(path, sizeof(path), "%s/", root) > 0);
    prefix = strlen(path);
    memcpy(path + prefix, raw, sizeof(raw));
    assert(unlink(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/a", root) > 0);
    assert(unlink(path) == 0);
    assert(rmdir(root) == 0);
    puts("ls data: ok");
    return 0;
}
