// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "ai_observation.h"

#include "builtins/ls_data.h"
#include "core/data_filter.h"
#include "core/data_wire.h"
#include "core/json_writer.h"
#include "osif/buffer.h"
#include "osif/osif_stat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef AI_OBSERVATION_REF_MAX
#define AI_OBSERVATION_REF_MAX (8u * 1024u * 1024u)
#endif

typedef struct {
    uint8_t* bytes;
    size_t size;
    size_t capacity;
} observation_buffer_t;

typedef struct {
    const uint8_t* bytes;
    size_t size;
    int sealed_fd;
    char reference[128];
    char path[1024];
    struct stat identity;
    uint64_t generation;
} observation_cache_t;

static observation_cache_t g_observation = {.sealed_fd = -1};

static long observation_sink(void* context, const void* data, size_t count) {
    observation_buffer_t* buffer = context;
    if (count > AI_OBSERVATION_REF_MAX - buffer->size || count > LONG_MAX) {
        errno = E2BIG;
        return -1;
    }
    size_t required = buffer->size + count;
    if (required > buffer->capacity) {
        size_t next = buffer->capacity ? buffer->capacity : 4096;
        while (next < required) {
            if (next > AI_OBSERVATION_REF_MAX / 2u) {
                next = AI_OBSERVATION_REF_MAX;
                break;
            }
            next *= 2u;
        }
        if (next < required) { errno = E2BIG; return -1; }
        uint8_t* grown = realloc(buffer->bytes, next);
        if (!grown) return -1;
        buffer->bytes = grown;
        buffer->capacity = next;
    }
    memcpy(buffer->bytes + buffer->size, data, count);
    buffer->size += count;
    return (long)count;
}

static int new_reference(char out[128], uint64_t generation,
                         const uint8_t* bytes, size_t count) {
    uint8_t nonce[16], hash[JDV_HASH_SIZE];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < sizeof(nonce)) {
        ssize_t n = read(fd, nonce + got, sizeof(nonce) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int saved = n < 0 ? errno : EIO;
            close(fd);
            errno = saved;
            return -1;
        }
        got += (size_t)n;
    }
    if (close(fd) != 0) return -1;
    blake3_hash(bytes, count, hash);
    static const char hex[] = "0123456789abcdef";
    char nonce_hex[33], hash_hex[65];
    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce_hex[2u * i] = hex[nonce[i] >> 4];
        nonce_hex[2u * i + 1u] = hex[nonce[i] & 15u];
    }
    nonce_hex[32] = '\0';
    for (size_t i = 0; i < sizeof(hash); i++) {
        hash_hex[2u * i] = hex[hash[i] >> 4];
        hash_hex[2u * i + 1u] = hex[hash[i] & 15u];
    }
    hash_hex[64] = '\0';
    int n = snprintf(out, 128, "o:%s-%016llx-%s", nonce_hex,
                     (unsigned long long)generation, hash_hex);
    if (n < 0 || n >= 128) { errno = EOVERFLOW; return -1; }
    return 0;
}

void jash_ai_observation_forget(void) {
    if (g_observation.bytes)
        jash_buffer_unmap(g_observation.bytes, g_observation.size);
    if (g_observation.sealed_fd >= 0) (void)close(g_observation.sealed_fd);
    g_observation.bytes = NULL;
    g_observation.size = 0;
    g_observation.sealed_fd = -1;
    g_observation.reference[0] = '\0';
    g_observation.path[0] = '\0';
}

static const jdv_value_t* record_field(const jdv_value_t* record,
                                        const char* name) {
    if (!record || record->kind != JDV_RECORD) return NULL;
    size_t length = strlen(name);
    for (size_t i = 0; i < record->as.record.len; i++) {
        const jdv_record_field_t* field = &record->as.record.fields[i];
        if (field->key.len == length &&
            !memcmp(field->key.data, name, length)) return &field->value;
    }
    return NULL;
}

