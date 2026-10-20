// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_filter.h"

#include <errno.h>
#include <string.h>

static int matches(const jdv_value_t* actual, const jdv_value_t* expected) {
    if (actual->kind != expected->kind) return 0;
    switch (actual->kind) {
        case JDV_NOTHING: return 1;
        case JDV_BOOL: return actual->as.boolean == expected->as.boolean;
        case JDV_I64: case JDV_TIMESTAMP: case JDV_DURATION:
            return actual->as.i64 == expected->as.i64;
        case JDV_U64: case JDV_FILESIZE:
            return actual->as.u64 == expected->as.u64;
        case JDV_STRING: case JDV_BYTES: case JDV_PATH: case JDV_SYMBOL:
            return actual->as.slice.len == expected->as.slice.len &&
                (!actual->as.slice.len || !memcmp(actual->as.slice.data,
                    expected->as.slice.data, actual->as.slice.len));
        default: return 0;
    }
}

static int record_matches(const jdv_filter_t* filter, const jdv_value_t* item) {
    if (item->kind != JDV_RECORD) return 0;
    for (size_t i = 0; i < item->as.record.len; i++) {
        const jdv_record_field_t* field = &item->as.record.fields[i];
        if (field->key.len == filter->field_len &&
            !memcmp(field->key.data, filter->field, filter->field_len))
            return matches(&field->value, &filter->expected);
    }
    return 0;
}

static jdv_stream_pull_result_t filtered_pull(void* context,
                                              jdv_stream_record_t* record) {
    jdv_filter_t* filter = context;
    for (;;) {
        jdv_stream_record_t upstream;
        jdv_stream_pull_result_t result = jdv_stream_pull(filter->input, &upstream);
        if (result == JDV_PULL_ITEM) {
            if (!record_matches(filter, upstream.item)) continue;
            if (filter->items == UINT64_MAX ||
                upstream.logical_bytes > UINT64_MAX - filter->logical_bytes) {
                (void)jdv_stream_end_init(&filter->terminal);
                filter->terminal.state = JDV_STREAM_FAILED;
                filter->terminal.completeness = JDV_COMPLETENESS_PARTIAL;
                filter->terminal.transform_status = EOVERFLOW;
                filter->terminal.errors_seen = 1;
                filter->terminal.items_seen = filter->items;
                filter->terminal.bytes_seen = filter->logical_bytes;
                record->terminal = &filter->terminal;
                return JDV_PULL_ERROR;
            }
            uint8_t content_hash[JDV_HASH_SIZE];
            if (jdv_hash(upstream.item, content_hash) < 0) {
                (void)jdv_stream_end_init(&filter->terminal);
                filter->terminal.state = JDV_STREAM_FAILED;
                filter->terminal.completeness = JDV_COMPLETENESS_PARTIAL;
                filter->terminal.transform_status = errno;
                filter->terminal.errors_seen = 1;
                filter->terminal.items_seen = filter->items;
                filter->terminal.bytes_seen = filter->logical_bytes;
                record->terminal = &filter->terminal;
                return JDV_PULL_ERROR;
            }
            blake3_update(&filter->digest, content_hash, sizeof(content_hash));
            filter->items++;
            filter->logical_bytes += upstream.logical_bytes;
            record->item = upstream.item;
            record->logical_bytes = upstream.logical_bytes;
            return JDV_PULL_ITEM;
        }
        if (result == JDV_PULL_PENDING) return JDV_PULL_PENDING;
        if (!upstream.terminal) {
            int saved = errno ? errno : EPROTO;
            (void)jdv_stream_end_init(&filter->terminal);
            filter->terminal.state = JDV_STREAM_FAILED;
            filter->terminal.completeness = JDV_COMPLETENESS_PARTIAL;
            filter->terminal.transform_status = saved;
            filter->terminal.errors_seen = 1;
        } else {
            filter->terminal = *upstream.terminal;
        }
        filter->terminal.items_seen = filter->items;
        filter->terminal.bytes_seen = filter->logical_bytes;
        if (filter->terminal.state == JDV_STREAM_COMPLETE) {
            filter->terminal.final_hash_present = 1;
            filter->terminal.final_hash_algorithm = JDV_HASH_BLAKE3;
            blake3_final(&filter->digest, filter->terminal.final_hash,
                          JDV_HASH_SIZE);
        } else {
            filter->terminal.final_hash_present = 0;
            filter->terminal.final_hash_algorithm = JDV_HASH_NONE;
            memset(filter->terminal.final_hash, 0, JDV_HASH_SIZE);
        }
        record->terminal = &filter->terminal;
        return filter->terminal.state == JDV_STREAM_COMPLETE
            ? JDV_PULL_END : JDV_PULL_ERROR;
    }
}

