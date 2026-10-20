// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_value.h"

#include <errno.h>
#include <limits.h>
#include <module/blake3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const jdv_limits_t JDV_DEFAULT_LIMITS = {
    JDV_MAX_DEPTH,
    16u * 1024u * 1024u,
    1024u * 1024u,
    1024u * 1024u,
    65536u,
    65536u,
    UINT64_C(64) * 1024u * 1024u,
};

typedef struct {
    const jdv_limits_t* limits;
    size_t aggregate_bytes;
    size_t nodes;
    size_t list_items;
    size_t record_fields;
    size_t schema_fields;
} jdv_budget_t;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t offset;
    int error;
    jdv_budget_t budget;
} jdv_reader_t;

static int fail_errno(int error) {
    errno = error;
    return -1;
}

static int limits_valid(const jdv_limits_t* limits) {
    if (!limits) return fail_errno(EINVAL);
    return 0;
}

static int budget_node(jdv_budget_t* budget, size_t depth) {
    if (depth >= budget->limits->max_depth ||
        budget->nodes >= budget->limits->max_nodes)
        return fail_errno(E2BIG);
    budget->nodes++;
    return 0;
}

static int budget_bytes(jdv_budget_t* budget, size_t size) {
    if (budget->aggregate_bytes > budget->limits->max_aggregate_bytes ||
        size > budget->limits->max_aggregate_bytes - budget->aggregate_bytes)
        return fail_errno(E2BIG);
    budget->aggregate_bytes += size;
    return 0;
}

static int budget_count(size_t* used, size_t count, size_t limit) {
    if (*used > limit || count > limit - *used) return fail_errno(E2BIG);
    *used += count;
    return 0;
}

void jdv_arena_init(jdv_arena_t* arena, void* storage, size_t capacity) {
    if (!arena) return;
    arena->data = storage;
    arena->capacity = capacity;
    arena->used = 0;
}

static int arena_valid(const jdv_arena_t* arena) {
    if (!arena || (!arena->data && arena->capacity)) return fail_errno(EINVAL);
    if (arena->used > arena->capacity) return fail_errno(EOVERFLOW);
    return 0;
}

static void* arena_alloc(jdv_arena_t* arena, size_t size, size_t alignment) {
    if (!alignment) {
        errno = EINVAL;
        return NULL;
    }
    if (arena_valid(arena) < 0) return NULL;
    if (!size) return arena ? arena->data : NULL;
    uintptr_t base = (uintptr_t)arena->data;
    if (arena->used > arena->capacity || base > UINTPTR_MAX - arena->used) {
        errno = EOVERFLOW;
        return NULL;
    }
    uintptr_t current = base + arena->used;
    size_t padding = (size_t)((alignment - current % alignment) % alignment);
    if (padding > arena->capacity - arena->used ||
        size > arena->capacity - arena->used - padding) {
        errno = ENOSPC;
        return NULL;
    }
    arena->used += padding;
    void* result = arena->data + arena->used;
    arena->used += size;
    return result;
}

static void* arena_array(jdv_arena_t* arena, size_t count, size_t item_size,
                         size_t alignment) {
    if (count && item_size > SIZE_MAX / count) {
        errno = EOVERFLOW;
        return NULL;
    }
    return arena_alloc(arena, count * item_size, alignment);
}

static int utf8_valid(const uint8_t* data, size_t size) {
    size_t i = 0;
    if (size && !data) return 0;
    while (i < size) {
        uint8_t c = data[i++];
        if (c < 0x80) continue;
        unsigned continuation;
        uint32_t value;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation = 1;
            value = c & 0x1fu;
            minimum = 0x80u;
        } else if (c >= 0xe0 && c <= 0xef) {
            continuation = 2;
            value = c & 0x0fu;
            minimum = 0x800u;
        } else if (c >= 0xf0 && c <= 0xf4) {
            continuation = 3;
            value = c & 0x07u;
            minimum = 0x10000u;
        } else {
            return 0;
        }
        if (continuation > size - i) return 0;
        for (unsigned n = 0; n < continuation; n++) {
            c = data[i++];
            if ((c & 0xc0u) != 0x80u) return 0;
            value = (value << 6) | (c & 0x3fu);
        }
        if (value < minimum || value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu)) return 0;
    }
    return 1;
}

static int contains_nul(const uint8_t* data, size_t size) {
    return size && data && memchr(data, 0, size) != NULL;
}

static int slice_compare(jdv_slice_t a, jdv_slice_t b) {
    size_t common = a.len < b.len ? a.len : b.len;
    int order = common ? memcmp(a.data, b.data, common) : 0;
    if (order) return order;
    return (a.len > b.len) - (a.len < b.len);
}

static int copy_slice(jdv_arena_t* arena, const void* data, size_t size,
                      jdv_slice_t* out) {
    if (!out || (size && !data)) return fail_errno(EINVAL);
    jdv_slice_t result = {NULL, 0};
    if (size) {
        uint8_t* copy = arena_alloc(arena, size, 1);
        if (!copy) return -1;
        memcpy(copy, data, size);
        result.data = copy;
        result.len = size;
    }
    *out = result;
    return 0;
}

static jdv_value_t scalar_value(jdv_kind_t kind) {
    jdv_value_t result;
    memset(&result, 0, sizeof(result));
    result.kind = kind;
    return result;
}

jdv_value_t jdv_value_nothing(void) { return scalar_value(JDV_NOTHING); }

jdv_value_t jdv_value_bool(int value) {
    jdv_value_t result = scalar_value(JDV_BOOL);
    result.as.boolean = value != 0;
    return result;
}

jdv_value_t jdv_value_i64(int64_t value) {
    jdv_value_t result = scalar_value(JDV_I64);
    result.as.i64 = value;
    return result;
}

jdv_value_t jdv_value_u64(uint64_t value) {
    jdv_value_t result = scalar_value(JDV_U64);
    result.as.u64 = value;
    return result;
}

jdv_value_t jdv_value_filesize(uint64_t bytes) {
    jdv_value_t result = scalar_value(JDV_FILESIZE);
    result.as.u64 = bytes;
    return result;
}

jdv_value_t jdv_value_timestamp(int64_t nanoseconds) {
    jdv_value_t result = scalar_value(JDV_TIMESTAMP);
    result.as.i64 = nanoseconds;
    return result;
}

jdv_value_t jdv_value_duration(int64_t nanoseconds) {
    jdv_value_t result = scalar_value(JDV_DURATION);
    result.as.i64 = nanoseconds;
    return result;
}

static int scalar_slice_valid(jdv_kind_t kind, jdv_slice_t slice) {
    if (slice.len && !slice.data) return fail_errno(EINVAL);
    if ((kind == JDV_STRING || kind == JDV_SYMBOL) &&
        !utf8_valid(slice.data, slice.len)) return fail_errno(EILSEQ);
    if ((kind == JDV_PATH || kind == JDV_SYMBOL) &&
        contains_nul(slice.data, slice.len)) return fail_errno(EINVAL);
    return 0;
}

static int value_validate_at(const jdv_value_t* value, size_t depth,
                             jdv_budget_t* budget) {
    if (!value) return fail_errno(EINVAL);
    if (budget_node(budget, depth) < 0) return -1;
    switch (value->kind) {
        case JDV_NOTHING:
        case JDV_I64:
        case JDV_U64:
        case JDV_FILESIZE:
        case JDV_TIMESTAMP:
        case JDV_DURATION:
            return 0;
        case JDV_BOOL:
            return value->as.boolean == 0 || value->as.boolean == 1
                ? 0 : fail_errno(EINVAL);
        case JDV_STRING:
        case JDV_BYTES:
        case JDV_PATH:
        case JDV_SYMBOL:
            if (scalar_slice_valid(value->kind, value->as.slice) < 0) return -1;
            return budget_bytes(budget, value->as.slice.len);
        case JDV_LIST:
            if (budget_count(&budget->list_items, value->as.list.len,
                             budget->limits->max_list_items) < 0) return -1;
            if (value->as.list.len && !value->as.list.items)
                return fail_errno(EINVAL);
            for (size_t i = 0; i < value->as.list.len; i++)
                if (value_validate_at(&value->as.list.items[i], depth + 1,
                                      budget) < 0) return -1;
            return 0;
        case JDV_RECORD:
            if (budget_count(&budget->record_fields, value->as.record.len,
                             budget->limits->max_record_fields) < 0) return -1;
            if (value->as.record.len && !value->as.record.fields)
                return fail_errno(EINVAL);
            for (size_t i = 0; i < value->as.record.len; i++) {
                const jdv_record_field_t* field = &value->as.record.fields[i];
                if (!utf8_valid(field->key.data, field->key.len) ||
                    contains_nul(field->key.data, field->key.len))
                    return fail_errno(EINVAL);
                if (i && slice_compare(value->as.record.fields[i - 1].key,
                                       field->key) >= 0)
                    return fail_errno(EINVAL);
                if (budget_bytes(budget, field->key.len) < 0 ||
                    value_validate_at(&field->value, depth + 1, budget) < 0)
                    return -1;
            }
            return 0;
    }
    return fail_errno(EINVAL);
}

