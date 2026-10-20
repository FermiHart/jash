// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_wire_import.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static int fail(int error) { errno = error; return -1; }

static uint64_t read_u64(const uint8_t* bytes) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; i++) result = (result << 8) | bytes[i];
    return result;
}

static int64_t read_i64(const uint8_t* bytes) {
    uint64_t value = read_u64(bytes);
    if (value <= INT64_MAX) return (int64_t)value;
    return -INT64_C(1) - (int64_t)(UINT64_MAX - value);
}

static int read_status(const uint8_t* bytes, int* out) {
    int64_t status = read_i64(bytes);
    if (status < INT_MIN || status > INT_MAX) return fail(EBADMSG);
    *out = (int)status;
    return 0;
}

static int overlaps(const void* a, size_t a_size,
                    const void* b, size_t b_size) {
    uintptr_t left = (uintptr_t)a, right = (uintptr_t)b;
    if (a_size > UINTPTR_MAX - left || b_size > UINTPTR_MAX - right)
        return 1;
    return a_size && b_size && left < right + b_size && right < left + a_size;
}

static int next_frame(const jdv_wire_import_t* source, size_t* position,
                      uint8_t* tag, const uint8_t** payload, size_t* length) {
    if (*position > source->size || source->size - *position < 9)
        return fail(EBADMSG);
    *tag = source->bytes[(*position)++];
    uint64_t count = read_u64(source->bytes + *position);
    *position += 8;
    if (count > SIZE_MAX || count > source->limits.max_frame_bytes)
        return fail(E2BIG);
    *length = (size_t)count;
    if (*length > source->size - *position) return fail(EBADMSG);
    *payload = source->bytes + *position;
    *position += *length;
    return 0;
}

static jdv_stream_pull_result_t import_pull(void* context,
                                            jdv_stream_record_t* record) {
    jdv_wire_import_t* source = context;
    size_t next = source->position;
    uint8_t tag;
    const uint8_t* payload;
    size_t length;
    if (next_frame(source, &next, &tag, &payload, &length) < 0)
        return JDV_PULL_ERROR;
    if (tag == 'I') {
        if (length < 8) { errno = EBADMSG; return JDV_PULL_ERROR; }
        uint64_t logical = read_u64(payload);
        source->items.used = 0;
        if (jdv_decode_exact_limited(&source->items, payload + 8, length - 8,
                                      &source->item, NULL,
                                      &source->limits.value_limits) < 0)
            return JDV_PULL_ERROR;
        source->position = next;
        record->item = &source->item;
        record->logical_bytes = logical;
        record->physical_bytes = 9u + length;
        record->materialized_bytes = source->items.used;
        return JDV_PULL_ITEM;
    }
    if (tag == 'F') {
        if (next_frame(source, &next, &tag, &payload, &length) < 0)
            return JDV_PULL_ERROR;
    }
    if (tag != 'T' || next != source->size) {
        errno = EBADMSG;
        return JDV_PULL_ERROR;
    }
    source->position = next;
    record->terminal = &source->terminal;
    return source->terminal.state == JDV_STREAM_COMPLETE
        ? JDV_PULL_END : JDV_PULL_ERROR;
}

static int import_cancel(void* context, jdv_slice_t reason) {
    (void)context; (void)reason;
    return 0;
}

static void import_release(void* context) {
    ((jdv_wire_import_t*)context)->active = 0;
}

static const jdv_stream_producer_vtable_t VTABLE = {
    JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
    import_pull, import_cancel, import_release
};

