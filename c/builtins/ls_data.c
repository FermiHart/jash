// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _POSIX_C_SOURCE 200809L
#include "ls_data.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

static const jdv_schema_t SCHEMA_BYTES = {
    .kind = JDV_SCHEMA_SCALAR, .as.scalar = JDV_BYTES
};
static const jdv_schema_t SCHEMA_PATH = {
    .kind = JDV_SCHEMA_SCALAR, .as.scalar = JDV_PATH
};
static const jdv_schema_t SCHEMA_SYMBOL = {
    .kind = JDV_SCHEMA_SCALAR, .as.scalar = JDV_SYMBOL
};
static const jdv_schema_t SCHEMA_NULL_SYMBOL = {
    .kind = JDV_SCHEMA_SCALAR, .nullable = 1, .as.scalar = JDV_SYMBOL
};
static const jdv_schema_t SCHEMA_NULL_BYTES = {
    .kind = JDV_SCHEMA_SCALAR, .nullable = 1, .as.scalar = JDV_BYTES
};
static const jdv_schema_t SCHEMA_NULL_U64 = {
    .kind = JDV_SCHEMA_SCALAR, .nullable = 1, .as.scalar = JDV_U64
};
static const jdv_schema_t SCHEMA_NULL_TIMESTAMP = {
    .kind = JDV_SCHEMA_SCALAR, .nullable = 1, .as.scalar = JDV_TIMESTAMP
};
static const jdv_schema_t SCHEMA_BOOL = {
    .kind = JDV_SCHEMA_SCALAR, .as.scalar = JDV_BOOL
};

#define FIELD(name, schema) { {(const uint8_t*)name, sizeof(name) - 1u}, schema, 0 }
static const jdv_schema_field_t LS_FIELDS[] = {
    FIELD("consistency", &SCHEMA_SYMBOL),
    FIELD("error_code", &SCHEMA_NULL_U64),
    FIELD("error_op", &SCHEMA_NULL_SYMBOL),
    FIELD("gid", &SCHEMA_NULL_U64),
    FIELD("mode", &SCHEMA_NULL_U64),
    FIELD("modified", &SCHEMA_NULL_TIMESTAMP),
    FIELD("name", &SCHEMA_BYTES),
    FIELD("path", &SCHEMA_PATH),
    FIELD("size", &SCHEMA_NULL_U64),
    FIELD("symlink_target", &SCHEMA_NULL_BYTES),
    FIELD("symlink_target_truncated", &SCHEMA_BOOL),
    FIELD("type", &SCHEMA_SYMBOL),
    FIELD("uid", &SCHEMA_NULL_U64)
};
#undef FIELD
static const jdv_schema_t LS_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD,
    .as.record = {LS_FIELDS, sizeof(LS_FIELDS) / sizeof(LS_FIELDS[0]), 0}
};

const jdv_schema_t* jash_ls_data_schema(void) { return &LS_SCHEMA; }

static const char* type_name(jash_ls_type_t kind) {
    switch (kind) {
        case JASH_LS_TYPE_REGULAR: return "file";
        case JASH_LS_TYPE_DIRECTORY: return "dir";
        case JASH_LS_TYPE_SYMLINK: return "symlink";
        case JASH_LS_TYPE_FIFO: return "fifo";
        case JASH_LS_TYPE_SOCKET: return "socket";
        case JASH_LS_TYPE_BLOCK: return "block";
        case JASH_LS_TYPE_CHARACTER: return "char";
        default: return "unknown";
    }
}

static const char* consistency_name(jash_ls_consistency_t consistency) {
    switch (consistency) {
        case JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND: return "descriptor";
        case JASH_LS_CONSISTENCY_PATH_FALLBACK: return "path";
        case JASH_LS_CONSISTENCY_UNSTABLE: return "unstable";
        default: return "unavailable";
    }
}

static const char* error_name(jash_ls_item_error_op_t operation) {
    switch (operation) {
        case JASH_LS_ITEM_ERROR_METADATA: return "metadata";
        case JASH_LS_ITEM_ERROR_READLINK: return "readlink";
        case JASH_LS_ITEM_ERROR_UNSTABLE: return "unstable";
        default: return NULL;
    }
}

