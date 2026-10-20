// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_wire.h"

#include <errno.h>
#include <limits.h>
#include <module/blake3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t used;
} wire_buffer_t;

static long buffer_sink(void* context, const void* bytes, size_t length) {
    wire_buffer_t* buffer = context;
    if (length > buffer->capacity - buffer->used || length > LONG_MAX) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(buffer->data + buffer->used, bytes, length);
    buffer->used += length;
    return (long)length;
}

static void put_u64(uint8_t bytes[8], uint64_t number) {
    for (unsigned index = 0; index < 8; index++)
        bytes[7u - index] = (uint8_t)(number >> (index * 8u));
}

static int frame(jdv_writer_t* output, uint8_t tag,
                 const uint8_t* bytes, size_t length) {
    uint8_t prefix[9];
    prefix[0] = tag;
    put_u64(prefix + 1, (uint64_t)length);
    if ((size_t)(uint64_t)length != length) { errno = EOVERFLOW; return -1; }
    return jdv_writer_write(output, prefix, sizeof(prefix)) < 0 ||
           jdv_writer_write(output, bytes, length) < 0 ? -1 : 0;
}

static int schema_frame(jdv_writer_t* output, wire_buffer_t* buffer,
                        uint8_t tag, const jdv_schema_t* schema) {
    buffer->used = 0;
    jdv_writer_t payload;
    jdv_writer_init(&payload, buffer_sink, buffer);
    if (jdv_schema_encode(schema, &payload) < 0 ||
        jdv_writer_finish(&payload) < 0) return -1;
    return frame(output, tag, buffer->data, buffer->used);
}

static int item_frame(jdv_writer_t* output, wire_buffer_t* buffer,
                      const jdv_value_t* item) {
    buffer->used = 0;
    jdv_writer_t payload;
    jdv_writer_init(&payload, buffer_sink, buffer);
    if (jdv_encode(item, &payload) < 0 ||
        jdv_writer_finish(&payload) < 0) return -1;
    return frame(output, 'I', buffer->data, buffer->used);
}

static int encode_item_v2(wire_buffer_t* buffer, const jdv_value_t* item) {
    if (buffer->capacity < 8) { errno = ENOSPC; return -1; }
    wire_buffer_t payload = {buffer->data + 8, buffer->capacity - 8, 0};
    jdv_writer_t writer;
    jdv_writer_init(&writer, buffer_sink, &payload);
    if (jdv_encode(item, &writer) < 0 || jdv_writer_finish(&writer) < 0)
        return -1;
    buffer->used = payload.used + 8;
    return 0;
}

static int encoded_item_frame_v2(jdv_writer_t* output, wire_buffer_t* buffer,
                                  uint64_t logical, struct blake3* digest) {
    uint8_t item_hash[JDV_HASH_SIZE];
    blake3_hash(buffer->data + 8, buffer->used - 8, item_hash);
    blake3_update(digest, item_hash, sizeof(item_hash));
    put_u64(buffer->data, logical);
    return frame(output, 'I', buffer->data, buffer->used);
}

static int item_frame_v2(jdv_writer_t* output, wire_buffer_t* buffer,
                         const jdv_value_t* item, uint64_t logical,
                         struct blake3* digest) {
    if (encode_item_v2(buffer, item)) return -1;
    return encoded_item_frame_v2(output, buffer, logical, digest);
}

static int terminal_frame(jdv_writer_t* output, wire_buffer_t* buffer,
                          const jdv_stream_end_t* terminal) {
    if (jdv_stream_end_validate(terminal, &JDV_DEFAULT_LIMITS) < 0) return -1;
    if (terminal->reason.len > buffer->capacity - 96) {
        errno = ENOSPC;
        return -1;
    }
    uint8_t* p = buffer->data;
    memset(p, 0, 96);
    p[0] = (uint8_t)terminal->state;
    p[1] = (uint8_t)terminal->completeness;
    p[2] = (uint8_t)terminal->truncated;
    p[3] = (uint8_t)terminal->final_hash_present;
    p[4] = (uint8_t)terminal->final_hash_algorithm;
    put_u64(p + 8, terminal->items_seen);
    put_u64(p + 16, terminal->bytes_seen);
    put_u64(p + 24, terminal->errors_seen);
    put_u64(p + 32, (uint64_t)(int64_t)terminal->producer_status);
    put_u64(p + 40, (uint64_t)(int64_t)terminal->transform_status);
    put_u64(p + 48, (uint64_t)(int64_t)terminal->transport_status);
    if (terminal->final_hash_present)
        memcpy(p + 56, terminal->final_hash, JDV_HASH_SIZE);
    put_u64(p + 88, (uint64_t)terminal->reason.len);
    if (terminal->reason.len)
        memcpy(p + 96, terminal->reason.data, terminal->reason.len);
    return frame(output, 'T', p, 96 + terminal->reason.len);
}