int jdv_wire_import_v2_open(jdv_wire_import_t* source, const void* bytes,
                            size_t size, void* schema_storage,
                            size_t schema_capacity, void* item_storage,
                            size_t item_capacity,
                            const jdv_wire_limits_t* limits) {
    if (!source || source->active || !bytes || !schema_storage ||
        !item_storage || !schema_capacity || !item_capacity || !limits ||
        overlaps(source, sizeof(*source), bytes, size) ||
        overlaps(source, sizeof(*source), schema_storage, schema_capacity) ||
        overlaps(source, sizeof(*source), item_storage, item_capacity) ||
        overlaps(bytes, size, schema_storage, schema_capacity) ||
        overlaps(bytes, size, item_storage, item_capacity) ||
        overlaps(schema_storage, schema_capacity, item_storage, item_capacity))
        return fail(EINVAL);
    jdv_arena_init(&source->schemas, schema_storage, schema_capacity);
    jdv_arena_init(&source->items, item_storage, item_capacity);
    jdv_wire_info_t verified;
    if (jdv_wire_validate_v2(bytes, size, &source->items,
                              limits, &verified) < 0) return -1;
    source->limits = *limits;
    source->bytes = bytes;
    source->size = size;
    size_t position = 4, items_start = 0;
    uint8_t tag;
    const uint8_t* payload;
    size_t length;
    if (next_frame(source, &position, &tag, &payload, &length) < 0 ||
        tag != 'D' ||
        jdv_schema_decode_exact_limited(&source->schemas, payload, length,
            &source->declared, NULL, &limits->value_limits) < 0)
        goto invalid;
    if (next_frame(source, &position, &tag, &payload, &length) < 0 ||
        tag != 'G' ||
        jdv_schema_decode_exact_limited(&source->schemas, payload, length,
            &source->guaranteed, NULL, &limits->value_limits) < 0)
        goto invalid;
    items_start = position;
    int final_present = 0;
    for (;;) {
        if (next_frame(source, &position, &tag, &payload, &length) < 0)
            goto invalid;
        if (tag == 'I') continue;
        if (tag == 'F') {
            if (jdv_schema_decode_exact_limited(&source->schemas,
                    payload, length, &source->final_schema, NULL,
                    &limits->value_limits) < 0) goto invalid;
            final_present = 1;
            if (next_frame(source, &position, &tag, &payload, &length) < 0)
                goto invalid;
        }
        if (tag != 'T' || position != size || length < 96) goto malformed;
        if (jdv_stream_end_init(&source->terminal) < 0) goto invalid;
        source->terminal.state = verified.state;
        source->terminal.completeness = verified.completeness;
        source->terminal.items_seen = verified.items_seen;
        source->terminal.bytes_seen = verified.bytes_seen;
        source->terminal.errors_seen = verified.errors_seen;
        source->terminal.truncated = verified.truncated;
        source->terminal.final_hash_present = verified.final_hash_present;
        source->terminal.final_hash_algorithm =
            verified.final_hash_present ? JDV_HASH_BLAKE3 : JDV_HASH_NONE;
        memcpy(source->terminal.final_hash, verified.final_hash, JDV_HASH_SIZE);
        if (read_status(payload + 32, &source->terminal.producer_status) < 0 ||
            read_status(payload + 40, &source->terminal.transform_status) < 0 ||
            read_status(payload + 48, &source->terminal.transport_status) < 0)
            goto invalid;
        source->terminal.reason.len = (size_t)read_u64(payload + 88);
        source->terminal.reason.data = source->terminal.reason.len
            ? payload + 96 : NULL;
        source->terminal.final_schema = final_present
            ? &source->final_schema : NULL;
        if (jdv_stream_end_validate(&source->terminal,
                                     &limits->value_limits) < 0)
            goto invalid;
        break;
    }
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = &source->declared;
    config.guaranteed_schema = &source->guaranteed;
    config.value_limits = limits->value_limits;
    config.stream_limits.max_items = limits->max_items;
    config.stream_limits.max_logical_bytes = verified.bytes_seen;
    config.stream_limits.max_physical_bytes = limits->max_wire_bytes;
    if (jdv_stream_descriptor_init(&source->descriptor, &config, &VTABLE,
                                   source) < 0 ||
        jdv_stream_open(&source->descriptor, &source->cursor) < 0)
        goto invalid;
    source->position = items_start;
    source->active = 1;
    return 0;
malformed:
    errno = EBADMSG;
invalid:
    {
        int saved = errno ? errno : EBADMSG;
        source->schemas.used = 0;
        source->items.used = 0;
        errno = saved;
        return -1;
    }
}

int jdv_wire_import_close(jdv_wire_import_t* source) {
    if (!source) return fail(EINVAL);
    if (!source->active) return 0;
    return jdv_stream_release(&source->cursor);
}