static int make_symbol(jdv_arena_t* arena, const char* text, jdv_value_t* out) {
    return jdv_value_symbol(arena, text, strlen(text), out);
}

static jdv_value_t maybe_u64(const jash_ls_item_t* item, unsigned flag,
                             uint64_t value) {
    return item->metadata.available & flag
        ? jdv_value_u64(value) : jdv_value_nothing();
}

static jdv_value_t modified_value(const jash_ls_item_t* item) {
    jdv_value_t absent = jdv_value_nothing();
    if (!item->metadata.modified.available) return absent;
    int64_t seconds = item->metadata.modified.seconds;
    if (seconds < INT64_MIN / INT64_C(1000000000) ||
        seconds > INT64_MAX / INT64_C(1000000000)) return absent;
    int64_t base = seconds * INT64_C(1000000000);
    if (base > INT64_MAX - item->metadata.modified.nanoseconds) return absent;
    return jdv_value_timestamp(base + item->metadata.modified.nanoseconds);
}

static int make_item(jash_ls_data_t* source, const jash_ls_item_t* item) {
    jdv_arena_init(&source->arena, source->scratch, source->scratch_bytes);
    jdv_record_input_t fields[sizeof(LS_FIELDS) / sizeof(LS_FIELDS[0])];
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        fields[i].key = LS_FIELDS[i].name;
        fields[i].value = jdv_value_nothing();
    }
    if (make_symbol(&source->arena, consistency_name(item->consistency),
                    &fields[0].value) < 0 ||
        make_symbol(&source->arena, type_name(item->type),
                    &fields[11].value) < 0 ||
        jdv_value_bytes(&source->arena, item->name.data, item->name.len,
                        &fields[6].value) < 0 ||
        jdv_value_path(&source->arena, item->path.data, item->path.len,
                       &fields[7].value) < 0) return -1;
    if (item->error.operation != JASH_LS_ITEM_ERROR_NONE) {
        fields[1].value = jdv_value_u64((uint64_t)item->error.error_number);
        if (make_symbol(&source->arena, error_name(item->error.operation),
                        &fields[2].value) < 0) return -1;
    }
    fields[3].value = maybe_u64(item, JASH_LS_META_GID, item->metadata.gid);
    fields[4].value = maybe_u64(item, JASH_LS_META_MODE, item->metadata.mode);
    fields[5].value = modified_value(item);
    fields[8].value = maybe_u64(item, JASH_LS_META_SIZE, item->metadata.size);
    if (item->symlink_target_available &&
        jdv_value_bytes(&source->arena, item->symlink_target.data,
                        item->symlink_target.len, &fields[9].value) < 0) return -1;
    fields[10].value = jdv_value_bool(item->symlink_target_truncated);
    fields[12].value = maybe_u64(item, JASH_LS_META_UID, item->metadata.uid);
    return jdv_value_record(&source->arena, fields,
                             sizeof(fields) / sizeof(fields[0]), &source->value);
}

static void finish(jash_ls_data_t* source, jdv_stream_state_t state,
                   int status, int truncated) {
    (void)jdv_stream_end_init(&source->terminal);
    source->terminal.state = state;
    source->terminal.items_seen = source->items;
    source->terminal.bytes_seen = source->logical_bytes;
    source->terminal.errors_seen = source->item_errors +
        (state == JDV_STREAM_FAILED && source->item_errors != UINT64_MAX);
    source->terminal.producer_status = status;
    source->terminal.truncated = truncated;
    source->terminal.completeness = state == JDV_STREAM_COMPLETE
        ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    if (state == JDV_STREAM_COMPLETE) {
        source->terminal.final_schema = &LS_SCHEMA;
        source->terminal.final_hash_present = 1;
        source->terminal.final_hash_algorithm = JDV_HASH_BLAKE3;
        blake3_final(&source->digest, source->terminal.final_hash,
                      JDV_HASH_SIZE);
    } else if (state == JDV_STREAM_INCOMPLETE) {
        source->terminal.reason = JDV_LITERAL("bounded ls collection");
    }
}

