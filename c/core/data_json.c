// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_json.h"
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    jdv_arena_t* arena;
    const unsigned char* p;
    const unsigned char* end;
    const jdv_limits_t* limits;
    size_t nodes, bytes;
} json_reader_t;

static int failure(int error) { errno = error; return -1; }
static int arena_valid(const jdv_arena_t* a) {
    if (!a || (!a->data && a->capacity)) return failure(EINVAL);
    if (a->used > a->capacity || (uintptr_t)a->data > UINTPTR_MAX - a->capacity)
        return failure(EOVERFLOW);
    return 0;
}
static void* allocate(jdv_arena_t* a, size_t size) {
    if (arena_valid(a)) return NULL;
    if (!a->data) { errno = ENOSPC; return NULL; }
    size_t alignment = _Alignof(max_align_t);
    size_t padding = (alignment - (uintptr_t)(a->data + a->used) % alignment) % alignment;
    if (padding > a->capacity - a->used || size > a->capacity - a->used - padding) {
        errno = ENOSPC; return NULL;
    }
    void* result = a->data + a->used + padding;
    a->used += padding + size;
    memset(result, 0, size);
    return result;
}
static int copy(jdv_arena_t* a, const void* data, size_t size, jdv_slice_t* result) {
    if (size == SIZE_MAX) return failure(EOVERFLOW);
    unsigned char* p = allocate(a, size + 1);
    if (!p) return -1;
    if (size) memcpy(p, data, size);
    *result = (jdv_slice_t){p, size}; return 0;
}
static void whitespace(json_reader_t* r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')) r->p++;
}
static int budget(json_reader_t* r, size_t bytes) {
    if (bytes > r->limits->max_aggregate_bytes - r->bytes) return failure(E2BIG);
    r->bytes += bytes; return 0;
}
static int hex4(json_reader_t* r, uint32_t* out) {
    if ((size_t)(r->end - r->p) < 4) return failure(EINVAL);
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = *r->p++;
        unsigned h = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                     c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                     c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
        if (h == 16) return failure(EINVAL);
        n = n * 16 + h;
    }
    *out = n; return 0;
}
static int parse_string(json_reader_t* r, jdv_slice_t* out) {
    if (r->p == r->end || *r->p++ != '"') return failure(EINVAL);
    const unsigned char* begin = r->p;
    const unsigned char* scan = begin;
    int escaped = 0;
    while (scan < r->end) {
        unsigned char c = *scan++;
        if (!escaped && c == '"') break;
        if (!escaped && c == '\\') escaped = 1;
        else escaped = 0;
    }
    if (scan == begin || scan[-1] != '"' || escaped) return failure(EINVAL);
    size_t capacity = (size_t)(scan - begin);
    unsigned char* value = allocate(r->arena, capacity);
    if (!value) return -1;
    size_t used = 0;
    while (r->p < scan - 1) {
        unsigned char c = *r->p++;
        if (c < 0x20) return failure(EINVAL);
        if (c != '\\') { value[used++] = c; continue; }
        if (r->p >= scan - 1) return failure(EINVAL);
        c = *r->p++;
        switch (c) {
            case '"': case '\\': case '/': value[used++] = c; break;
            case 'b': value[used++] = '\b'; break;
            case 'f': value[used++] = '\f'; break;
            case 'n': value[used++] = '\n'; break;
            case 'r': value[used++] = '\r'; break;
            case 't': value[used++] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (hex4(r, &cp)) return -1;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if ((size_t)(r->end - r->p) < 6 || r->p[0] != '\\' || r->p[1] != 'u') return failure(EILSEQ);
                    r->p += 2;
                    uint32_t low;
                    if (hex4(r, &low) || low < 0xdc00 || low > 0xdfff) return failure(EILSEQ);
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return failure(EILSEQ);
                if (cp < 0x80) value[used++] = (unsigned char)cp;
                else if (cp < 0x800) {
                    value[used++] = (unsigned char)(0xc0 | (cp >> 6));
                    value[used++] = (unsigned char)(0x80 | (cp & 63));
                } else if (cp < 0x10000) {
                    value[used++] = (unsigned char)(0xe0 | (cp >> 12));
                    value[used++] = (unsigned char)(0x80 | ((cp >> 6) & 63));
                    value[used++] = (unsigned char)(0x80 | (cp & 63));
                } else {
                    value[used++] = (unsigned char)(0xf0 | (cp >> 18));
                    value[used++] = (unsigned char)(0x80 | ((cp >> 12) & 63));
                    value[used++] = (unsigned char)(0x80 | ((cp >> 6) & 63));
                    value[used++] = (unsigned char)(0x80 | (cp & 63));
                }
                break;
            }
            default: return failure(EINVAL);
        }
        if (r->p > scan - 1) return failure(EINVAL);
    }
    value[used] = 0;
    if (!jash_json_utf8_valid_n(value, used)) return failure(EILSEQ);
    r->p = scan;
    if (budget(r, used)) return -1;
    *out = (jdv_slice_t){value, used}; return 0;
}
static int digit(unsigned char c) { return c >= '0' && c <= '9'; }
static const unsigned char* number_end(const unsigned char* p, const unsigned char* end) {
    if (p == end) return NULL;
    if (*p == '-') p++;
    if (p == end || !digit(*p)) return NULL;
    if (*p == '0') p++;
    else while (p < end && digit(*p)) p++;
    if (p < end && *p == '.') {
        p++; if (p == end || !digit(*p)) return NULL;
        while (p < end && digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++; if (p < end && (*p == '+' || *p == '-')) p++;
        if (p == end || !digit(*p)) return NULL;
        while (p < end && digit(*p)) p++;
    }
    return p;
}
static int parse_value(json_reader_t* r, size_t depth, jdj_value_t* out) {
    whitespace(r);
    if (depth > r->limits->max_depth || depth > JDV_MAX_DEPTH || ++r->nodes > r->limits->max_nodes) return failure(E2BIG);
    if (r->p == r->end) return failure(EINVAL);
    jdj_value_t v = {0}; unsigned char c = *r->p;
    if (c == '"') { v.kind = JDJ_STRING; if (parse_string(r, &v.scalar)) return -1; }
    else if (c == '{' || c == '[') {
        int object = c == '{'; unsigned char close = object ? '}' : ']';
        v.kind = object ? JDJ_OBJECT : JDJ_ARRAY; r->p++; whitespace(r);
        jdj_node_t* tail = NULL;
        if (r->p < r->end && *r->p == close) r->p++;
        else for (;;) {
            if (v.count >= (object ? r->limits->max_record_fields : r->limits->max_list_items)) return failure(E2BIG);
            jdj_node_t* node = allocate(r->arena, sizeof(*node)); if (!node) return -1;
            if (object) {
                if (parse_string(r, &node->key)) return -1;
                for (const jdj_node_t* n = v.children; n; n = n->next)
                    if (n->key.len == node->key.len && !memcmp(n->key.data, node->key.data, n->key.len)) return failure(EEXIST);
                whitespace(r); if (r->p == r->end || *r->p++ != ':') return failure(EINVAL);
            }
            if (parse_value(r, depth + 1, &node->value)) return -1;
            if (tail) tail->next = node; else v.children = node; tail = node; v.count++;
            whitespace(r); if (r->p == r->end) return failure(EINVAL);
            if (*r->p == close) { r->p++; break; }
            if (*r->p++ != ',') return failure(EINVAL);
            whitespace(r);
        }
    } else if (c == '-' || digit(c)) {
        const unsigned char* start = r->p;
        const unsigned char* end = number_end(start, r->end);
        if (!end) return failure(EINVAL);
        r->p = end;
        size_t len = (size_t)(r->p - start);
        v.kind = JDJ_NUMBER;
        if (budget(r, len) || copy(r->arena, start, len, &v.scalar)) return -1;
    } else {
        const char* token = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : "";
        size_t length = strlen(token);
        if (!length || (size_t)(r->end - r->p) < length || memcmp(r->p, token, length)) return failure(EINVAL);
        r->p += length; v.kind = c == 'n' ? JDJ_NULL : JDJ_BOOL; v.boolean = c == 't';
    }
    *out = v; return 0;
}
int jdj_parse(jdv_arena_t* arena, const void* bytes, size_t size, const jdv_limits_t* limits, jdj_value_t* out) {
    if (!bytes || !limits || !out || arena_valid(arena)) return failure(EINVAL);
    if (size > limits->max_encoded_bytes) return failure(E2BIG);
    uintptr_t begin = (uintptr_t)bytes;
    if (begin > UINTPTR_MAX - size) return failure(EOVERFLOW);
    uintptr_t unused = (uintptr_t)arena->data + arena->used;
    uintptr_t arena_end = (uintptr_t)arena->data + arena->capacity;
    if (size && unused < arena_end && begin < arena_end && begin + size > unused)
        return failure(EINVAL);
    size_t mark = arena->used; jdj_value_t value;
    json_reader_t r = {arena, bytes, (const unsigned char*)bytes + size, limits, 0, 0};
    int rc = parse_value(&r, 0, &value); whitespace(&r);
    if (!rc && r.p != r.end) rc = failure(EINVAL);
    if (!rc) rc = jdj_validate(&value, limits);
    if (rc) { arena->used = mark; return -1; }
    *out = value; return 0;
}
const jdj_value_t* jdj_get(const jdj_value_t* value, const void* key, size_t len) {
    if (!value || value->kind != JDJ_OBJECT || (!key && len)) return NULL;
    size_t visited = 0;
    for (const jdj_node_t* n = value->children; n && visited++ < value->count; n = n->next)
        if (n->key.len == len && (!len || !memcmp(n->key.data, key, len))) return &n->value;
    return NULL;
}

typedef struct {
    const jdv_limits_t* limits;
    size_t nodes, bytes, fields, items;
    uint64_t encoded;
} json_budget_t;
static int count_budget(size_t* used, size_t amount, size_t limit) {
    if (*used > limit || amount > limit - *used) return failure(E2BIG);
    *used += amount; return 0;
}
static int encoded_budget(json_budget_t* b, uint64_t amount) {
    if (b->encoded > b->limits->max_encoded_bytes || amount > b->limits->max_encoded_bytes - b->encoded)
        return failure(E2BIG);
    b->encoded += amount; return 0;
}
static int validate_text(jdv_slice_t text, json_budget_t* b) {
    if (!jash_json_utf8_valid_n(text.data, text.len)) return failure(EILSEQ);
    if (count_budget(&b->bytes, text.len, b->limits->max_aggregate_bytes) || encoded_budget(b, 2)) return -1;
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = text.data[i];
        if (encoded_budget(b, c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : 1)) return -1;
    }
    return 0;
}
static int validate_value(const jdj_value_t* v, json_budget_t* b, size_t depth) {
    if (!v) return failure(EINVAL);
    if (depth >= b->limits->max_depth || depth >= JDV_MAX_DEPTH ||
        count_budget(&b->nodes, 1, b->limits->max_nodes)) return failure(E2BIG);
    switch (v->kind) {
        case JDJ_NULL: return encoded_budget(b, 4);
        case JDJ_BOOL:
            if (v->boolean != 0 && v->boolean != 1) return failure(EINVAL);
            return encoded_budget(b, v->boolean ? 4 : 5);
        case JDJ_NUMBER:
            if (!v->scalar.data || !v->scalar.len ||
                (uintptr_t)v->scalar.data > UINTPTR_MAX - v->scalar.len ||
                number_end(v->scalar.data, v->scalar.data + v->scalar.len) != v->scalar.data + v->scalar.len)
                return failure(EINVAL);
            if (count_budget(&b->bytes, v->scalar.len, b->limits->max_aggregate_bytes)) return -1;
            return encoded_budget(b, v->scalar.len);
        case JDJ_STRING: return validate_text(v->scalar, b);
        case JDJ_ARRAY: case JDJ_OBJECT: {
            int object = v->kind == JDJ_OBJECT;
            if (count_budget(object ? &b->fields : &b->items, v->count,
                    object ? b->limits->max_record_fields : b->limits->max_list_items) ||
                encoded_budget(b, 2)) return -1;
            size_t count = 0;
            for (const jdj_node_t* n = v->children; n; n = n->next) {
                if (count >= v->count) return failure(EINVAL);
                if (count && encoded_budget(b, 1)) return -1;
                if (object) {
                    if (validate_text(n->key, b) || encoded_budget(b, 1)) return -1;
                    const jdj_node_t* previous = v->children;
                    for (size_t i = 0; i < count; i++, previous = previous->next)
                        if (previous->key.len == n->key.len && (!n->key.len || !memcmp(previous->key.data, n->key.data, n->key.len)))
                            return failure(EEXIST);
                }
                if (validate_value(&n->value, b, depth + 1)) return -1;
                count++;
            }
            return count == v->count ? 0 : failure(EINVAL);
        }
        default: return failure(EINVAL);
    }
}
int jdj_validate(const jdj_value_t* value, const jdv_limits_t* limits) {
    if (!limits) return failure(EINVAL);
    json_budget_t budget = {.limits = limits};
    return validate_value(value, &budget, 0);
}
static int write_value(jash_json_writer_t* w, const jdj_value_t* v, const jdv_limits_t* limits, size_t depth, size_t* nodes) {
    if (!v || depth > limits->max_depth || depth > JDV_MAX_DEPTH || ++*nodes > limits->max_nodes) return failure(E2BIG);
    switch (v->kind) {
        case JDJ_NULL: jash_jsonw_raw(w, "null"); break;
        case JDJ_BOOL: jash_jsonw_raw(w, v->boolean ? "true" : "false"); break;
        case JDJ_NUMBER:
            /* jdj_write validated the entire lexeme before any output. Keep
             * all digits, including fractional and exponent suffixes, exact. */
            jash_jsonw_raw_n(w, v->scalar.data, v->scalar.len); break;
        case JDJ_STRING: jash_jsonw_string_n(w, v->scalar.data, v->scalar.len); break;
        case JDJ_ARRAY: case JDJ_OBJECT: {
            int object = v->kind == JDJ_OBJECT;
            if (v->count > (object ? limits->max_record_fields : limits->max_list_items)) return failure(E2BIG);
            jash_jsonw_raw(w, object ? "{" : "["); size_t count = 0;
            for (const jdj_node_t* n = v->children; n; n = n->next) {
                if (count++ >= v->count) return failure(EINVAL);
                if (count > 1) jash_jsonw_raw(w, ",");
                if (object) { jash_jsonw_string_n(w, n->key.data, n->key.len); jash_jsonw_raw(w, ":"); }
                if (write_value(w, &n->value, limits, depth + 1, nodes)) return -1;
            }
            if (count != v->count) return failure(EINVAL);
            jash_jsonw_raw(w, object ? "}" : "]"); break;
        }
        default: return failure(EINVAL);
    }
    return w->error ? failure(w->error) : 0;
}
int jdj_write(jash_json_writer_t* w, const jdj_value_t* v, const jdv_limits_t* limits) {
    if (!w || !limits) return failure(EINVAL);
    if (w->error) return failure(w->error);
    if (jdj_validate(v, limits)) return -1;
    size_t nodes = 0; return write_value(w, v, limits, 0, &nodes);
}