struct jdv_wire_push {
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    jdv_writer_t output;
    wire_buffer_t buffer;
    struct blake3 digest;
    const jdv_schema_t* guaranteed;
    jdv_limits_t limits;
    const jdv_value_t* item;
    uint64_t logical, physical, materialized;
    jdv_stream_end_t terminal;
    unsigned version;
    int opened, ending, ended, busy, error;
};

static jdv_stream_pull_result_t push_pull(void* context, jdv_stream_record_t* record) {
    jdv_wire_push_t* push = context;
    if (push->item) {
        record->item = push->item;
        record->logical_bytes = push->logical;
        record->physical_bytes = push->physical;
        record->materialized_bytes = push->materialized;
        return JDV_PULL_ITEM;
    }
    if (push->ending) {
        record->terminal = &push->terminal;
        return push->terminal.state == JDV_STREAM_COMPLETE ? JDV_PULL_END : JDV_PULL_ERROR;
    }
    return JDV_PULL_PENDING;
}
static int push_cancel(void* context, jdv_slice_t reason) { (void)context; (void)reason; return 0; }
static void push_release(void* context) { (void)context; }
static const jdv_stream_producer_vtable_t PUSH_PRODUCER = {
    JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
    push_pull, push_cancel, push_release
};
static int push_error(jdv_wire_push_t* push, int error) {
    if (push && !push->error) push->error = error ? error : EIO;
    errno = push ? push->error : error ? error : EINVAL;
    return -1;
}
static long push_discard(void* context, const void* bytes, size_t size) {
    (void)context; (void)bytes;
    return (long)size; /* jdv_writer_write has already bounded LONG_MAX. */
}
void jdv_wire_push_close(jdv_wire_push_t* push) {
    if (!push) return;
    int saved = errno;
    if (push->opened) (void)jdv_stream_release(&push->cursor);
    free(push);
    errno = saved;
}
int jdv_wire_push_open_version(unsigned version, jdv_wire_push_t** out,
                          const jdv_stream_config_t* config,
                          jdv_sink_t sink, void* context,
                          void* workspace, size_t workspace_bytes) {
    if ((version != 1 && version != 2) || !out || !config || !sink || !workspace || workspace_bytes < 96 ||
        config->cardinality != JDV_DATA_SEQUENCE || config->boundedness != JDV_STREAM_FINITE)
        return push_error(NULL, EINVAL);
    jdv_wire_push_t* push = calloc(1, sizeof(*push));
    if (!push) return -1;
    push->version = version;
    push->buffer = (wire_buffer_t){workspace, workspace_bytes, 0};
    push->limits = config->value_limits;
    push->guaranteed = config->guaranteed_schema;
    jdv_writer_init(&push->output, sink, context);
    if (jdv_stream_descriptor_init(&push->descriptor, config, &PUSH_PRODUCER, push) ||
        jdv_stream_open(&push->descriptor, &push->cursor)) {
        jdv_wire_push_close(push); return -1;
    }
    push->opened = 1;
    const jdv_schema_set_t* schemas;
    jdv_writer_t dry; jdv_writer_init(&dry, push_discard, NULL);
    if (jdv_stream_schemas(&push->cursor, &schemas) ||
        schema_frame(&dry, &push->buffer, 'D', schemas->declared_schema) ||
        schema_frame(&dry, &push->buffer, 'G', schemas->guaranteed_schema) ||
        jdv_writer_write(&push->output, version == 2 ? "JTV2" : "JTV1", 4) ||
        schema_frame(&push->output, &push->buffer, 'D', schemas->declared_schema) ||
        schema_frame(&push->output, &push->buffer, 'G', schemas->guaranteed_schema)) {
        jdv_wire_push_close(push); return -1;
    }
    blake3_init(&push->digest);
    const char* domain = version == 2 ? "JTV2-ITEMS-v1" : "JTV1-ITEMS-v1";
    blake3_update(&push->digest, domain, strlen(domain));
    *out = push;
    return 0;
}
int jdv_wire_push_item(jdv_wire_push_t* push, const jdv_value_t* item,
                          uint64_t logical_credit) {
    if (!push || !item) return push_error(push, EINVAL);
    if (push->error) return push_error(push, push->error);
    if (push->busy || push->ending || push->ended) return push_error(push, EALREADY);
    push->busy = 1;
    int rc = -1, borrowed = 0;
    if (encode_item_v2(&push->buffer, item)) goto done;
    if (push->buffer.used > UINT64_MAX - 9) { errno = EOVERFLOW; goto done; }
    push->item = item;
    push->logical = logical_credit;
    push->physical = (uint64_t)push->buffer.used + (push->version == 2 ? 9u : 1u);
    push->materialized = (uint64_t)push->buffer.used - 8;
    jdv_stream_record_t record;
    if (jdv_stream_pull(&push->cursor, &record) != JDV_PULL_ITEM) goto done;
    borrowed = 1;
    if (push->version == 2) {
        if (encoded_item_frame_v2(&push->output, &push->buffer, record.logical_bytes, &push->digest)) goto done;
    } else {
        uint8_t hash[JDV_HASH_SIZE];
        blake3_hash(push->buffer.data + 8, push->buffer.used - 8, hash);
        blake3_update(&push->digest, hash, sizeof(hash));
        if (frame(&push->output, 'I', push->buffer.data + 8, push->buffer.used - 8)) goto done;
    }
    push->item = NULL;
    /* Expire the borrow while the caller's item is still alive. PENDING is a
     * real producer boundary, not a polling loop or terminal fabrication. */
    if (jdv_stream_pull(&push->cursor, &record) != JDV_PULL_PENDING) { errno = EPROTO; goto done; }
    borrowed = 0;
    rc = 0;
done:
    push->item = NULL;
    if (borrowed) {
        int saved = errno;
        jdv_stream_record_t expired;
        (void)jdv_stream_pull(&push->cursor, &expired);
        errno = saved;
    }
    push->busy = 0;
    return rc || push->error ? push_error(push, errno) : 0;
}
int jdv_wire_push_end(jdv_wire_push_t* push, const jdv_stream_end_t* outcome,
                         jdv_stream_end_t* final) {
    if (!push || !outcome || !final) return push_error(push, EINVAL);
    if (push->error) return push_error(push, push->error);
    if (push->busy || push->ending || push->ended) return push_error(push, EALREADY);
    if (outcome->final_schema && outcome->final_schema != push->guaranteed)
        return push_error(push, EDOM);
    push->busy = 1;
    int rc = -1;
    jdv_stream_accounting_t accounting;
    if (jdv_stream_accounting(&push->cursor, &accounting)) goto done;
    push->terminal = *outcome;
    push->terminal.items_seen = accounting.items;
    push->terminal.bytes_seen = accounting.logical_bytes;
    push->terminal.final_hash_present = 1;
    push->terminal.final_hash_algorithm = JDV_HASH_BLAKE3;
    if (push->terminal.state == JDV_STREAM_COMPLETE) push->terminal.final_schema = push->guaranteed;
    blake3_final(&push->digest, push->terminal.final_hash, JDV_HASH_SIZE);
    if (jdv_stream_end_validate(&push->terminal, &push->limits)) goto done;
    jdv_writer_t dry; jdv_writer_init(&dry, push_discard, NULL);
    if (terminal_frame(&dry, &push->buffer, &push->terminal)) goto done;
    push->ending = 1;
    jdv_stream_record_t record;
    jdv_stream_pull_result_t result = jdv_stream_pull(&push->cursor, &record);
    if (result != (outcome->state == JDV_STREAM_COMPLETE ? JDV_PULL_END : JDV_PULL_ERROR) ||
        !record.terminal || record.terminal->state != outcome->state ||
        record.terminal->reason.data != outcome->reason.data ||
        record.terminal->producer_status != outcome->producer_status ||
        !record.terminal->final_hash_present) { errno = EPROTO; goto done; }
    if (record.terminal->final_schema &&
        schema_frame(&push->output, &push->buffer, 'F', record.terminal->final_schema)) goto done;
    if (terminal_frame(&push->output, &push->buffer, record.terminal) ||
        jdv_writer_finish(&push->output)) goto done;
    /* A sink must not reenter this writer. Such an attempt is sticky even if
     * the sink subsequently reports a successful write. Keep final untouched
     * for every failed call, including a failure raised inside the callback. */
    if (push->error) { errno = push->error; goto done; }
    *final = *record.terminal;
    push->ended = 1;
    rc = 0;
done:
    push->busy = 0;
    return rc || push->error ? push_error(push, errno) : 0;
}