static int value_validate(const jdv_value_t* value, const jdv_limits_t* limits) {
    if (limits_valid(limits) < 0) return -1;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    return value_validate_at(value, 0, &budget);
}

static int compare_value_fields(const void* left, const void* right) {
    const jdv_record_field_t* a = left;
    const jdv_record_field_t* b = right;
    return slice_compare(a->key, b->key);
}

static void sort_value_fields(jdv_record_field_t* fields, size_t count) {
    qsort(fields, count, sizeof(*fields), compare_value_fields);
}

static int clone_value_at(jdv_arena_t* arena, const jdv_value_t* source,
                          jdv_value_t* out, size_t depth,
                          jdv_budget_t* budget) {
    if (!source) return fail_errno(EINVAL);
    if (budget_node(budget, depth) < 0) return -1;
    jdv_value_t result = *source;
    switch (source->kind) {
        case JDV_NOTHING:
        case JDV_I64:
        case JDV_U64:
        case JDV_FILESIZE:
        case JDV_TIMESTAMP:
        case JDV_DURATION:
            break;
        case JDV_BOOL:
            if (source->as.boolean != 0 && source->as.boolean != 1)
                return fail_errno(EINVAL);
            break;
        case JDV_STRING:
        case JDV_BYTES:
        case JDV_PATH:
        case JDV_SYMBOL:
            if (scalar_slice_valid(source->kind, source->as.slice) < 0 ||
                budget_bytes(budget, source->as.slice.len) < 0 ||
                copy_slice(arena, source->as.slice.data, source->as.slice.len,
                           &result.as.slice) < 0) return -1;
            break;
        case JDV_LIST: {
            size_t count = source->as.list.len;
            if (budget_count(&budget->list_items, count,
                             budget->limits->max_list_items) < 0) return -1;
            if (count && !source->as.list.items) return fail_errno(EINVAL);
            result.as.list.items = NULL;
            if (count) {
                jdv_value_t* items = arena_array(arena, count, sizeof(*items),
                                                 _Alignof(jdv_value_t));
                if (!items) return -1;
                result.as.list.items = items;
                for (size_t i = 0; i < count; i++)
                    if (clone_value_at(arena, &source->as.list.items[i],
                                       &items[i], depth + 1, budget) < 0)
                        return -1;
            }
            break;
        }
        case JDV_RECORD: {
            size_t count = source->as.record.len;
            if (budget_count(&budget->record_fields, count,
                             budget->limits->max_record_fields) < 0) return -1;
            if (count && !source->as.record.fields) return fail_errno(EINVAL);
            result.as.record.fields = NULL;
            if (count) {
                jdv_record_field_t* fields = arena_array(
                    arena, count, sizeof(*fields), _Alignof(jdv_record_field_t));
                if (!fields) return -1;
                result.as.record.fields = fields;
                for (size_t i = 0; i < count; i++) {
                    const jdv_record_field_t* input = &source->as.record.fields[i];
                    if (!utf8_valid(input->key.data, input->key.len) ||
                        contains_nul(input->key.data, input->key.len))
                        return fail_errno(EINVAL);
                    if (budget_bytes(budget, input->key.len) < 0 ||
                        copy_slice(arena, input->key.data, input->key.len,
                                   &fields[i].key) < 0 ||
                        clone_value_at(arena, &input->value, &fields[i].value,
                                       depth + 1, budget) < 0) return -1;
                }
                sort_value_fields(fields, count);
                for (size_t i = 1; i < count; i++)
                    if (slice_compare(fields[i - 1].key, fields[i].key) == 0)
                        return fail_errno(EEXIST);
            }
            break;
        }
        default:
            return fail_errno(EINVAL);
    }
    *out = result;
    return 0;
}

static int make_slice_value(jdv_arena_t* arena, const void* data, size_t size,
                            jdv_kind_t kind, jdv_value_t* out,
                            const jdv_limits_t* limits) {
    if (!out || (size && !data)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    jdv_value_t source = scalar_value(kind);
    source.as.slice.data = data;
    source.as.slice.len = size;
    jdv_value_t result;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    size_t mark = arena->used;
    if (clone_value_at(arena, &source, &result, 0, &budget) < 0) {
        arena->used = mark;
        return -1;
    }
    *out = result;
    return 0;
}

#define DEFINE_SLICE_CONSTRUCTOR(name, kind)                                      \
    int name##_limited(jdv_arena_t* arena, const void* data, size_t size,         \
                       jdv_value_t* out, const jdv_limits_t* limits) {             \
        return make_slice_value(arena, data, size, kind, out, limits);             \
    }                                                                              \
    int name(jdv_arena_t* arena, const void* data, size_t size, jdv_value_t* out) {\
        return name##_limited(arena, data, size, out, &JDV_DEFAULT_LIMITS);         \
    }

DEFINE_SLICE_CONSTRUCTOR(jdv_value_string, JDV_STRING)
DEFINE_SLICE_CONSTRUCTOR(jdv_value_bytes, JDV_BYTES)
DEFINE_SLICE_CONSTRUCTOR(jdv_value_path, JDV_PATH)
DEFINE_SLICE_CONSTRUCTOR(jdv_value_symbol, JDV_SYMBOL)