static int to_value(jdv_arena_t* arena, const jdj_value_t* v, const jdv_limits_t* limits, size_t depth, jdv_value_t* out) {
    if (depth > limits->max_depth || depth > JDV_MAX_DEPTH) return failure(E2BIG);
    switch (v->kind) {
        case JDJ_NULL: *out = jdv_value_nothing(); return 0;
        case JDJ_BOOL: *out = jdv_value_bool(v->boolean); return 0;
        case JDJ_STRING: return jdv_value_string_limited(arena, v->scalar.data, v->scalar.len, out, limits);
        case JDJ_NUMBER: {
            size_t i = 0; int negative = v->scalar.len && v->scalar.data[0] == '-'; if (negative) i++;
            uint64_t n = 0;
            if (i == v->scalar.len) return failure(EINVAL);
            for (; i < v->scalar.len; i++) {
                unsigned char c = v->scalar.data[i];
                if (!digit(c)) return failure(ENOTSUP);
                if (n > (UINT64_MAX - (c - '0')) / 10) return failure(EOVERFLOW);
                n = n * 10 + (c - '0');
            }
            if (negative) {
                if (n > (uint64_t)INT64_MAX + 1) return failure(EOVERFLOW);
                *out = jdv_value_i64(n == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)n);
            } else *out = jdv_value_u64(n);
            return 0;
        }
        case JDJ_ARRAY: case JDJ_OBJECT: {
            if (v->count > (v->kind == JDJ_OBJECT ? limits->max_record_fields : limits->max_list_items)) return failure(E2BIG);
            jdv_value_t* items = NULL; jdv_record_input_t* fields = NULL;
            if (v->kind == JDJ_OBJECT) fields = calloc(v->count ? v->count : 1, sizeof(*fields));
            else items = calloc(v->count ? v->count : 1, sizeof(*items));
            if (!fields && !items) return -1;
            size_t i = 0; int rc = 0;
            for (const jdj_node_t* n = v->children; n; n = n->next) {
                if (i >= v->count) { rc = failure(EINVAL); break; }
                if (fields) { fields[i].key = n->key; rc = to_value(arena, &n->value, limits, depth + 1, &fields[i].value); }
                else rc = to_value(arena, &n->value, limits, depth + 1, &items[i]);
                if (rc) break;
                i++;
            }
            if (!rc && i != v->count) rc = failure(EINVAL);
            if (!rc) rc = fields ? jdv_value_record_limited(arena, fields, i, out, limits) : jdv_value_list_limited(arena, items, i, out, limits);
            free(fields); free(items); return rc;
        }
        default: return failure(EINVAL);
    }
}
int jdj_to_value(jdv_arena_t* arena, const jdj_value_t* v, const jdv_limits_t* limits, jdv_value_t* out) {
    if (!v || !limits || !out || arena_valid(arena)) return failure(EINVAL);
    if (jdj_validate(v, limits)) return -1;
    size_t mark = arena->used; jdv_value_t result;
    if (to_value(arena, v, limits, 0, &result)) { arena->used = mark; return -1; }
    *out = result; return 0;
}