static int open_root(jash_ls_data_t* source) {
    jash_ls_collect_options_t options = source->options_template;
    if (source->roots) {
        const char* root = source->roots[source->root_index];
        struct stat metadata;
        options.root.data = (const unsigned char*)root;
        options.root.len = strlen(root);
        options.mode = !strcmp(root, ".") ||
            (lstat(root, &metadata) == 0 && S_ISDIR(metadata.st_mode))
                ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND;
    }
    return jash_ls_cursor_open(&source->filesystem, &options);
}

static jdv_stream_pull_result_t ls_pull(void* context,
                                        jdv_stream_record_t* record) {
    jash_ls_data_t* source = context;
    const jash_ls_item_t* item = NULL;
    jash_ls_next_result_t next;
    for (;;) {
        next = jash_ls_cursor_next(&source->filesystem, &item);
        if (next != JASH_LS_NEXT_END ||
            source->root_index + 1 >= source->root_count) break;
        jash_ls_collect_summary_t completed;
        if (jash_ls_cursor_summary(&source->filesystem, &completed) < 0 ||
            completed.state != JASH_LS_TERMINAL_COMPLETE) break;
        (void)jash_ls_cursor_close(&source->filesystem);
        source->root_index++;
        if (open_root(source) < 0) {
            finish(source, JDV_STREAM_FAILED, errno, 0);
            record->terminal = &source->terminal;
            return JDV_PULL_ERROR;
        }
    }
    if (next == JASH_LS_NEXT_ITEM) {
        if (source->items == UINT64_MAX ||
            (item->error.operation != JASH_LS_ITEM_ERROR_NONE &&
             source->item_errors == UINT64_MAX)) {
            finish(source, JDV_STREAM_FAILED, EOVERFLOW, 0);
            record->terminal = &source->terminal;
            return JDV_PULL_ERROR;
        }
        if (make_item(source, item) < 0) {
            finish(source, JDV_STREAM_FAILED, errno, 0);
            source->terminal.transform_status = errno;
            record->terminal = &source->terminal;
            return JDV_PULL_ERROR;
        }
        uint8_t hash[JDV_HASH_SIZE];
        if (jdv_hash(&source->value, hash) < 0) {
            finish(source, JDV_STREAM_FAILED, errno, 0);
            source->terminal.transform_status = errno;
            record->terminal = &source->terminal;
            return JDV_PULL_ERROR;
        }
        uint64_t logical = item->name.len + item->path.len +
                           item->symlink_target.len;
        if (source->logical_bytes > UINT64_MAX - logical) {
            finish(source, JDV_STREAM_FAILED, EOVERFLOW, 0);
            record->terminal = &source->terminal;
            return JDV_PULL_ERROR;
        }
        blake3_update(&source->digest, hash, sizeof(hash));
        source->items++;
        if (item->error.operation != JASH_LS_ITEM_ERROR_NONE)
            source->item_errors++;
        source->logical_bytes += logical;
        record->item = &source->value;
        record->logical_bytes = logical;
        record->materialized_bytes = source->arena.used;
        return JDV_PULL_ITEM;
    }
    jash_ls_collect_summary_t summary;
    if (jash_ls_cursor_summary(&source->filesystem, &summary) < 0) {
        finish(source, JDV_STREAM_FAILED, errno, 0);
        record->terminal = &source->terminal;
        return JDV_PULL_ERROR;
    }
    if (next == JASH_LS_NEXT_ERROR ||
        summary.state == JASH_LS_TERMINAL_PRODUCER_ERROR ||
        summary.state == JASH_LS_TERMINAL_SINK_ERROR) {
        finish(source, JDV_STREAM_FAILED,
               summary.error_number ? summary.error_number : EIO, 0);
        record->terminal = &source->terminal;
        return JDV_PULL_ERROR;
    }
    if (summary.state == JASH_LS_TERMINAL_TRUNCATED) {
        finish(source, JDV_STREAM_INCOMPLETE, 0, 1);
        record->terminal = &source->terminal;
        return JDV_PULL_ERROR;
    }
    finish(source, JDV_STREAM_COMPLETE, 0, 0);
    record->terminal = &source->terminal;
    return JDV_PULL_END;
}