static int filtered_cancel(void* context, jdv_slice_t reason) {
    jdv_filter_t* filter = context;
    return jdv_stream_cancel(filter->input, reason, NULL);
}

static void filtered_release(void* context) {
    jdv_filter_t* filter = context;
    (void)jdv_stream_release(filter->input);
    filter->active = 0;
}

static const jdv_stream_producer_vtable_t VTABLE = {
    JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
    filtered_pull, filtered_cancel, filtered_release
};

int jdv_filter_equal_open(jdv_filter_t* filter, jdv_stream_cursor_t* input,
                          jdv_slice_t field, const jdv_value_t* expected) {
    if (!filter || filter->active || !input || !expected || !field.data ||
        !field.len || field.len > sizeof(filter->field)) {
        errno = EINVAL;
        return -1;
    }
    if (expected->kind == JDV_LIST || expected->kind == JDV_RECORD ||
        expected->kind < JDV_NOTHING || expected->kind > JDV_SYMBOL) {
        errno = EINVAL;
        return -1;
    }
    uint8_t validation_hash[JDV_HASH_SIZE];
    if (jdv_hash(expected, validation_hash) < 0) return -1;
    const jdv_schema_set_t* schemas;
    if (jdv_stream_schemas(input, &schemas) < 0) return -1;
    if (schemas->declared_schema->kind != JDV_SCHEMA_RECORD) {
        errno = EDOM;
        return -1;
    }
    const jdv_schema_field_t* declared_field = NULL;
    for (size_t i = 0; i < schemas->declared_schema->as.record.len; i++) {
        const jdv_schema_field_t* candidate =
            &schemas->declared_schema->as.record.fields[i];
        if (candidate->name.len == field.len &&
            !memcmp(candidate->name.data, field.data, field.len)) {
            declared_field = candidate;
            break;
        }
    }
    jdv_relation_t accepted;
    if (!declared_field ||
        jdv_schema_accepts_checked(declared_field->schema, expected,
                                   &accepted, &JDV_DEFAULT_LIMITS) < 0 ||
        accepted != JDV_RELATION_YES) {
        errno = EDOM;
        return -1;
    }
    filter->items = 0;
    filter->logical_bytes = 0;
    filter->input = input;
    filter->field_len = field.len;
    memcpy(filter->field, field.data, field.len);
    filter->expected = *expected;
    if (expected->kind >= JDV_STRING && expected->kind <= JDV_SYMBOL) {
        if (expected->as.slice.len > sizeof(filter->expected_bytes)) {
            errno = E2BIG;
            return -1;
        }
        memcpy(filter->expected_bytes, expected->as.slice.data,
                expected->as.slice.len);
        filter->expected.as.slice.data = filter->expected_bytes;
    }
    blake3_init(&filter->digest);
    blake3_update(&filter->digest, "JTV1-ITEMS-v1",
                  sizeof("JTV1-ITEMS-v1") - 1u);
    jdv_stream_config_t config;
    jdv_stream_config_init(&config);
    config.boundedness = JDV_STREAM_FINITE;
    config.declared_schema = schemas->declared_schema;
    config.guaranteed_schema = schemas->guaranteed_schema;
    if (jdv_stream_descriptor_init(&filter->descriptor, &config, &VTABLE,
                                   filter) < 0 ||
        jdv_stream_open(&filter->descriptor, &filter->cursor) < 0)
        return -1;
    filter->active = 1;
    return 0;
}

int jdv_filter_close(jdv_filter_t* filter) {
    if (!filter) { errno = EINVAL; return -1; }
    if (!filter->active) return 0;
    return jdv_stream_release(&filter->cursor);
}