static int from_value(jdv_arena_t* a, const jdv_value_t* v, const jdv_limits_t* l, size_t depth, jdj_value_t* out) {
    if (depth > l->max_depth || depth > JDV_MAX_DEPTH) return failure(E2BIG);
    jdj_value_t result = {0}; char number[32];
    switch (v->kind) {
        case JDV_NOTHING: result.kind = JDJ_NULL; break;
        case JDV_BOOL: result.kind = JDJ_BOOL; result.boolean = v->as.boolean; break;
        case JDV_I64: case JDV_TIMESTAMP: case JDV_DURATION:
            (void)snprintf(number, sizeof(number), "%" PRId64, v->as.i64); result.kind = JDJ_NUMBER;
            if (copy(a, number, strlen(number), &result.scalar)) return -1;
            break;
        case JDV_U64: case JDV_FILESIZE:
            (void)snprintf(number, sizeof(number), "%" PRIu64, v->as.u64); result.kind = JDJ_NUMBER;
            if (copy(a, number, strlen(number), &result.scalar)) return -1;
            break;
        case JDV_STRING: case JDV_SYMBOL:
            result.kind = JDJ_STRING; if (copy(a, v->as.slice.data, v->as.slice.len, &result.scalar)) return -1; break;
        case JDV_PATH: case JDV_BYTES: {
            /* Use the canonical checked byte projection, then parse that JSON. */
            size_t cap = v->as.slice.len > (SIZE_MAX - 128) / 6 ? 0 : v->as.slice.len * 6 + 128;
            if (!cap) return failure(E2BIG);
            if (l->max_encoded_bytes < cap) cap = (size_t)l->max_encoded_bytes + 1;
            char* text = malloc(cap);
            if (!text) return -1;
            jash_json_buffer_t sink = {text, cap, 0}; jash_json_writer_t writer;
            jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &sink);
            if (v->kind == JDV_PATH) jash_jsonw_text_or_bytes(&writer, v->as.slice.data, v->as.slice.len);
            else jash_jsonw_bytes(&writer, v->as.slice.data, v->as.slice.len);
            int rc = jash_jsonw_finish(&writer);
            if (!rc) rc = jdj_parse(a, text, sink.used, l, &result);
            free(text); if (rc) return -1; break;
        }
        case JDV_LIST: case JDV_RECORD: {
            int object = v->kind == JDV_RECORD; size_t count = object ? v->as.record.len : v->as.list.len;
            if (count > (object ? l->max_record_fields : l->max_list_items)) return failure(E2BIG);
            result.kind = object ? JDJ_OBJECT : JDJ_ARRAY; result.count = count;
            jdj_node_t* tail = NULL;
            for (size_t i = 0; i < count; i++) {
                jdj_node_t* n = allocate(a, sizeof(*n)); if (!n) return -1;
                const jdv_value_t* item = object ? &v->as.record.fields[i].value : &v->as.list.items[i];
                if (object && copy(a, v->as.record.fields[i].key.data, v->as.record.fields[i].key.len, &n->key)) return -1;
                if (from_value(a, item, l, depth + 1, &n->value)) return -1;
                if (tail) tail->next = n; else result.children = n; tail = n;
            }
            break;
        }
        default: return failure(EINVAL);
    }
    *out = result; return 0;
}
int jdj_from_value(jdv_arena_t* a, const jdv_value_t* v, const jdv_limits_t* l, jdj_value_t* out) {
    if (!v || !l || !out || arena_valid(a)) return failure(EINVAL);
    jdv_data_t data = jdv_data_scalar(*v);
    if (jdv_data_validate(&data, l)) return -1;
    size_t mark = a->used; jdj_value_t result;
    if (from_value(a, v, l, 0, &result) || jdj_validate(&result, l)) { a->used = mark; return -1; }
    *out = result; return 0;
}
int jdv_json_parse(jdv_arena_t* a, const void* bytes, size_t size, const jdv_limits_t* l, jdv_value_t* out) {
    if (!a || !l || !out) return failure(EINVAL);
    size_t mark = a->used; jdj_value_t json; jdv_value_t result;
    if (jdj_parse(a, bytes, size, l, &json) || jdj_to_value(a, &json, l, &result)) { a->used = mark; return -1; }
    *out = result; return 0;
}
int jdv_json_write(jash_json_writer_t* w, const jdv_value_t* v, const jdv_limits_t* l) {
    if (!w || !v || !l || l->max_aggregate_bytes > (SIZE_MAX - 65536) / 8) return failure(EINVAL);
    size_t capacity = l->max_aggregate_bytes * 8 + 65536;
    void* memory = malloc(capacity); if (!memory) return -1;
    jdv_arena_t a; jdv_arena_init(&a, memory, capacity); jdj_value_t json;
    int rc = jdj_from_value(&a, v, l, &json);
    if (!rc) rc = jdj_write(w, &json, l);
    free(memory); return rc;
}