static int ls_cancel(void* context, jdv_slice_t reason) {
    (void)reason;
    return jash_ls_cursor_cancel(&((jash_ls_data_t*)context)->filesystem);
}

static void ls_release(void* context) {
    jash_ls_data_t* source = context;
    (void)jash_ls_cursor_close(&source->filesystem);
    source->active = 0;
}

static const jdv_stream_producer_vtable_t LS_VTABLE = {
    JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
    ls_pull, ls_cancel, ls_release
};

int jash_ls_data_open(jash_ls_data_t* source,
                      const jash_ls_collect_options_t* options,
                      void* scratch, size_t scratch_bytes,
                      const jdv_stream_limits_t* limits) {
    if (!source || source->active || !options || !scratch || !scratch_bytes) {
        errno = EINVAL;
        return -1;
    }
    source->scratch = scratch;
    source->scratch_bytes = scratch_bytes;
    source->items = source->logical_bytes = source->item_errors = 0;
    source->options_template = *options;
    source->roots = NULL;
    source->root_count = 1;
    source->root_index = 0;
    if (open_root(source) < 0) return -1;
    blake3_init(&source->digest);
    blake3_update(&source->digest, "JTV1-ITEMS-v1",
                  sizeof("JTV1-ITEMS-v1") - 1u);
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = config.guaranteed_schema = &LS_SCHEMA;
    if (limits) config.stream_limits = *limits;
    else {
        config.stream_limits.max_items = options->max_items;
        config.stream_limits.max_logical_bytes = options->max_logical_bytes;
    }
    if (jdv_stream_descriptor_init(&source->descriptor, &config, &LS_VTABLE,
                                   source) < 0 ||
        jdv_stream_open(&source->descriptor, &source->cursor) < 0) {
        int saved = errno;
        (void)jash_ls_cursor_close(&source->filesystem);
        errno = saved;
        return -1;
    }
    source->active = 1;
    return 0;
}

int jash_ls_data_open_many(jash_ls_data_t* source,
                           const jash_ls_collect_options_t* options,
                           const char* const* roots, size_t root_count,
                           void* scratch, size_t scratch_bytes,
                           const jdv_stream_limits_t* limits) {
    if (!source || !options || !roots || !root_count || root_count > 1024) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i] || !*roots[i] || strlen(roots[i]) >= 1024) {
            errno = E2BIG;
            return -1;
        }
    }
    jash_ls_collect_options_t first = *options;
    first.root.data = (const unsigned char*)roots[0];
    first.root.len = strlen(roots[0]);
    struct stat metadata;
    first.mode = !strcmp(roots[0], ".") ||
        (lstat(roots[0], &metadata) == 0 && S_ISDIR(metadata.st_mode))
            ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND;
    if (jash_ls_data_open(source, &first, scratch,
                           scratch_bytes, limits) < 0) return -1;
    source->options_template = *options;
    source->roots = roots;
    source->root_count = root_count;
    return 0;
}

jdv_stream_pull_result_t jash_ls_data_pull(jash_ls_data_t* source,
                                            jdv_stream_record_t* record) {
    if (!source || !source->active) { errno = EBADF; return JDV_PULL_ERROR; }
    return jdv_stream_pull(&source->cursor, record);
}

int jash_ls_data_cancel(jash_ls_data_t* source, jdv_slice_t reason,
                        const jdv_stream_end_t** terminal) {
    if (!source || !source->active) { errno = EBADF; return -1; }
    return jdv_stream_cancel(&source->cursor, reason, terminal);
}

int jash_ls_data_close(jash_ls_data_t* source) {
    if (!source) { errno = EINVAL; return -1; }
    if (!source->active) return 0;
    return jdv_stream_release(&source->cursor);
}