static int same_directory(const struct stat* before, const struct stat* after) {
    return before->st_dev == after->st_dev &&
           before->st_ino == after->st_ino &&
           BEAR_ST_MTIM(*before).tv_sec == BEAR_ST_MTIM(*after).tv_sec &&
           BEAR_ST_MTIM(*before).tv_nsec == BEAR_ST_MTIM(*after).tv_nsec &&
           BEAR_ST_CTIM(*before).tv_sec == BEAR_ST_CTIM(*after).tv_sec &&
           BEAR_ST_CTIM(*before).tv_nsec == BEAR_ST_CTIM(*after).tv_nsec;
}

int jash_ai_observation_describe(char* out, size_t capacity) {
    static const char description[] =
        "{\"version\":1,\"operations\":[{\"name\":\"fs.list\",\"effect\":\"read\","
        "\"schema\":\"jash.ls.data.v1\",\"preview_items\":8,"
        "\"model_budget_bytes\":4096,\"freshness\":\"best_effort\","
        "\"filters\":[\"type\",\"uid\"]},"
        "{\"name\":\"fs.count\",\"effect\":\"read\","
        "\"schema\":\"jash.fs-count.v1\",\"model_budget_bytes\":4096,"
        "\"filters\":[\"type\",\"uid\"]},"
        "{\"name\":\"fs.source.read\",\"effect\":\"read\","
        "\"schema\":\"jash.source-page.v1\",\"max_page_bytes\":1024,"
        "\"retention\":\"live-best-effort\"},"
        "{\"name\":\"fs.observation.read\",\"effect\":\"read\","
        "\"schema\":\"jash.observation-page.v1\",\"max_page_bytes\":1024,"
        "\"retention\":\"volatile\"}],"
        "\"repair_cell_schema\":\"jash.ai.cell.v1\","
        "\"repair_cell_transport\":\"jsonl-opt-in\"}";
    if (!out || !capacity) { errno = EINVAL; return -1; }
    out[0] = '\0';
    if (capacity < sizeof(description)) { errno = ENOSPC; return -1; }
    memcpy(out, description, sizeof(description));
    return 0;
}

typedef struct {
    jash_json_writer_t* writer;
    uint64_t preview;
} projection_t;

static int observe_preview(void* context, const jdv_value_t* item) {
    projection_t* projection = context;
    if (projection->preview >= 8) return 0;
    const jdv_value_t* name = record_field(item, "name");
    if (!name || name->kind != JDV_BYTES ||
        name->as.slice.len >= NAME_MAX + 1u) {
        errno = EPROTO;
        return -1;
    }
    char printable[NAME_MAX + 1];
    memcpy(printable, name->as.slice.data, name->as.slice.len);
    printable[name->as.slice.len] = '\0';
    if (projection->preview) jash_jsonw_raw(projection->writer, ",");
    jash_jsonw_raw(projection->writer, "{\"name\":");
    jash_jsonw_string_or_bytes(projection->writer, printable);
    jash_jsonw_raw(projection->writer, "}");
    if (projection->writer->error) {
        errno = projection->writer->error;
        return -1;
    }
    projection->preview++;
    return 0;
}

static int observation_ref_live(const char* ref) {
    if (!ref || strnlen(ref, sizeof(g_observation.reference)) >=
                   sizeof(g_observation.reference)) {
        errno = EINVAL;
        return -1;
    }
    if (!g_observation.bytes) { errno = ENOENT; return -1; }
    if (strcmp(ref, "latest") &&
        strcmp(ref, g_observation.reference)) { errno = ESTALE; return -1; }
    if (jash_buffer_validate(g_observation.sealed_fd, g_observation.size,
                             AI_OBSERVATION_REF_MAX) < 0) return -1;
    struct stat current;
    if (lstat(g_observation.path, &current) != 0 ||
        !same_directory(&g_observation.identity, &current)) {
        errno = ESTALE;
        return -1;
    }
    return 0;
}

int jash_ai_observation_reader_fd(const char* ref, uint64_t* size) {
    if (!size) { errno = EINVAL; return -1; }
    if (observation_ref_live(ref) < 0) return -1;
    int fd = jash_buffer_reader_fd(g_observation.sealed_fd,
                                   g_observation.size, AI_OBSERVATION_REF_MAX);
    if (fd < 0) return -1;
    struct stat current;
    if (lstat(g_observation.path, &current) != 0 ||
        !same_directory(&g_observation.identity, &current)) {
        close(fd);
        errno = ESTALE;
        return -1;
    }
    *size = g_observation.size;
    return fd;
}