/* The original entry points retain their JTV2 open contract. */
int jdv_wire_push_v2_open(jdv_wire_push_t** out, const jdv_stream_config_t* config,
                          jdv_sink_t sink, void* context, void* workspace,
                          size_t workspace_bytes) {
    return jdv_wire_push_open_version(2, out, config, sink, context, workspace, workspace_bytes);
}
int jdv_wire_push_v2_item(jdv_wire_push_t* push, const jdv_value_t* item, uint64_t credit) {
    return jdv_wire_push_item(push, item, credit);
}
int jdv_wire_push_v2_end(jdv_wire_push_t* push, const jdv_stream_end_t* outcome,
                         jdv_stream_end_t* final) {
    return jdv_wire_push_end(push, outcome, final);
}
void jdv_wire_push_v2_close(jdv_wire_push_t* push) { jdv_wire_push_close(push); }

static int write_stream(jdv_stream_cursor_t* cursor, jdv_sink_t sink,
                        void* sink_context, void* workspace,
                        size_t workspace_bytes, jdv_item_observer_t observer,
                        void* observer_context, jdv_stream_end_t* final,
                        int version) {
    if (!cursor || !sink || !workspace || workspace_bytes < 96 || !final) {
        errno = EINVAL;
        return -1;
    }
    wire_buffer_t buffer = {workspace, workspace_bytes, 0};
    jdv_writer_t output;
    jdv_writer_init(&output, sink, sink_context);
    const jdv_schema_set_t* schemas;
    struct blake3 digest;
    uint64_t logical_total = 0;
    if (version == 2) {
        blake3_init(&digest);
        blake3_update(&digest, "JTV2-ITEMS-v1",
                      sizeof("JTV2-ITEMS-v1") - 1u);
    }
    if (jdv_stream_schemas(cursor, &schemas) < 0 ||
        jdv_writer_write(&output, version == 2 ? "JTV2" : "JTV1", 4) < 0 ||
        schema_frame(&output, &buffer, 'D', schemas->declared_schema) < 0 ||
        schema_frame(&output, &buffer, 'G', schemas->guaranteed_schema) < 0)
        return -1;
    for (;;) {
        jdv_stream_record_t record;
        jdv_stream_pull_result_t result = jdv_stream_pull(cursor, &record);
        if (result == JDV_PULL_ITEM) {
            if (observer && observer(observer_context, record.item) < 0) {
                if (!errno) errno = EIO;
                return -1;
            }
            if (version == 2) {
                if (record.logical_bytes > UINT64_MAX - logical_total) {
                    errno = EOVERFLOW;
                    return -1;
                }
                if (item_frame_v2(&output, &buffer, record.item,
                                   record.logical_bytes, &digest) < 0)
                    return -1;
                logical_total += record.logical_bytes;
            } else if (item_frame(&output, &buffer, record.item) < 0)
                return -1;
            continue;
        }
        if (result == JDV_PULL_PENDING) { errno = EAGAIN; return -1; }
        if (!record.terminal) { if (!errno) errno = EPROTO; return -1; }
        jdv_stream_end_t terminal = *record.terminal;
        if (version == 2) {
            if (terminal.bytes_seen != logical_total) {
                errno = EBADMSG;
                return -1;
            }
            if (terminal.final_hash_present)
                blake3_final(&digest, terminal.final_hash, JDV_HASH_SIZE);
        }
        if (terminal.final_schema &&
            schema_frame(&output, &buffer, 'F',
                         terminal.final_schema) < 0) return -1;
        if (terminal_frame(&output, &buffer, &terminal) < 0 ||
            jdv_writer_finish(&output) < 0) return -1;
        *final = terminal;
        return 0;
    }
}

int jdv_stream_write_frames_observed(jdv_stream_cursor_t* cursor,
                                     jdv_sink_t sink, void* sink_context,
                                     void* workspace, size_t workspace_bytes,
                                     jdv_item_observer_t observer,
                                     void* observer_context,
                                     jdv_stream_end_t* final) {
    return write_stream(cursor, sink, sink_context, workspace,
                         workspace_bytes, observer, observer_context, final, 1);
}

int jdv_stream_write_frames_v2(jdv_stream_cursor_t* cursor, jdv_sink_t sink,
                               void* sink_context, void* workspace,
                               size_t workspace_bytes, jdv_stream_end_t* final) {
    return write_stream(cursor, sink, sink_context, workspace,
                         workspace_bytes, NULL, NULL, final, 2);
}

int jdv_stream_write_frames(jdv_stream_cursor_t* cursor, jdv_sink_t sink,
                            void* sink_context, void* workspace,
                            size_t workspace_bytes, jdv_stream_end_t* final) {
    return jdv_stream_write_frames_observed(cursor, sink, sink_context,
                                             workspace, workspace_bytes,
                                             NULL, NULL, final);
}