int jdv_value_list_limited(jdv_arena_t* arena, const jdv_value_t* items,
                           size_t count, jdv_value_t* out,
                           const jdv_limits_t* limits) {
    if (!out || (count && !items)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    jdv_value_t source = scalar_value(JDV_LIST);
    source.as.list.items = items;
    source.as.list.len = count;
    jdv_value_t result;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    size_t mark = arena->used;
    if (clone_value_at(arena, &source, &result, 0, &budget) < 0) {
        arena->used = mark;
        return -1;
    }
    *out = result;
    return 0;
}

int jdv_value_list(jdv_arena_t* arena, const jdv_value_t* items, size_t count,
                   jdv_value_t* out) {
    return jdv_value_list_limited(arena, items, count, out, &JDV_DEFAULT_LIMITS);
}

int jdv_value_record_limited(jdv_arena_t* arena,
                             const jdv_record_input_t* fields, size_t count,
                             jdv_value_t* out, const jdv_limits_t* limits) {
    if (!out || (count && !fields)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    jdv_value_t source = scalar_value(JDV_RECORD);
    source.as.record.fields = fields;
    source.as.record.len = count;
    jdv_value_t result;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    size_t mark = arena->used;
    if (clone_value_at(arena, &source, &result, 0, &budget) < 0) {
        arena->used = mark;
        return -1;
    }
    *out = result;
    return 0;
}

int jdv_value_record(jdv_arena_t* arena, const jdv_record_input_t* fields,
                     size_t count, jdv_value_t* out) {
    return jdv_value_record_limited(arena, fields, count, out,
                                    &JDV_DEFAULT_LIMITS);
}

static jdv_data_t data_base(jdv_cardinality_t cardinality,
                            jdv_ownership_t ownership) {
    jdv_data_t result;
    memset(&result, 0, sizeof(result));
    result.metadata_version = JDV_METADATA_VERSION;
    result.struct_size = (uint32_t)sizeof(result);
    result.cardinality = cardinality;
    result.ownership = ownership;
    return result;
}

jdv_data_t jdv_data_empty(void) {
    return data_base(JDV_DATA_EMPTY, JDV_OWNERSHIP_IMMUTABLE_VALUE);
}

jdv_data_t jdv_data_scalar(jdv_value_t value) {
    jdv_data_t result = data_base(JDV_DATA_SCALAR, JDV_OWNERSHIP_IMMUTABLE_VALUE);
    result.as.scalar = value;
    result.provenance_id = value.provenance_id;
    return result;
}

jdv_data_t jdv_data_sequence(const jdv_sequence_t* sequence) {
    jdv_data_t result = data_base(JDV_DATA_SEQUENCE, JDV_OWNERSHIP_LINEAR_STREAM);
    result.as.sequence = sequence;
    return result;
}

jdv_data_t jdv_data_byte_stream(const jdv_byte_stream_t* stream) {
    jdv_data_t result = data_base(JDV_DATA_BYTE_STREAM, JDV_OWNERSHIP_LINEAR_STREAM);
    result.as.byte_stream = stream;
    return result;
}

int jdv_data_validate(const jdv_data_t* data, const jdv_limits_t* limits) {
    if (!data || limits_valid(limits) < 0) return fail_errno(EINVAL);
    if (data->metadata_version != JDV_METADATA_VERSION ||
        data->struct_size != sizeof(*data)) return fail_errno(EPROTONOSUPPORT);
    if (data->cardinality < JDV_DATA_EMPTY ||
        data->cardinality > JDV_DATA_BYTE_STREAM ||
        data->ownership < JDV_OWNERSHIP_IMMUTABLE_VALUE ||
        data->ownership > JDV_OWNERSHIP_DURABLE_REFERENCE)
        return fail_errno(EINVAL);
    if (data->cardinality == JDV_DATA_EMPTY)
        return data->ownership == JDV_OWNERSHIP_IMMUTABLE_VALUE
            ? 0 : fail_errno(EINVAL);
    if (data->cardinality == JDV_DATA_SCALAR) {
        if (data->ownership != JDV_OWNERSHIP_IMMUTABLE_VALUE)
            return fail_errno(EINVAL);
        return value_validate(&data->as.scalar, limits);
    }
    if (data->cardinality == JDV_DATA_SEQUENCE && !data->as.sequence)
        return fail_errno(EINVAL);
    if (data->cardinality == JDV_DATA_BYTE_STREAM && !data->as.byte_stream)
        return fail_errno(EINVAL);
    return 0;
}

static int scalar_schema_kind(jdv_kind_t kind) {
    return kind >= JDV_NOTHING && kind <= JDV_SYMBOL;
}

static int special_schema_kind(jdv_schema_kind_t kind) {
    return kind == JDV_SCHEMA_UNKNOWN || kind == JDV_SCHEMA_ANY ||
           kind == JDV_SCHEMA_INVALID;
}

static int schema_validate_at(const jdv_schema_t* schema, size_t depth,
                              jdv_budget_t* budget) {
    if (!schema) return fail_errno(EINVAL);
    if (budget_node(budget, depth) < 0) return -1;
    if (schema->nullable != 0 && schema->nullable != 1)
        return fail_errno(EINVAL);
    if (special_schema_kind(schema->kind))
        return schema->nullable == 0 ? 0 : fail_errno(EINVAL);
    switch (schema->kind) {
        case JDV_SCHEMA_SCALAR:
            return scalar_schema_kind(schema->as.scalar)
                ? 0 : fail_errno(EINVAL);
        case JDV_SCHEMA_LIST:
            return schema_validate_at(schema->as.element, depth + 1, budget);
        case JDV_SCHEMA_RECORD:
            if ((schema->as.record.open != 0 && schema->as.record.open != 1) ||
                (schema->as.record.len && !schema->as.record.fields))
                return fail_errno(EINVAL);
            if (budget_count(&budget->schema_fields, schema->as.record.len,
                             budget->limits->max_schema_fields) < 0) return -1;
            for (size_t i = 0; i < schema->as.record.len; i++) {
                const jdv_schema_field_t* field = &schema->as.record.fields[i];
                if ((field->optional != 0 && field->optional != 1) ||
                    !utf8_valid(field->name.data, field->name.len) ||
                    contains_nul(field->name.data, field->name.len))
                    return fail_errno(EINVAL);
                if (i && slice_compare(schema->as.record.fields[i - 1].name,
                                       field->name) >= 0)
                    return fail_errno(EINVAL);
                if (budget_bytes(budget, field->name.len) < 0 ||
                    schema_validate_at(field->schema, depth + 1, budget) < 0)
                    return -1;
            }
            return 0;
        default:
            return fail_errno(EINVAL);
    }
}

static int schema_validate(const jdv_schema_t* schema,
                           const jdv_limits_t* limits) {
    if (limits_valid(limits) < 0) return -1;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    return schema_validate_at(schema, 0, &budget);
}

static int compare_schema_fields(const void* left, const void* right) {
    const jdv_schema_field_t* a = left;
    const jdv_schema_field_t* b = right;
    return slice_compare(a->name, b->name);
}

static void sort_schema_fields(jdv_schema_field_t* fields, size_t count) {
    qsort(fields, count, sizeof(*fields), compare_schema_fields);
}

static int clone_schema_at(jdv_arena_t* arena, const jdv_schema_t* source,
                           jdv_schema_t* out, size_t depth,
                           jdv_budget_t* budget) {
    if (!source) return fail_errno(EINVAL);
    if (budget_node(budget, depth) < 0) return -1;
    if (source->nullable != 0 && source->nullable != 1)
        return fail_errno(EINVAL);
    jdv_schema_t result = *source;
    if (special_schema_kind(source->kind)) {
        if (source->nullable) return fail_errno(EINVAL);
    } else if (source->kind == JDV_SCHEMA_SCALAR) {
        if (!scalar_schema_kind(source->as.scalar)) return fail_errno(EINVAL);
    } else if (source->kind == JDV_SCHEMA_LIST) {
        jdv_schema_t* element = arena_alloc(arena, sizeof(*element),
                                            _Alignof(jdv_schema_t));
        if (!element) return -1;
        result.as.element = element;
        if (clone_schema_at(arena, source->as.element, element, depth + 1,
                            budget) < 0) return -1;
    } else if (source->kind == JDV_SCHEMA_RECORD) {
        size_t count = source->as.record.len;
        if ((source->as.record.open != 0 && source->as.record.open != 1) ||
            (count && !source->as.record.fields)) return fail_errno(EINVAL);
        if (budget_count(&budget->schema_fields, count,
                         budget->limits->max_schema_fields) < 0) return -1;
        result.as.record.fields = NULL;
        if (count) {
            jdv_schema_field_t* fields = arena_array(
                arena, count, sizeof(*fields), _Alignof(jdv_schema_field_t));
            if (!fields) return -1;
            result.as.record.fields = fields;
            for (size_t i = 0; i < count; i++) {
                const jdv_schema_field_t* input = &source->as.record.fields[i];
                if ((input->optional != 0 && input->optional != 1) ||
                    !utf8_valid(input->name.data, input->name.len) ||
                    contains_nul(input->name.data, input->name.len))
                    return fail_errno(EINVAL);
                fields[i].optional = input->optional;
                if (budget_bytes(budget, input->name.len) < 0 ||
                    copy_slice(arena, input->name.data, input->name.len,
                               &fields[i].name) < 0) return -1;
                jdv_schema_t* child = arena_alloc(arena, sizeof(*child),
                                                  _Alignof(jdv_schema_t));
                if (!child) return -1;
                fields[i].schema = child;
                if (clone_schema_at(arena, input->schema, child, depth + 1,
                                    budget) < 0) return -1;
            }
            sort_schema_fields(fields, count);
            for (size_t i = 1; i < count; i++)
                if (slice_compare(fields[i - 1].name, fields[i].name) == 0)
                    return fail_errno(EEXIST);
        }
    } else {
        return fail_errno(EINVAL);
    }
    *out = result;
    return 0;
}

static int make_special_schema(jdv_schema_kind_t kind, jdv_schema_t* out,
                               const jdv_limits_t* limits) {
    if (!out || limits_valid(limits) < 0) return fail_errno(EINVAL);
    jdv_schema_t result;
    memset(&result, 0, sizeof(result));
    result.kind = kind;
    if (schema_validate(&result, limits) < 0) return -1;
    *out = result;
    return 0;
}

int jdv_schema_unknown_limited(jdv_schema_t* out, const jdv_limits_t* limits) {
    return make_special_schema(JDV_SCHEMA_UNKNOWN, out, limits);
}

int jdv_schema_any_limited(jdv_schema_t* out, const jdv_limits_t* limits) {
    return make_special_schema(JDV_SCHEMA_ANY, out, limits);
}

int jdv_schema_invalid_limited(jdv_schema_t* out, const jdv_limits_t* limits) {
    return make_special_schema(JDV_SCHEMA_INVALID, out, limits);
}

int jdv_schema_unknown(jdv_schema_t* out) {
    return jdv_schema_unknown_limited(out, &JDV_DEFAULT_LIMITS);
}

int jdv_schema_any(jdv_schema_t* out) {
    return jdv_schema_any_limited(out, &JDV_DEFAULT_LIMITS);
}

int jdv_schema_invalid(jdv_schema_t* out) {
    return jdv_schema_invalid_limited(out, &JDV_DEFAULT_LIMITS);
}

int jdv_schema_scalar_limited(jdv_kind_t kind, int nullable, jdv_schema_t* out,
                              const jdv_limits_t* limits) {
    if (!out || !scalar_schema_kind(kind) ||
        (nullable != 0 && nullable != 1) || limits_valid(limits) < 0)
        return fail_errno(EINVAL);
    jdv_schema_t result;
    memset(&result, 0, sizeof(result));
    result.kind = JDV_SCHEMA_SCALAR;
    result.nullable = nullable;
    result.as.scalar = kind;
    if (schema_validate(&result, limits) < 0) return -1;
    *out = result;
    return 0;
}

int jdv_schema_scalar(jdv_kind_t kind, int nullable, jdv_schema_t* out) {
    return jdv_schema_scalar_limited(kind, nullable, out, &JDV_DEFAULT_LIMITS);
}

int jdv_schema_list_limited(jdv_arena_t* arena, const jdv_schema_t* element,
                            int nullable, jdv_schema_t* out,
                            const jdv_limits_t* limits) {
    if (!out || (nullable != 0 && nullable != 1)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    jdv_schema_t source;
    memset(&source, 0, sizeof(source));
    source.kind = JDV_SCHEMA_LIST;
    source.nullable = nullable;
    source.as.element = element;
    jdv_schema_t result;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    size_t mark = arena->used;
    if (clone_schema_at(arena, &source, &result, 0, &budget) < 0) {
        arena->used = mark;
        return -1;
    }
    *out = result;
    return 0;
}

int jdv_schema_list(jdv_arena_t* arena, const jdv_schema_t* element,
                    int nullable, jdv_schema_t* out) {
    return jdv_schema_list_limited(arena, element, nullable, out,
                                   &JDV_DEFAULT_LIMITS);
}

int jdv_schema_record_limited(jdv_arena_t* arena,
                              const jdv_schema_field_input_t* fields,
                              size_t count, int open, int nullable,
                              jdv_schema_t* out,
                              const jdv_limits_t* limits) {
    if (!out || (count && !fields) || (open != 0 && open != 1) ||
        (nullable != 0 && nullable != 1)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    jdv_schema_t source;
    memset(&source, 0, sizeof(source));
    source.kind = JDV_SCHEMA_RECORD;
    source.nullable = nullable;
    source.as.record.fields = fields;
    source.as.record.len = count;
    source.as.record.open = open;
    jdv_schema_t result;
    jdv_budget_t budget = {limits, 0, 0, 0, 0, 0};
    size_t mark = arena->used;
    if (clone_schema_at(arena, &source, &result, 0, &budget) < 0) {
        arena->used = mark;
        return -1;
    }
    *out = result;
    return 0;
}

int jdv_schema_record(jdv_arena_t* arena,
                      const jdv_schema_field_input_t* fields, size_t count,
                      int open, int nullable, jdv_schema_t* out) {
    return jdv_schema_record_limited(arena, fields, count, open, nullable, out,
                                     &JDV_DEFAULT_LIMITS);
}

static const jdv_schema_field_t* schema_field(const jdv_schema_t* schema,
                                               jdv_slice_t name) {
    for (size_t i = 0; i < schema->as.record.len; i++)
        if (slice_compare(schema->as.record.fields[i].name, name) == 0)
            return &schema->as.record.fields[i];
    return NULL;
}

static const jdv_record_field_t* value_field(const jdv_value_t* value,
                                              jdv_slice_t name) {
    for (size_t i = 0; i < value->as.record.len; i++)
        if (slice_compare(value->as.record.fields[i].key, name) == 0)
            return &value->as.record.fields[i];
    return NULL;
}

static jdv_relation_t relation_merge(jdv_relation_t a, jdv_relation_t b) {
    if (a == JDV_RELATION_NO || b == JDV_RELATION_NO) return JDV_RELATION_NO;
    if (a == JDV_RELATION_RUNTIME || b == JDV_RELATION_RUNTIME)
        return JDV_RELATION_RUNTIME;
    return JDV_RELATION_YES;
}

static jdv_relation_t schema_accepts_at(const jdv_schema_t* schema,
                                        const jdv_value_t* value) {
    if (schema->kind == JDV_SCHEMA_ANY) return JDV_RELATION_YES;
    if (schema->kind == JDV_SCHEMA_INVALID) return JDV_RELATION_NO;
    if (schema->kind == JDV_SCHEMA_UNKNOWN) return JDV_RELATION_RUNTIME;
    if (value->kind == JDV_NOTHING)
        return schema->nullable ||
               (schema->kind == JDV_SCHEMA_SCALAR &&
                schema->as.scalar == JDV_NOTHING)
            ? JDV_RELATION_YES : JDV_RELATION_NO;
    if (schema->kind == JDV_SCHEMA_SCALAR)
        return value->kind == schema->as.scalar
            ? JDV_RELATION_YES : JDV_RELATION_NO;
    if (schema->kind == JDV_SCHEMA_LIST) {
        if (value->kind != JDV_LIST) return JDV_RELATION_NO;
        jdv_relation_t result = JDV_RELATION_YES;
        for (size_t i = 0; i < value->as.list.len; i++)
            result = relation_merge(result,
                schema_accepts_at(schema->as.element, &value->as.list.items[i]));
        return result;
    }
    if (value->kind != JDV_RECORD) return JDV_RELATION_NO;
    jdv_relation_t result = JDV_RELATION_YES;
    for (size_t i = 0; i < schema->as.record.len; i++) {
        const jdv_schema_field_t* expected = &schema->as.record.fields[i];
        const jdv_record_field_t* actual = value_field(value, expected->name);
        if (!actual) {
            if (!expected->optional) return JDV_RELATION_NO;
        } else {
            result = relation_merge(result,
                schema_accepts_at(expected->schema, &actual->value));
            if (result == JDV_RELATION_NO) return result;
        }
    }
    if (!schema->as.record.open)
        for (size_t i = 0; i < value->as.record.len; i++)
            if (!schema_field(schema, value->as.record.fields[i].key))
                return JDV_RELATION_NO;
    return result;
}

int jdv_schema_accepts_checked(const jdv_schema_t* schema,
                               const jdv_value_t* value,
                               jdv_relation_t* result,
                               const jdv_limits_t* limits) {
    if (!result) return fail_errno(EINVAL);
    if (schema_validate(schema, limits) < 0 ||
        value_validate(value, limits) < 0) return -1;
    jdv_relation_t checked = schema_accepts_at(schema, value);
    *result = checked;
    return 0;
}

int jdv_schema_accepts(const jdv_schema_t* schema, const jdv_value_t* value) {
    jdv_relation_t result;
    return jdv_schema_accepts_checked(schema, value, &result,
                                      &JDV_DEFAULT_LIMITS) == 0 &&
           result == JDV_RELATION_YES;
}

static jdv_relation_t schema_relation_at(const jdv_schema_t* source,
                                         const jdv_schema_t* destination) {
    if (source->kind == JDV_SCHEMA_INVALID) return JDV_RELATION_YES;
    if (destination->kind == JDV_SCHEMA_ANY) return JDV_RELATION_YES;
    if (destination->kind == JDV_SCHEMA_INVALID) return JDV_RELATION_NO;
    if (source->kind == JDV_SCHEMA_UNKNOWN ||
        destination->kind == JDV_SCHEMA_UNKNOWN) return JDV_RELATION_RUNTIME;
    if (source->kind == JDV_SCHEMA_ANY)
        return destination->kind == JDV_SCHEMA_ANY
            ? JDV_RELATION_YES : JDV_RELATION_NO;
    int source_nothing = source->kind == JDV_SCHEMA_SCALAR &&
                         source->as.scalar == JDV_NOTHING;
    int destination_nothing = destination->kind == JDV_SCHEMA_SCALAR &&
                              destination->as.scalar == JDV_NOTHING;
    if (source_nothing)
        return destination->nullable || destination_nothing
            ? JDV_RELATION_YES : JDV_RELATION_NO;
    if (source->nullable && !destination->nullable) return JDV_RELATION_NO;
    if (destination_nothing || source->kind != destination->kind)
        return JDV_RELATION_NO;
    if (source->kind == JDV_SCHEMA_SCALAR)
        return source->as.scalar == destination->as.scalar
            ? JDV_RELATION_YES : JDV_RELATION_NO;
    if (source->kind == JDV_SCHEMA_LIST)
        return schema_relation_at(source->as.element, destination->as.element);
    if (source->as.record.open && !destination->as.record.open)
        return JDV_RELATION_NO;
    jdv_relation_t result = JDV_RELATION_YES;
    for (size_t i = 0; i < destination->as.record.len; i++) {
        const jdv_schema_field_t* wanted = &destination->as.record.fields[i];
        const jdv_schema_field_t* offered = schema_field(source, wanted->name);
        if (!offered) {
            if (!wanted->optional) return JDV_RELATION_NO;
            if (source->as.record.open) result = JDV_RELATION_RUNTIME;
            continue;
        }
        if (!wanted->optional && offered->optional) return JDV_RELATION_NO;
        result = relation_merge(result,
            schema_relation_at(offered->schema, wanted->schema));
        if (result == JDV_RELATION_NO) return result;
    }
    if (!destination->as.record.open)
        for (size_t i = 0; i < source->as.record.len; i++)
            if (!schema_field(destination, source->as.record.fields[i].name))
                return JDV_RELATION_NO;
    return result;
}

int jdv_schema_relation_checked(const jdv_schema_t* source,
                                const jdv_schema_t* destination,
                                jdv_relation_t* result,
                                const jdv_limits_t* limits) {
    if (!result) return fail_errno(EINVAL);
    if (schema_validate(source, limits) < 0 ||
        schema_validate(destination, limits) < 0) return -1;
    jdv_relation_t checked = schema_relation_at(source, destination);
    *result = checked;
    return 0;
}

jdv_relation_t jdv_schema_relation(const jdv_schema_t* source,
                                   const jdv_schema_t* destination) {
    jdv_relation_t result;
    return jdv_schema_relation_checked(source, destination, &result,
                                       &JDV_DEFAULT_LIMITS) == 0
        ? result : JDV_RELATION_NO;
}

int jdv_schema_set_init(jdv_schema_set_t* out,
                        const jdv_schema_t* declared,
                        const jdv_schema_t* guaranteed,
                        const jdv_limits_t* limits) {
    if (!out) return fail_errno(EINVAL);
    if (schema_validate(declared, limits) < 0 ||
        schema_validate(guaranteed, limits) < 0) return -1;
    jdv_relation_t relation;
    if (jdv_schema_relation_checked(guaranteed, declared, &relation, limits) < 0)
        return -1;
    if (relation == JDV_RELATION_NO) return fail_errno(EDOM);
    jdv_schema_set_t result;
    memset(&result, 0, sizeof(result));
    result.declared_schema = declared;
    result.guaranteed_schema = guaranteed;
    result.validation_scope = JDV_VALIDATION_NONE;
    *out = result;
    return 0;
}

static int schema_set_valid(const jdv_schema_set_t* set,
                            const jdv_limits_t* limits) {
    if (!set) return fail_errno(EINVAL);
    if (schema_validate(set->declared_schema, limits) < 0 ||
        schema_validate(set->guaranteed_schema, limits) < 0)
        return -1;
    if (set->validation_scope < JDV_VALIDATION_NONE ||
        set->validation_scope > JDV_VALIDATION_FAILED)
        return fail_errno(EINVAL);
    int terminal = set->validation_scope == JDV_VALIDATION_COMPLETE ||
                   set->validation_scope == JDV_VALIDATION_FAILED;
    if ((set->validation_scope != JDV_VALIDATION_NONE &&
         set->validation_scope != JDV_VALIDATION_FAILED &&
         !set->observed_schema) ||
        (terminal != (set->final_schema != NULL))) return fail_errno(EINVAL);
    if (set->observed_schema && schema_validate(set->observed_schema, limits) < 0)
        return -1;
    if (set->final_schema && schema_validate(set->final_schema, limits) < 0)
        return -1;
    return 0;
}

int jdv_schema_set_transition(jdv_schema_set_t* set,
                              const jdv_schema_t* observed,
                              const jdv_schema_t* final_schema,
                              jdv_validation_scope_t scope,
                              const jdv_limits_t* limits) {
    if (!set) return fail_errno(EINVAL);
    if (schema_set_valid(set, limits) < 0) return -1;
    if (set->validation_scope == JDV_VALIDATION_COMPLETE ||
        set->validation_scope == JDV_VALIDATION_FAILED)
        return fail_errno(EALREADY);
    if (scope <= set->validation_scope || scope > JDV_VALIDATION_FAILED)
        return fail_errno(EINVAL);
    int terminal = scope == JDV_VALIDATION_COMPLETE ||
                   scope == JDV_VALIDATION_FAILED;
    if ((scope != JDV_VALIDATION_FAILED && !observed) ||
        (terminal != (final_schema != NULL))) return fail_errno(EINVAL);
    if (observed && schema_validate(observed, limits) < 0) return -1;
    if (final_schema && schema_validate(final_schema, limits) < 0) return -1;
    jdv_schema_set_t result = *set;
    result.observed_schema = observed;
    result.final_schema = final_schema;
    result.validation_scope = scope;
    *set = result;
    return 0;
}

int jdv_stream_end_init(jdv_stream_end_t* out) {
    if (!out) return fail_errno(EINVAL);
    jdv_stream_end_t result;
    memset(&result, 0, sizeof(result));
    result.version = JDV_STREAM_END_VERSION;
    result.struct_size = (uint32_t)sizeof(result);
    result.state = JDV_STREAM_INCOMPLETE;
    result.completeness = JDV_COMPLETENESS_UNKNOWN;
    *out = result;
    return 0;
}

int jdv_stream_end_validate(const jdv_stream_end_t* end,
                            const jdv_limits_t* limits) {
    if (!end || limits_valid(limits) < 0) return fail_errno(EINVAL);
    if (end->version != JDV_STREAM_END_VERSION ||
        end->struct_size != sizeof(*end)) return fail_errno(EPROTONOSUPPORT);
    if (end->state < JDV_STREAM_COMPLETE || end->state > JDV_STREAM_INCOMPLETE ||
        end->completeness < JDV_COMPLETENESS_UNKNOWN ||
        end->completeness > JDV_COMPLETENESS_PARTIAL ||
        (end->final_hash_present != 0 && end->final_hash_present != 1) ||
        end->final_hash_algorithm < JDV_HASH_NONE ||
        end->final_hash_algorithm > JDV_HASH_BLAKE3 ||
        (end->truncated != 0 && end->truncated != 1) ||
        (end->reason.len && !end->reason.data) ||
        !utf8_valid(end->reason.data, end->reason.len) ||
        contains_nul(end->reason.data, end->reason.len)) return fail_errno(EINVAL);
    if (end->reason.len > limits->max_aggregate_bytes) return fail_errno(E2BIG);
    if (end->final_schema && schema_validate(end->final_schema, limits) < 0)
        return -1;
    if (end->final_hash_present !=
        (end->final_hash_algorithm != JDV_HASH_NONE)) return fail_errno(EINVAL);
    if (end->state == JDV_STREAM_COMPLETE) {
        if (end->completeness != JDV_COMPLETENESS_COMPLETE || end->truncated ||
            !end->final_schema || !end->final_hash_present ||
            end->final_hash_algorithm != JDV_HASH_BLAKE3)
            return fail_errno(EINVAL);
    } else if (end->completeness == JDV_COMPLETENESS_COMPLETE ||
               (end->truncated && end->completeness != JDV_COMPLETENESS_PARTIAL)) {
        return fail_errno(EINVAL);
    }
    return 0;
}

void jdv_writer_init(jdv_writer_t* writer, jdv_sink_t sink, void* context) {
    if (!writer) return;
    writer->sink = sink;
    writer->context = context;
    writer->bytes_written = 0;
    writer->error = sink ? 0 : EINVAL;
}

static int writer_error(jdv_writer_t* writer, int error) {
    if (writer && !writer->error) writer->error = error ? error : EIO;
    errno = writer ? writer->error : EINVAL;
    return -1;
}

int jdv_writer_write(jdv_writer_t* writer, const void* data, size_t size) {
    if (!writer) return fail_errno(EINVAL);
    if (writer->error) return writer_error(writer, writer->error);
    if (size && !data) return writer_error(writer, EINVAL);
    uint64_t amount = (uint64_t)size;
    if ((size_t)amount != size || writer->bytes_written > UINT64_MAX - amount)
        return writer_error(writer, EOVERFLOW);
    const uint8_t* bytes = data;
    size_t done = 0;
    while (done < size) {
        size_t request = size - done;
        if (request > (size_t)LONG_MAX) request = (size_t)LONG_MAX;
        errno = 0;
        long written = writer->sink(writer->context, bytes + done, request);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || (size_t)written > request)
            return writer_error(writer, written < 0 && errno ? errno : EIO);
        done += (size_t)written;
        writer->bytes_written += (uint64_t)written;
    }
    return 0;
}

int jdv_writer_finish(jdv_writer_t* writer) {
    if (!writer) return fail_errno(EINVAL);
    if (writer->error) return writer_error(writer, writer->error);
    return 0;
}

static int write_u8(jdv_writer_t* writer, uint8_t value) {
    return jdv_writer_write(writer, &value, 1);
}

static int write_u64(jdv_writer_t* writer, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++)
        bytes[7u - i] = (uint8_t)(value >> (i * 8u));
    return jdv_writer_write(writer, bytes, sizeof(bytes));
}

static int write_slice(jdv_writer_t* writer, jdv_slice_t slice) {
    return write_u64(writer, (uint64_t)slice.len) < 0 ||
           jdv_writer_write(writer, slice.data, slice.len) < 0 ? -1 : 0;
}

static int encode_value_at(const jdv_value_t* value, jdv_writer_t* writer) {
    if (write_u8(writer, (uint8_t)value->kind) < 0) return -1;
    switch (value->kind) {
        case JDV_NOTHING:
            return 0;
        case JDV_BOOL:
            return write_u8(writer, (uint8_t)value->as.boolean);
        case JDV_I64:
        case JDV_TIMESTAMP:
        case JDV_DURATION:
            return write_u64(writer, (uint64_t)value->as.i64);
        case JDV_U64:
        case JDV_FILESIZE:
            return write_u64(writer, value->as.u64);
        case JDV_STRING:
        case JDV_BYTES:
        case JDV_PATH:
        case JDV_SYMBOL:
            return write_slice(writer, value->as.slice);
        case JDV_LIST:
            if (write_u64(writer, (uint64_t)value->as.list.len) < 0) return -1;
            for (size_t i = 0; i < value->as.list.len; i++)
                if (encode_value_at(&value->as.list.items[i], writer) < 0)
                    return -1;
            return 0;
        case JDV_RECORD:
            if (write_u64(writer, (uint64_t)value->as.record.len) < 0) return -1;
            for (size_t i = 0; i < value->as.record.len; i++)
                if (write_slice(writer, value->as.record.fields[i].key) < 0 ||
                    encode_value_at(&value->as.record.fields[i].value, writer) < 0)
                    return -1;
            return 0;
    }
    return writer_error(writer, EINVAL);
}

static int encode_schema_at(const jdv_schema_t* schema, jdv_writer_t* writer) {
    if (write_u8(writer, (uint8_t)schema->kind) < 0 ||
        write_u8(writer, (uint8_t)schema->nullable) < 0) return -1;
    if (special_schema_kind(schema->kind)) return 0;
    if (schema->kind == JDV_SCHEMA_SCALAR)
        return write_u8(writer, (uint8_t)schema->as.scalar);
    if (schema->kind == JDV_SCHEMA_LIST)
        return encode_schema_at(schema->as.element, writer);
    if (write_u8(writer, (uint8_t)schema->as.record.open) < 0 ||
        write_u64(writer, (uint64_t)schema->as.record.len) < 0) return -1;
    for (size_t i = 0; i < schema->as.record.len; i++) {
        const jdv_schema_field_t* field = &schema->as.record.fields[i];
        if (write_slice(writer, field->name) < 0 ||
            write_u8(writer, (uint8_t)field->optional) < 0 ||
            encode_schema_at(field->schema, writer) < 0) return -1;
    }
    return 0;
}

typedef struct {
    uint8_t* data;
    uint64_t used;
    uint64_t limit;
} snapshot_sink_t;

static long snapshot_sink(void* context, const void* data, size_t size) {
    snapshot_sink_t* sink = context;
    if ((uint64_t)size > sink->limit -
                         (sink->used <= sink->limit ? sink->used : sink->limit)) {
        errno = E2BIG;
        return -1;
    }
    if (sink->data) memcpy(sink->data + (size_t)sink->used, data, size);
    sink->used += (uint64_t)size;
    return (long)size;
}

static int value_snapshot(const jdv_value_t* value,
                          const jdv_limits_t* limits,
                          uint8_t** data, size_t* size) {
    if (value_validate(value, limits) < 0) return -1;
    snapshot_sink_t count = {NULL, 0, limits->max_encoded_bytes};
    jdv_writer_t counter;
    jdv_writer_init(&counter, snapshot_sink, &count);
    if (jdv_writer_write(&counter, "JDV1", 4) < 0 ||
        encode_value_at(value, &counter) < 0) return -1;
    if (count.used > SIZE_MAX) return fail_errno(EOVERFLOW);
    uint8_t* bytes = malloc((size_t)count.used);
    if (!bytes) return fail_errno(ENOMEM);
    snapshot_sink_t output = {bytes, 0, count.used};
    jdv_writer_t writer;
    jdv_writer_init(&writer, snapshot_sink, &output);
    if (jdv_writer_write(&writer, "JDV1", 4) < 0 ||
        encode_value_at(value, &writer) < 0 || output.used != count.used) {
        int error = errno ? errno : EIO;
        free(bytes);
        return fail_errno(error);
    }
    *data = bytes;
    *size = (size_t)count.used;
    return 0;
}

static int schema_snapshot(const jdv_schema_t* schema,
                           const jdv_limits_t* limits,
                           uint8_t** data, size_t* size) {
    if (schema_validate(schema, limits) < 0) return -1;
    snapshot_sink_t count = {NULL, 0, limits->max_encoded_bytes};
    jdv_writer_t counter;
    jdv_writer_init(&counter, snapshot_sink, &count);
    if (jdv_writer_write(&counter, "JSC1", 4) < 0 ||
        encode_schema_at(schema, &counter) < 0) return -1;
    if (count.used > SIZE_MAX) return fail_errno(EOVERFLOW);
    uint8_t* bytes = malloc((size_t)count.used);
    if (!bytes) return fail_errno(ENOMEM);
    snapshot_sink_t output = {bytes, 0, count.used};
    jdv_writer_t writer;
    jdv_writer_init(&writer, snapshot_sink, &output);
    if (jdv_writer_write(&writer, "JSC1", 4) < 0 ||
        encode_schema_at(schema, &writer) < 0 || output.used != count.used) {
        int error = errno ? errno : EIO;
        free(bytes);
        return fail_errno(error);
    }
    *data = bytes;
    *size = (size_t)count.used;
    return 0;
}

int jdv_encode_limited(const jdv_value_t* value, jdv_writer_t* writer,
                       const jdv_limits_t* limits) {
    if (!writer || limits_valid(limits) < 0) return fail_errno(EINVAL);
    if (writer->error) return writer_error(writer, writer->error);
    uint8_t* snapshot;
    size_t size;
    if (value_snapshot(value, limits, &snapshot, &size) < 0)
        return writer_error(writer, errno);
    int result = jdv_writer_write(writer, snapshot, size);
    free(snapshot);
    return result;
}

int jdv_encode(const jdv_value_t* value, jdv_writer_t* writer) {
    return jdv_encode_limited(value, writer, &JDV_DEFAULT_LIMITS);
}

int jdv_schema_encode_limited(const jdv_schema_t* schema, jdv_writer_t* writer,
                              const jdv_limits_t* limits) {
    if (!writer || limits_valid(limits) < 0) return fail_errno(EINVAL);
    if (writer->error) return writer_error(writer, writer->error);
    uint8_t* snapshot;
    size_t size;
    if (schema_snapshot(schema, limits, &snapshot, &size) < 0)
        return writer_error(writer, errno);
    int result = jdv_writer_write(writer, snapshot, size);
    free(snapshot);
    return result;
}

int jdv_schema_encode(const jdv_schema_t* schema, jdv_writer_t* writer) {
    return jdv_schema_encode_limited(schema, writer, &JDV_DEFAULT_LIMITS);
}

static const uint8_t* reader_take(jdv_reader_t* reader, size_t size) {
    if (reader->error) return NULL;
    if (size > reader->size - reader->offset) {
        reader->error = EBADMSG;
        return NULL;
    }
    const uint8_t* result = reader->data + reader->offset;
    reader->offset += size;
    return result;
}

static int read_u8(jdv_reader_t* reader, uint8_t* out) {
    const uint8_t* data = reader_take(reader, 1);
    if (!data) return -1;
    *out = data[0];
    return 0;
}

static int read_u64(jdv_reader_t* reader, uint64_t* out) {
    const uint8_t* data = reader_take(reader, 8);
    if (!data) return -1;
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value = (value << 8) | data[i];
    *out = value;
    return 0;
}

static int read_limited_count(jdv_reader_t* reader, size_t limit, size_t* out) {
    uint64_t value;
    if (read_u64(reader, &value) < 0) return -1;
    if ((uintmax_t)value > (uintmax_t)limit) {
        reader->error = E2BIG;
        return -1;
    }
    if ((uintmax_t)value > (uintmax_t)SIZE_MAX) {
        reader->error = EOVERFLOW;
        return -1;
    }
    *out = (size_t)value;
    return 0;
}

static int decode_slice(jdv_reader_t* reader, jdv_arena_t* arena,
                        jdv_slice_t* out) {
    size_t size;
    if (reader->budget.aggregate_bytes >
        reader->budget.limits->max_aggregate_bytes) {
        reader->error = E2BIG;
        return -1;
    }
    size_t remaining = reader->budget.limits->max_aggregate_bytes -
                       reader->budget.aggregate_bytes;
    if (read_limited_count(reader, remaining, &size) < 0) return -1;
    if (budget_bytes(&reader->budget, size) < 0) {
        reader->error = errno;
        return -1;
    }
    const uint8_t* data = reader_take(reader, size);
    if (!data && size) return -1;
    if (copy_slice(arena, data, size, out) < 0) {
        reader->error = errno;
        return -1;
    }
    return 0;
}

static int64_t decode_i64(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX) return (int64_t)bits;
    return -(int64_t)(UINT64_MAX - bits) - 1;
}

static int decode_value_at(jdv_reader_t* reader, jdv_arena_t* arena,
                           jdv_value_t* out, size_t depth) {
    if (budget_node(&reader->budget, depth) < 0) {
        reader->error = errno;
        return -1;
    }
    uint8_t tag;
    if (read_u8(reader, &tag) < 0 || tag > JDV_RECORD) {
        if (!reader->error) reader->error = EBADMSG;
        return -1;
    }
    jdv_value_t result = scalar_value((jdv_kind_t)tag);
    uint64_t bits;
    switch (result.kind) {
        case JDV_NOTHING:
            break;
        case JDV_BOOL: {
            uint8_t boolean;
            if (read_u8(reader, &boolean) < 0 || boolean > 1) {
                if (!reader->error) reader->error = EBADMSG;
                return -1;
            }
            result.as.boolean = boolean;
            break;
        }
        case JDV_I64:
        case JDV_TIMESTAMP:
        case JDV_DURATION:
            if (read_u64(reader, &bits) < 0) return -1;
            result.as.i64 = decode_i64(bits);
            break;
        case JDV_U64:
        case JDV_FILESIZE:
            if (read_u64(reader, &result.as.u64) < 0) return -1;
            break;
        case JDV_STRING:
        case JDV_BYTES:
        case JDV_PATH:
        case JDV_SYMBOL:
            if (decode_slice(reader, arena, &result.as.slice) < 0) return -1;
            if (scalar_slice_valid(result.kind, result.as.slice) < 0) {
                reader->error = errno;
                return -1;
            }
            break;
        case JDV_LIST: {
            size_t count;
            size_t remaining = reader->budget.limits->max_list_items -
                (reader->budget.list_items <= reader->budget.limits->max_list_items
                    ? reader->budget.list_items
                    : reader->budget.limits->max_list_items);
            if (read_limited_count(reader, remaining, &count) < 0) return -1;
            if (budget_count(&reader->budget.list_items, count,
                             reader->budget.limits->max_list_items) < 0) {
                reader->error = errno;
                return -1;
            }
            result.as.list.len = count;
            result.as.list.items = NULL;
            if (count) {
                jdv_value_t* items = arena_array(arena, count, sizeof(*items),
                                                 _Alignof(jdv_value_t));
                if (!items) {
                    reader->error = errno;
                    return -1;
                }
                result.as.list.items = items;
                for (size_t i = 0; i < count; i++)
                    if (decode_value_at(reader, arena, &items[i], depth + 1) < 0)
                        return -1;
            }
            break;
        }
        case JDV_RECORD: {
            size_t count;
            size_t remaining = reader->budget.limits->max_record_fields -
                (reader->budget.record_fields <=
                     reader->budget.limits->max_record_fields
                    ? reader->budget.record_fields
                    : reader->budget.limits->max_record_fields);
            if (read_limited_count(reader, remaining, &count) < 0) return -1;
            if (budget_count(&reader->budget.record_fields, count,
                             reader->budget.limits->max_record_fields) < 0) {
                reader->error = errno;
                return -1;
            }
            result.as.record.len = count;
            result.as.record.fields = NULL;
            if (count) {
                jdv_record_field_t* fields = arena_array(
                    arena, count, sizeof(*fields), _Alignof(jdv_record_field_t));
                if (!fields) {
                    reader->error = errno;
                    return -1;
                }
                result.as.record.fields = fields;
                for (size_t i = 0; i < count; i++) {
                    if (decode_slice(reader, arena, &fields[i].key) < 0) return -1;
                    if (!utf8_valid(fields[i].key.data, fields[i].key.len) ||
                        contains_nul(fields[i].key.data, fields[i].key.len) ||
                        (i && slice_compare(fields[i - 1].key,
                                            fields[i].key) >= 0)) {
                        reader->error = EBADMSG;
                        return -1;
                    }
                    if (decode_value_at(reader, arena, &fields[i].value,
                                        depth + 1) < 0) return -1;
                }
            }
            break;
        }
    }
    *out = result;
    return 0;
}

int jdv_decode_exact_limited(jdv_arena_t* arena, const void* data, size_t size,
                             jdv_value_t* out, size_t* consumed,
                             const jdv_limits_t* limits) {
    if (!out || (size && !data)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    uint64_t encoded_size = (uint64_t)size;
    if ((size_t)encoded_size != size || encoded_size > limits->max_encoded_bytes)
        return fail_errno(E2BIG);
    jdv_reader_t reader = {
        (const uint8_t*)data, size, 0, 0, {limits, 0, 0, 0, 0, 0}
    };
    size_t mark = arena->used;
    jdv_value_t result;
    const uint8_t* magic = reader_take(&reader, 4);
    if (!magic || memcmp(magic, "JDV1", 4) != 0 ||
        decode_value_at(&reader, arena, &result, 0) < 0 ||
        reader.offset != reader.size) {
        int error = reader.error ? reader.error : EBADMSG;
        arena->used = mark;
        return fail_errno(error);
    }
    *out = result;
    if (consumed) *consumed = reader.offset;
    return 0;
}

int jdv_decode_exact(jdv_arena_t* arena, const void* data, size_t size,
                     jdv_value_t* out, size_t* consumed) {
    return jdv_decode_exact_limited(arena, data, size, out, consumed,
                                    &JDV_DEFAULT_LIMITS);
}

int jdv_decode(jdv_arena_t* arena, const void* data, size_t size,
               jdv_value_t* out, size_t* consumed) {
    return jdv_decode_exact(arena, data, size, out, consumed);
}

static int decode_schema_at(jdv_reader_t* reader, jdv_arena_t* arena,
                            jdv_schema_t* out, size_t depth) {
    if (budget_node(&reader->budget, depth) < 0) {
        reader->error = errno;
        return -1;
    }
    uint8_t kind, nullable;
    if (read_u8(reader, &kind) < 0 || read_u8(reader, &nullable) < 0 ||
        kind > JDV_SCHEMA_INVALID || nullable > 1) {
        if (!reader->error) reader->error = EBADMSG;
        return -1;
    }
    jdv_schema_t result;
    memset(&result, 0, sizeof(result));
    result.kind = (jdv_schema_kind_t)kind;
    result.nullable = nullable;
    if (special_schema_kind(result.kind)) {
        if (nullable) {
            reader->error = EBADMSG;
            return -1;
        }
    } else if (result.kind == JDV_SCHEMA_SCALAR) {
        uint8_t scalar;
        if (read_u8(reader, &scalar) < 0 ||
            !scalar_schema_kind((jdv_kind_t)scalar)) {
            if (!reader->error) reader->error = EBADMSG;
            return -1;
        }
        result.as.scalar = (jdv_kind_t)scalar;
    } else if (result.kind == JDV_SCHEMA_LIST) {
        jdv_schema_t* element = arena_alloc(arena, sizeof(*element),
                                            _Alignof(jdv_schema_t));
        if (!element) {
            reader->error = errno;
            return -1;
        }
        result.as.element = element;
        if (decode_schema_at(reader, arena, element, depth + 1) < 0) return -1;
    } else if (result.kind == JDV_SCHEMA_RECORD) {
        uint8_t open;
        size_t count;
        size_t remaining = reader->budget.limits->max_schema_fields -
            (reader->budget.schema_fields <=
                 reader->budget.limits->max_schema_fields
                ? reader->budget.schema_fields
                : reader->budget.limits->max_schema_fields);
        if (read_u8(reader, &open) < 0 || open > 1 ||
            read_limited_count(reader, remaining, &count) < 0) {
            if (!reader->error) reader->error = EBADMSG;
            return -1;
        }
        if (budget_count(&reader->budget.schema_fields, count,
                         reader->budget.limits->max_schema_fields) < 0) {
            reader->error = errno;
            return -1;
        }
        result.as.record.open = open;
        result.as.record.len = count;
        result.as.record.fields = NULL;
        if (count) {
            jdv_schema_field_t* fields = arena_array(
                arena, count, sizeof(*fields), _Alignof(jdv_schema_field_t));
            if (!fields) {
                reader->error = errno;
                return -1;
            }
            result.as.record.fields = fields;
            for (size_t i = 0; i < count; i++) {
                uint8_t optional;
                if (decode_slice(reader, arena, &fields[i].name) < 0) return -1;
                if (!utf8_valid(fields[i].name.data, fields[i].name.len) ||
                    contains_nul(fields[i].name.data, fields[i].name.len) ||
                    (i && slice_compare(fields[i - 1].name,
                                        fields[i].name) >= 0) ||
                    read_u8(reader, &optional) < 0 || optional > 1) {
                    if (!reader->error) reader->error = EBADMSG;
                    return -1;
                }
                fields[i].optional = optional;
                jdv_schema_t* child = arena_alloc(arena, sizeof(*child),
                                                  _Alignof(jdv_schema_t));
                if (!child) {
                    reader->error = errno;
                    return -1;
                }
                fields[i].schema = child;
                if (decode_schema_at(reader, arena, child, depth + 1) < 0)
                    return -1;
            }
        }
    } else {
        reader->error = EBADMSG;
        return -1;
    }
    *out = result;
    return 0;
}

int jdv_schema_decode_exact_limited(jdv_arena_t* arena, const void* data,
                                    size_t size, jdv_schema_t* out,
                                    size_t* consumed,
                                    const jdv_limits_t* limits) {
    if (!out || (size && !data)) return fail_errno(EINVAL);
    if (arena_valid(arena) < 0 || limits_valid(limits) < 0) return -1;
    uint64_t encoded_size = (uint64_t)size;
    if ((size_t)encoded_size != size || encoded_size > limits->max_encoded_bytes)
        return fail_errno(E2BIG);
    jdv_reader_t reader = {
        (const uint8_t*)data, size, 0, 0, {limits, 0, 0, 0, 0, 0}
    };
    size_t mark = arena->used;
    jdv_schema_t result;
    const uint8_t* magic = reader_take(&reader, 4);
    if (!magic || memcmp(magic, "JSC1", 4) != 0 ||
        decode_schema_at(&reader, arena, &result, 0) < 0 ||
        reader.offset != reader.size) {
        int error = reader.error ? reader.error : EBADMSG;
        arena->used = mark;
        return fail_errno(error);
    }
    *out = result;
    if (consumed) *consumed = reader.offset;
    return 0;
}

int jdv_schema_decode_exact(jdv_arena_t* arena, const void* data, size_t size,
                            jdv_schema_t* out, size_t* consumed) {
    return jdv_schema_decode_exact_limited(arena, data, size, out, consumed,
                                           &JDV_DEFAULT_LIMITS);
}

int jdv_schema_decode(jdv_arena_t* arena, const void* data, size_t size,
                      jdv_schema_t* out, size_t* consumed) {
    return jdv_schema_decode_exact(arena, data, size, out, consumed);
}

static long hash_sink(void* context, const void* data, size_t size) {
    blake3_update(context, data, (uint64_t)size);
    return (long)size;
}

int jdv_hash_limited(const jdv_value_t* value,
                     uint8_t out[JDV_HASH_SIZE],
                     const jdv_limits_t* limits) {
    if (!out || limits_valid(limits) < 0) return fail_errno(EINVAL);
    struct blake3 hash;
    blake3_init(&hash);
    jdv_writer_t writer;
    jdv_writer_init(&writer, hash_sink, &hash);
    if (jdv_encode_limited(value, &writer, limits) < 0 ||
        jdv_writer_finish(&writer) < 0) return -1;
    uint8_t result[JDV_HASH_SIZE];
    blake3_final(&hash, result, JDV_HASH_SIZE);
    memcpy(out, result, sizeof(result));
    return 0;
}

int jdv_hash(const jdv_value_t* value, uint8_t out[JDV_HASH_SIZE]) {
    return jdv_hash_limited(value, out, &JDV_DEFAULT_LIMITS);
}