int jash_ai_observation_read_json(const char* ref, size_t offset,
                                  size_t limit, char* out, size_t capacity) {
    if (!out || !capacity) { errno = EINVAL; return -1; }
    out[0] = '\0';
    if (!limit || limit > 1024) {
        errno = EINVAL;
        return -1;
    }
    if (observation_ref_live(ref) < 0) return -1;
    if (offset > g_observation.size) { errno = ERANGE; return -1; }
    size_t amount = g_observation.size - offset;
    if (amount > limit) amount = limit;
    char result[4096];
    jash_json_buffer_t buffer = {result, sizeof(result), 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_jsonw_raw(&writer,
        "{\"schema\":\"jash.observation-page.v1\",\"observation_ref\":");
    jash_jsonw_string(&writer, g_observation.reference);
    jash_jsonw_raw(&writer, ",\"offset\":");
    jash_jsonw_u64(&writer, offset);
    jash_jsonw_raw(&writer, ",\"bytes_returned\":");
    jash_jsonw_u64(&writer, amount);
    jash_jsonw_raw(&writer, ",\"total_bytes\":");
    jash_jsonw_u64(&writer, g_observation.size);
    jash_jsonw_raw(&writer, ",\"eof\":");
    jash_jsonw_raw(&writer,
                    offset + amount == g_observation.size ? "true" : "false");
    jash_jsonw_raw(&writer, ",\"data\":");
    jash_jsonw_bytes(&writer, g_observation.bytes + offset, amount);
    jash_jsonw_raw(&writer, "}");
    if (jash_jsonw_finish(&writer) < 0) return -1;
    if (buffer.used >= capacity) { errno = ENOSPC; return -1; }
    memcpy(out, result, buffer.used + 1);
    return 0;
}

static int admitted_type(const char* type) {
    static const char* const types[] = {
        "file", "dir", "symlink", "fifo", "socket", "block", "char", "unknown"
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (!strcmp(type, types[i])) return 1;
    return 0;
}

int jash_ai_observe_ls_where(const char* root, const char* type,
                             int has_uid, uint64_t uid,
                             char* out, size_t capacity) {
    if (!out || !capacity) { errno = EINVAL; return -1; }
    out[0] = '\0';
    if (!root || !*root || strlen(root) >= 1024 ||
        (has_uid != 0 && has_uid != 1) || (has_uid && uid > UINT32_MAX) ||
        (type && !admitted_type(type))) {
        errno = EINVAL;
        return -1;
    }
    struct stat before;
    if (lstat(root, &before) != 0) return -1;

    jash_ls_collect_options_t options = {
        .root = {(const unsigned char*)root, strlen(root)},
        .mode = S_ISDIR(before.st_mode)
            ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW,
        .max_items = 65536,
        .max_logical_bytes = 32u * 1024u * 1024u,
        .max_memory_bytes = 32u * 1024u * 1024u,
        .max_symlink_bytes = 1024
    };
    unsigned char scratch[32768];
    jash_ls_data_t source = {0};
    if (jash_ls_data_open(&source, &options, scratch,
                           sizeof(scratch), NULL) < 0) return -1;

    jdv_filter_t type_filter = {0}, uid_filter = {0};
    if (type) {
        jdv_value_t expected = {0};
        expected.kind = JDV_SYMBOL;
        expected.as.slice.data = (const uint8_t*)type;
        expected.as.slice.len = strlen(type);
        if (jdv_filter_equal_open(&type_filter, &source.cursor,
                                  JDV_LITERAL("type"), &expected) < 0) {
            int saved = errno;
            (void)jash_ls_data_close(&source);
            errno = saved;
            return -1;
        }
    }
    if (has_uid) {
        jdv_value_t expected = jdv_value_u64(uid);
        /* INV-868: filter input is one of two live local cursors. Keep the
         * provenance explicit at the call boundary for Bear. */
        int opened = type
            ? jdv_filter_equal_open(&uid_filter, &type_filter.cursor,
                                    JDV_LITERAL("uid"), &expected)
            : jdv_filter_equal_open(&uid_filter, &source.cursor,
                                    JDV_LITERAL("uid"), &expected);
        if (opened < 0) {
            int saved = errno;
            if (type) (void)jdv_filter_close(&type_filter);
            else (void)jash_ls_data_close(&source);
            errno = saved;
            return -1;
        }
    }

    char result[4096];
    jash_json_buffer_t buffer = {result, sizeof(result), 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_jsonw_raw(&writer, "{\"operation\":\"fs.list\",\"schema\":\"jash.ls.data.v1\",\"root\":");
    jash_jsonw_string_or_bytes(&writer, root);
    if (type || has_uid) {
        jash_jsonw_raw(&writer, ",\"filter\":{");
        if (type) {
            jash_jsonw_raw(&writer, "\"type\":");
            jash_jsonw_string(&writer, type);
        }
        if (has_uid) {
            if (type) jash_jsonw_raw(&writer, ",");
            jash_jsonw_raw(&writer, "\"uid\":");
            jash_jsonw_u64(&writer, uid);
        }
        jash_jsonw_raw(&writer, "}");
    }
    jash_jsonw_raw(&writer, ",\"rows_preview\":[");
    observation_buffer_t candidate = {0};
    projection_t projection = {&writer, 0};
    unsigned char workspace[32768];
    jdv_stream_end_t terminal;
    int error = 0;
    /* INV-868: write through the exact live cursor, never a pointer joining
     * three stack objects before a call with escaping state. */
    int wrote;
    if (has_uid)
        wrote = jdv_stream_write_frames_observed(&uid_filter.cursor, observation_sink,
            &candidate, workspace, sizeof(workspace), observe_preview,
            &projection, &terminal);
    else if (type)
        wrote = jdv_stream_write_frames_observed(&type_filter.cursor, observation_sink,
            &candidate, workspace, sizeof(workspace), observe_preview,
            &projection, &terminal);
    else
        wrote = jdv_stream_write_frames_observed(&source.cursor, observation_sink,
            &candidate, workspace, sizeof(workspace), observe_preview,
            &projection, &terminal);
    if (wrote < 0) {
        error = errno;
        goto cleanup;
    }
    struct stat after;
    int stable = lstat(root, &after) == 0 &&
        same_directory(&before, &after);
    int publish = stable && terminal.state == JDV_STREAM_COMPLETE;
    char new_ref[128] = "";
    uint64_t generation = g_observation.generation + 1;
    if (publish && (!generation ||
        new_reference(new_ref, generation,
                      candidate.bytes, candidate.size) < 0)) {
        error = generation ? errno : EOVERFLOW;
        goto cleanup;
    }
    jash_jsonw_raw(&writer, "],\"items_seen\":");
    jash_jsonw_u64(&writer, terminal.items_seen);
    jash_jsonw_raw(&writer, ",\"errors_seen\":");
    jash_jsonw_u64(&writer, terminal.errors_seen);
    jash_jsonw_raw(&writer, ",\"projection_truncated\":");
    jash_jsonw_raw(&writer,
        terminal.items_seen > projection.preview ? "true" : "false");
    jash_jsonw_raw(&writer, ",\"complete\":");
    jash_jsonw_raw(&writer, publish ? "true" : "false");
    jash_jsonw_raw(&writer, ",\"freshness\":");
    jash_jsonw_string(&writer, stable ? "best_effort" : "changed");
    jash_jsonw_raw(&writer, ",\"content_hash\":");
    if (terminal.final_hash_present) {
        static const char hex[] = "0123456789abcdef";
        char hash[JDV_HASH_SIZE * 2u + 1u];
        for (size_t i = 0; i < JDV_HASH_SIZE; i++) {
            hash[2u * i] = hex[terminal.final_hash[i] >> 4];
            hash[2u * i + 1u] = hex[terminal.final_hash[i] & 15u];
        }
        hash[JDV_HASH_SIZE * 2u] = '\0';
        jash_jsonw_string(&writer, hash);
    } else jash_jsonw_raw(&writer, "null");
    jash_jsonw_raw(&writer, ",\"observation_ref\":");
    if (publish) jash_jsonw_string(&writer, new_ref);
    else jash_jsonw_raw(&writer, "null");
    jash_jsonw_raw(&writer, ",\"stored_bytes\":");
    jash_jsonw_u64(&writer, publish ? candidate.size : 0);
    jash_jsonw_raw(&writer, ",\"ref_blake3\":");
    if (publish) {
        const char* suffix = strrchr(new_ref, '-');
        jash_jsonw_string(&writer, suffix ? suffix + 1 : "");
    } else jash_jsonw_raw(&writer, "null");
    jash_jsonw_raw(&writer, "}");
    if (jash_jsonw_finish(&writer) < 0) {
        error = errno;
        goto cleanup;
    }
    if (buffer.used >= capacity) { error = ENOSPC; goto cleanup; }
    if (publish) {
        int sealed = jash_buffer_create(candidate.bytes, candidate.size,
                                        AI_OBSERVATION_REF_MAX);
        if (sealed < 0) { error = errno; goto cleanup; }
        const void* mapped = NULL;
        if (jash_buffer_map(sealed, candidate.size,
                             AI_OBSERVATION_REF_MAX, &mapped) < 0) {
            error = errno;
            close(sealed);
            goto cleanup;
        }
        jdv_wire_limits_t wire_limits;
        jdv_wire_limits_init(&wire_limits);
        wire_limits.max_wire_bytes = AI_OBSERVATION_REF_MAX;
        wire_limits.max_frame_bytes = sizeof(workspace);
        jdv_arena_t verification;
        jdv_arena_init(&verification, scratch, sizeof(scratch));
        jdv_wire_info_t verified;
        if (jdv_wire_validate(mapped, candidate.size, &verification,
                               &wire_limits, &verified) < 0 ||
            verified.state != JDV_STREAM_COMPLETE ||
            verified.items_seen != terminal.items_seen ||
            verified.bytes_seen != terminal.bytes_seen ||
            verified.errors_seen != terminal.errors_seen ||
            !verified.final_hash_present ||
            memcmp(verified.final_hash, terminal.final_hash,
                   JDV_HASH_SIZE)) {
            error = errno ? errno : EBADMSG;
            jash_buffer_unmap(mapped, candidate.size);
            close(sealed);
            goto cleanup;
        }
        jash_ai_observation_forget();
        g_observation.bytes = mapped;
        g_observation.size = candidate.size;
        g_observation.sealed_fd = sealed;
        g_observation.identity = after;
        g_observation.generation = generation;
        memcpy(g_observation.reference, new_ref, strlen(new_ref) + 1u);
        memcpy(g_observation.path, root, strlen(root) + 1u);
    }
    memcpy(out, result, buffer.used + 1);
  cleanup:
    if (has_uid) (void)jdv_filter_close(&uid_filter);
    else if (type) (void)jdv_filter_close(&type_filter);
    else (void)jash_ls_data_close(&source);
    if (candidate.bytes) {
        volatile uint8_t* bytes = candidate.bytes;
        for (size_t i = 0; i < candidate.size; i++) bytes[i] = 0;
        free(candidate.bytes);
    }
    if (error) { errno = error; return -1; }
    return 0;
}

/* Keep the unfiltered ABI stable for existing module clients. */
int jash_ai_observe_ls(const char* root, char* out, size_t capacity) {
    return jash_ai_observe_ls_filtered(root, NULL, out, capacity);
}

int jash_ai_observe_ls_filtered(const char* root, const char* type,
                                char* out, size_t capacity) {
    return jash_ai_observe_ls_where(root, type, 0, 0, out, capacity);
}

int jash_ai_count_ls_where(const char* root, const char* type,
                           int has_uid, uint64_t uid,
                           char* out, size_t capacity) {
    if (!out || !capacity) { errno = EINVAL; return -1; }
    out[0] = '\0';
    if (!root || !*root || strlen(root) >= 1024 ||
        (has_uid != 0 && has_uid != 1) || (has_uid && uid > UINT32_MAX) ||
        (type && !admitted_type(type))) { errno = EINVAL; return -1; }
    struct stat before;
    if (lstat(root, &before) < 0) return -1;
    jash_ls_collect_options_t options = {
        .root = {(const unsigned char*)root, strlen(root)},
        .mode = S_ISDIR(before.st_mode)
            ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW,
        .max_items = 65536,
        .max_logical_bytes = 32u * 1024u * 1024u,
        .max_memory_bytes = 32u * 1024u * 1024u,
        .max_symlink_bytes = 1024
    };
    unsigned char scratch[32768];
    jash_ls_data_t source = {0};
    if (jash_ls_data_open(&source, &options, scratch,
                           sizeof(scratch), NULL) < 0) return -1;
    jdv_filter_t by_type = {0}, by_uid = {0};
    if (type) {
        jdv_value_t expected = {0};
        expected.kind = JDV_SYMBOL;
        expected.as.slice.data = (const uint8_t*)type;
        expected.as.slice.len = strlen(type);
        if (jdv_filter_equal_open(&by_type, &source.cursor,
                                  JDV_LITERAL("type"), &expected) < 0)
            goto finish;
    }
    if (has_uid) {
        jdv_value_t expected = jdv_value_u64(uid);
        int opened = type
            ? jdv_filter_equal_open(&by_uid, &by_type.cursor,
                                    JDV_LITERAL("uid"), &expected)
            : jdv_filter_equal_open(&by_uid, &source.cursor,
                                    JDV_LITERAL("uid"), &expected);
        if (opened < 0)
            goto finish;
    }
    jdv_stream_end_t terminal = {0};
    for (;;) {
        jdv_stream_record_t record;
        jdv_stream_pull_result_t pulled;
        if (has_uid) pulled = jdv_stream_pull(&by_uid.cursor, &record);
        else if (type) pulled = jdv_stream_pull(&by_type.cursor, &record);
        else pulled = jdv_stream_pull(&source.cursor, &record);
        if (pulled == JDV_PULL_ITEM) continue;
        if (pulled == JDV_PULL_PENDING) { errno = EAGAIN; goto finish; }
        if (pulled != JDV_PULL_END || !record.terminal ||
            record.terminal->state != JDV_STREAM_COMPLETE) {
            errno = EPROTO;
            goto finish;
        }
        terminal = *record.terminal;
        break;
    }
    struct stat after;
    int stable = lstat(root, &after) == 0 &&
                 same_directory(&before, &after);
    char result[4096];
    jash_json_buffer_t buffer = {result, sizeof(result), 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_jsonw_raw(&writer,
        "{\"operation\":\"fs.count\",\"schema\":\"jash.fs-count.v1\",\"root\":");
    jash_jsonw_string_or_bytes(&writer, root);
    if (type || has_uid) {
        jash_jsonw_raw(&writer, ",\"filter\":{");
        if (type) {
            jash_jsonw_raw(&writer, "\"type\":");
            jash_jsonw_string(&writer, type);
        }
        if (has_uid) {
            if (type) jash_jsonw_raw(&writer, ",");
            jash_jsonw_raw(&writer, "\"uid\":");
            jash_jsonw_u64(&writer, uid);
        }
        jash_jsonw_raw(&writer, "}");
    }
    jash_jsonw_raw(&writer, ",\"items_seen\":");
    jash_jsonw_u64(&writer, terminal.items_seen);
    jash_jsonw_raw(&writer, ",\"errors_seen\":");
    jash_jsonw_u64(&writer, terminal.errors_seen);
    jash_jsonw_raw(&writer, ",\"complete\":");
    jash_jsonw_raw(&writer, stable && !terminal.errors_seen ? "true" : "false");
    jash_jsonw_raw(&writer, ",\"freshness\":");
    jash_jsonw_string(&writer, stable ? "best_effort" : "changed");
    jash_jsonw_raw(&writer, "}");
    if (jash_jsonw_finish(&writer) < 0 || buffer.used >= capacity) {
        if (!errno) errno = ENOSPC;
        goto finish;
    }
    memcpy(out, result, buffer.used + 1u);
    if (has_uid) (void)jdv_filter_close(&by_uid);
    else if (type) (void)jdv_filter_close(&by_type);
    else (void)jash_ls_data_close(&source);
    return 0;
 finish: {
        int saved = errno ? errno : EIO;
        if (by_uid.active) (void)jdv_filter_close(&by_uid);
        else if (by_type.active) (void)jdv_filter_close(&by_type);
        else (void)jash_ls_data_close(&source);
        errno = saved;
        return -1;
    }
}
