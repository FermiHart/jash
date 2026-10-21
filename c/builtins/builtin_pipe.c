// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
/* Strict bounded JSONL operators and explicit JTV2 adapters.
 * JSONL is the default. --data/--data-v2 select JTV2 in and out;
 * --from-data selects JTV2 -> JSONL; --to-data selects JSONL -> JTV2.
 * Ordinary shell pipes remain bytes. Input is fully admitted before output:
 * 16 MiB wire, 64 KiB JSON row, 65536 rows, 64 MiB semantic arena. */
#include "builtin.h"
#include <core/data_emit.h>
#include <core/data_json.h>
#include <core/data_wire_import.h>
#include <core/output_budget.h>
#include <core/structured_output.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPE_INPUT_MAX (16u * 1024u * 1024u)
#define PIPE_ARENA_MAX (64u * 1024u * 1024u)
#define PIPE_OUTPUT_MAX (64u * 1024u * 1024u)
#define PIPE_ROW_MAX 65536u
#define PIPE_LINE_MAX 65536u
#define PIPE_FIELD_MAX 256u
#define PIPE_NUM_MAX 4096u

typedef struct { int negative, exponent; size_t len; unsigned char digits[PIPE_NUM_MAX + 1]; } decimal_t;
typedef struct { jdj_value_t json; jdv_value_t native; uint64_t credit; size_t order; int has_native; } row_t;
typedef struct {
    unsigned char* input; size_t size;
    void* storage; jdv_arena_t arena; jdv_limits_t limits;
    row_t* rows; size_t count, capacity;
    void* schema_storage; void* item_storage;
    jdv_wire_import_t imported;
    const jdv_schema_t* declared; const jdv_schema_t* guaranteed;
} dataset_t;
typedef struct { char name[PIPE_FIELD_MAX]; char op[3]; jdj_value_t rhs; } condition_t;
typedef struct {
    int data_input, data_output, reverse, counts;
    char fields[64][PIPE_FIELD_MAX]; size_t nfields;
    char group[PIPE_FIELD_MAX];
    condition_t conditions[64]; size_t nconditions;
} options_t;

enum { OP_WHERE, OP_PICK, OP_SORT, OP_COUNT, OP_SUM, OP_AVG, OP_UNIQ, OP_DESCRIBE };
static const char* const NAMES[] = {"where", "pick", "sortby", "count", "sum", "avg", "uniqby", "describe"};
static int bad(int code) { errno = code; return -1; }
static const char* skip_space(const char* p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static void* reserve(dataset_t* d, size_t size) {
    size_t align = _Alignof(max_align_t);
    size_t pad = (align - (uintptr_t)(d->arena.data + d->arena.used) % align) % align;
    if (pad > d->arena.capacity - d->arena.used || size > d->arena.capacity - d->arena.used - pad) { errno = ENOSPC; return NULL; }
    void* p = d->arena.data + d->arena.used + pad; d->arena.used += pad + size; memset(p, 0, size); return p;
}
static jdv_slice_t literal(const char* s) { return (jdv_slice_t){(const uint8_t*)s, strlen(s)}; }
static int append(dataset_t* d, row_t row) {
    if (d->count >= PIPE_ROW_MAX) return bad(E2BIG);
    if (d->count == d->capacity) {
        size_t cap = d->capacity ? d->capacity * 2 : 64;
        row_t* p = realloc(d->rows, cap * sizeof(*p)); if (!p) return -1;
        d->rows = p; d->capacity = cap;
    }
    row.order = d->count; d->rows[d->count++] = row; return 0;
}
static void dispose(dataset_t* d) {
    if (d->imported.active) (void)jdv_wire_import_close(&d->imported);
    free(d->input); free(d->storage); free(d->rows); free(d->schema_storage); free(d->item_storage);
    memset(d, 0, sizeof(*d));
}
static int init(dataset_t* d) {
    d->storage = malloc(PIPE_ARENA_MAX); if (!d->storage) return -1;
    jdv_arena_init(&d->arena, d->storage, PIPE_ARENA_MAX);
    d->limits = JDV_DEFAULT_LIMITS;
    d->limits.max_depth = 64; d->limits.max_nodes = 65536;
    d->limits.max_aggregate_bytes = 2u * 1024u * 1024u;
    d->limits.max_record_fields = 4096; d->limits.max_list_items = 65536;
    d->limits.max_schema_fields = 4096; d->limits.max_encoded_bytes = 2u * 1024u * 1024u;
    return 0;
}
static int read_bytes(dataset_t* d) {
    size_t cap = 65536; d->input = malloc(cap + 1); if (!d->input) return -1;
    for (;;) {
        if (d->size == cap) {
            if (cap == PIPE_INPUT_MAX) { int extra = fgetc(stdin); if (extra != EOF || ferror(stdin)) return bad(E2BIG); break; }
            cap *= 2; unsigned char* p = realloc(d->input, cap + 1); if (!p) return -1; d->input = p;
        }
        size_t got = fread(d->input + d->size, 1, cap - d->size, stdin); d->size += got;
        if (!got) { if (ferror(stdin)) return bad(EIO); break; }
    }
    d->input[d->size] = 0; return 0;
}
static int clone_native(dataset_t* d, const jdv_value_t* v, jdv_value_t* out) {
    jdv_value_t list;
    if (jdv_value_list_limited(&d->arena, v, 1, &list, &d->limits)) return -1;
    *out = list.as.list.items[0]; return 0;
}
static int read_rows(dataset_t* d, int native) {
    if (read_bytes(d)) return -1;
    if (native) {
        d->schema_storage = malloc(2u * 1024u * 1024u); d->item_storage = malloc(4u * 1024u * 1024u);
        if (!d->schema_storage || !d->item_storage) return -1;
        jdv_wire_limits_t limits; jdv_wire_limits_init(&limits);
        limits.max_wire_bytes = PIPE_INPUT_MAX; limits.max_frame_bytes = 2u * 1024u * 1024u;
        limits.max_items = PIPE_ROW_MAX; limits.value_limits = d->limits;
        if (jdv_wire_import_v2_open(&d->imported, d->input, d->size,
                d->schema_storage, 2u * 1024u * 1024u, d->item_storage, 4u * 1024u * 1024u, &limits)) return -1;
        d->declared = &d->imported.declared; d->guaranteed = &d->imported.guaranteed;
        for (;;) {
            jdv_stream_record_t record;
            jdv_stream_pull_result_t result = jdv_stream_pull(&d->imported.cursor, &record);
            if (result == JDV_PULL_ITEM) {
                if (record.item->kind != JDV_RECORD) return bad(EDOM);
                row_t row = {0}; row.credit = record.logical_bytes; row.has_native = 1;
                if (clone_native(d, record.item, &row.native) || jdj_from_value(&d->arena, record.item, &d->limits, &row.json) || append(d, row)) return -1;
            } else {
                if (result != JDV_PULL_END || !record.terminal || record.terminal->state != JDV_STREAM_COMPLETE ||
                    record.terminal->completeness != JDV_COMPLETENESS_COMPLETE || record.terminal->truncated || record.terminal->errors_seen ||
                    !record.terminal->final_hash_present ||
                    record.terminal->producer_status || record.terminal->transform_status || record.terminal->transport_status)
                    return bad(EPROTO);
                break;
            }
        }
        return 0;
    }
    for (size_t offset = 0; offset < d->size;) {
        size_t end = offset; while (end < d->size && d->input[end] != '\n') end++;
        size_t length = end - offset;
        if (length > PIPE_LINE_MAX) return bad(E2BIG);
        int nonblank = 0;
        for (size_t i = offset; i < end; i++) if (d->input[i] != ' ' && d->input[i] != '\t' && d->input[i] != '\r') { nonblank = 1; break; }
        if (nonblank) {
            row_t row = {0}; row.credit = length;
            if (jdj_parse(&d->arena, d->input + offset, length, &d->limits, &row.json) || row.json.kind != JDJ_OBJECT) return bad(EINVAL);
            if (append(d, row)) return -1;
        }
        offset = end < d->size ? end + 1 : end;
    }
    return 0;
}

/* Bounded exact base-ten arithmetic. No binary floating-point conversion can
 * merge 2^53 and 2^53+1 or produce an inf/NaN JSON token. */
static void normalize(decimal_t* n) {
    while (n->len && n->digits[n->len - 1] == 0) n->len--;
    if (!n->len) { n->negative = 0; n->exponent = 0; return; }
    size_t zeros = 0; while (zeros < n->len && n->digits[zeros] == 0) zeros++;
    if (zeros) { memmove(n->digits, n->digits + zeros, n->len - zeros); n->len -= zeros; n->exponent += (int)zeros; }
}
static int decimal_parse(jdv_slice_t value, decimal_t* out) {
    decimal_t n = {0}; size_t start = 0, end = value.len, point = SIZE_MAX;
    if (!end) return bad(EINVAL);
    if (value.data[0] == '-') { n.negative = 1; start++; }
    size_t exponent_at = start; while (exponent_at < end && value.data[exponent_at] != 'e' && value.data[exponent_at] != 'E') exponent_at++;
    int exponent = 0;
    if (exponent_at < end) {
        size_t i = exponent_at + 1; int negative = 0;
        if (i < end && (value.data[i] == '+' || value.data[i] == '-')) { negative = value.data[i] == '-'; i++; }
        if (i == end) return bad(EINVAL);
        for (; i < end; i++) {
            if (value.data[i] < '0' || value.data[i] > '9' || exponent > 4096) return bad(EOVERFLOW);
            exponent = exponent * 10 + value.data[i] - '0';
        }
        if (exponent > 4096) return bad(EOVERFLOW);
        if (negative) exponent = -exponent;
    }
    for (size_t i = start; i < exponent_at; i++) {
        if (value.data[i] == '.') { if (point != SIZE_MAX) return bad(EINVAL); point = i; }
        else if (value.data[i] < '0' || value.data[i] > '9') return bad(EINVAL);
    }
    if (point != SIZE_MAX) exponent -= (int)(exponent_at - point - 1);
    if (exponent < -4096 || exponent > 4096) return bad(EOVERFLOW);
    n.exponent = exponent;
    for (size_t i = exponent_at; i > start;) {
        unsigned char c = value.data[--i]; if (c == '.') continue;
        if (n.len == PIPE_NUM_MAX) return bad(EOVERFLOW);
        n.digits[n.len++] = (unsigned char)(c - '0');
    }
    if (!n.len) return bad(EINVAL);
    normalize(&n); *out = n; return 0;
}
static int magnitude_compare(const decimal_t* a, const decimal_t* b) {
    if (!a->len || !b->len) return (a->len != 0) - (b->len != 0);
    int atop = a->exponent + (int)a->len, btop = b->exponent + (int)b->len;
    if (atop != btop) return (atop > btop) - (atop < btop);
    int floor = a->exponent < b->exponent ? a->exponent : b->exponent;
    for (int pos = atop - 1; pos >= floor; pos--) {
        unsigned av = pos >= a->exponent ? a->digits[pos - a->exponent] : 0;
        unsigned bv = pos >= b->exponent ? b->digits[pos - b->exponent] : 0;
        if (av != bv) return (av > bv) - (av < bv);
    }
    return 0;
}
static int decimal_compare(const decimal_t* a, const decimal_t* b) {
    if (a->negative != b->negative) return a->negative ? -1 : 1;
    int cmp = magnitude_compare(a, b); return a->negative ? -cmp : cmp;
}
static int decimal_add(decimal_t* a, const decimal_t* b) {
    if (!b->len) return 0;
    if (!a->len) { *a = *b; return 0; }
    int exp = a->exponent < b->exponent ? a->exponent : b->exponent;
    size_t al = a->len + (size_t)(a->exponent - exp), bl = b->len + (size_t)(b->exponent - exp);
    size_t len = al > bl ? al : bl; if (len > PIPE_NUM_MAX) return bad(EOVERFLOW);
    decimal_t result = {0}; result.exponent = exp; result.len = len;
    const decimal_t* large = a; const decimal_t* small = b;
    if (a->negative != b->negative && magnitude_compare(a, b) < 0) { large = b; small = a; }
    result.negative = large->negative; int carry = 0;
    for (size_t i = 0; i < len; i++) {
        int position = (int)i + exp;
        int ai = position - large->exponent, bi = position - small->exponent;
        int av = ai >= 0 && (size_t)ai < large->len ? large->digits[ai] : 0;
        int bv = bi >= 0 && (size_t)bi < small->len ? small->digits[bi] : 0;
        if (a->negative == b->negative) { int sum = av + bv + carry; result.digits[i] = (unsigned char)(sum % 10); carry = sum / 10; }
        else { int difference = av - bv - carry; carry = difference < 0; result.digits[i] = (unsigned char)(difference + (carry ? 10 : 0)); }
    }
    if (carry) { if (result.len == PIPE_NUM_MAX || a->negative != b->negative) return bad(EOVERFLOW); result.digits[result.len++] = (unsigned char)carry; }
    normalize(&result); *a = result; return 0;
}
static int decimal_divide(decimal_t* n, uint64_t denominator) {
    if (!denominator) return bad(EINVAL);
    uint64_t remainder = 0;
    for (size_t i = n->len; i > 0;) { i--; uint64_t value = remainder * 10 + n->digits[i]; n->digits[i] = (unsigned char)(value / denominator); remainder = value % denominator; }
    for (int extra = 0; remainder && extra < 18; extra++) {
        if (n->len == PIPE_NUM_MAX) return bad(EOVERFLOW);
        memmove(n->digits + 1, n->digits, n->len); n->len++; n->exponent--;
        remainder *= 10; n->digits[0] = (unsigned char)(remainder / denominator); remainder %= denominator;
    }
    /* Round the final retained decimal place, ties away from zero. */
    if (remainder && remainder >= (denominator + 1) / 2) {
        size_t i = 0; while (i < n->len && n->digits[i] == 9) n->digits[i++] = 0;
        if (i == n->len) { if (n->len == PIPE_NUM_MAX) return bad(EOVERFLOW); n->digits[n->len++] = 0; }
        n->digits[i]++;
    }
    normalize(n); return 0;
}
static int number_value(dataset_t* d, const decimal_t* n, jdj_value_t* out) {
    char* text = reserve(d, PIPE_NUM_MAX + 32); if (!text) return -1;
    size_t used = 0; if (n->negative && n->len) text[used++] = '-';
    if (!n->len) text[used++] = '0';
    else {
        for (size_t i = n->len; i > 0;) text[used++] = (char)('0' + n->digits[--i]);
        if (n->exponent > 0 && used + (size_t)n->exponent <= PIPE_NUM_MAX + 1) {
            for (int i = 0; i < n->exponent; i++) text[used++] = '0';
        } else if (n->exponent) used += (size_t)snprintf(text + used, 30, "e%d", n->exponent);
    }
    *out = (jdj_value_t){.kind = JDJ_NUMBER, .scalar = {(const uint8_t*)text, used}}; return 0;
}
static int unsigned_value(dataset_t* d, uint64_t n, jdj_value_t* out) {
    char* text = reserve(d, 32); if (!text) return -1;
    int length = snprintf(text, 32, "%" PRIu64, n);
    *out = (jdj_value_t){.kind = JDJ_NUMBER, .scalar = {(const uint8_t*)text, (size_t)length}}; return 0;
}
static int slice_compare(jdv_slice_t a, jdv_slice_t b) {
    size_t n = a.len < b.len ? a.len : b.len; int cmp = n ? memcmp(a.data, b.data, n) : 0;
    return cmp ? (cmp > 0 ? 1 : -1) : (a.len > b.len) - (a.len < b.len);
}
static int compare(const jdj_value_t* a, const jdj_value_t* b, int* order, size_t depth) {
    if (depth > 64) return bad(E2BIG);
    if (!a || !b) { *order = (a == NULL) - (b == NULL); return 0; }
    if (a->kind != b->kind) { *order = (a->kind > b->kind) - (a->kind < b->kind); return 0; }
    if (a->kind == JDJ_NUMBER) { decimal_t av, bv; if (decimal_parse(a->scalar, &av) || decimal_parse(b->scalar, &bv)) return -1; *order = decimal_compare(&av, &bv); return 0; }
    if (a->kind == JDJ_STRING) { *order = slice_compare(a->scalar, b->scalar); return 0; }
    if (a->kind == JDJ_BOOL) { *order = a->boolean - b->boolean; return 0; }
    if (a->kind == JDJ_NULL) { *order = 0; return 0; }
    if (a->count != b->count) { *order = (a->count > b->count) - (a->count < b->count); return 0; }
    const jdj_node_t* right = b->children;
    for (const jdj_node_t* left = a->children; left; left = left->next) {
        const jdj_value_t* other = a->kind == JDJ_OBJECT ? jdj_get(b, left->key.data, left->key.len) : right ? &right->value : NULL;
        if (compare(&left->value, other, order, depth + 1)) return -1;
        if (*order) return 0;
        if (right) right = right->next;
    }
    *order = 0; return 0;
}
static const jdj_value_t* field(const row_t* row, const char* name) { return jdj_get(&row->json, name, strlen(name)); }
static const jdv_value_t* native_field(const row_t* row, const char* name) {
    if (!row->has_native || row->native.kind != JDV_RECORD) return NULL;
    size_t size = strlen(name);
    for (size_t i = 0; i < row->native.as.record.len; i++) {
        const jdv_record_field_t* f = &row->native.as.record.fields[i];
        if (f->key.len == size && !memcmp(f->key.data, name, size)) return &f->value;
    }
    return NULL;
}
/* Native identity includes the JDV kind. A FILESIZE is not a dimensionless
 * U64, and an arbitrary user record is not a BYTES value even when both have
 * the same JSON projection. Inputs were validated by the JTV2 importer. */
static int native_compare(const jdv_value_t* a, const jdv_value_t* b, int* order, size_t depth) {
    if (depth >= JDV_MAX_DEPTH) return bad(E2BIG);
    if (!a || !b) { *order = (a == NULL) - (b == NULL); return 0; }
    if (a->kind != b->kind) { *order = (a->kind > b->kind) - (a->kind < b->kind); return 0; }
    switch (a->kind) {
        case JDV_NOTHING: *order = 0; return 0;
        case JDV_BOOL: *order = a->as.boolean - b->as.boolean; return 0;
        case JDV_I64: case JDV_TIMESTAMP: case JDV_DURATION:
            *order = (a->as.i64 > b->as.i64) - (a->as.i64 < b->as.i64); return 0;
        case JDV_U64: case JDV_FILESIZE:
            *order = (a->as.u64 > b->as.u64) - (a->as.u64 < b->as.u64); return 0;
        case JDV_STRING: case JDV_BYTES: case JDV_PATH: case JDV_SYMBOL:
            *order = slice_compare(a->as.slice, b->as.slice); return 0;
        case JDV_RECORD: case JDV_LIST: {
            int object = a->kind == JDV_RECORD;
            size_t ac = object ? a->as.record.len : a->as.list.len;
            size_t bc = object ? b->as.record.len : b->as.list.len;
            if (ac != bc) { *order = (ac > bc) - (ac < bc); return 0; }
            for (size_t i = 0; i < ac; i++) {
                if (object) {
                    *order = slice_compare(a->as.record.fields[i].key, b->as.record.fields[i].key);
                    if (*order) return 0;
                }
                const jdv_value_t* av = object ? &a->as.record.fields[i].value : &a->as.list.items[i];
                const jdv_value_t* bv = object ? &b->as.record.fields[i].value : &b->as.list.items[i];
                if (native_compare(av, bv, order, depth + 1)) return -1;
                if (*order) return 0;
            }
            *order = 0; return 0;
        }
        default: return bad(EINVAL);
    }
}
static int word(const char** input, char* out, size_t capacity) {
    const char* p = skip_space(*input); const char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    size_t length = (size_t)(p - start); if (!length || length >= capacity) return bad(EINVAL);
    memcpy(out, start, length); out[length] = 0; *input = skip_space(p); return 0;
}
static int conditions(dataset_t* d, const char* p, options_t* o) {
    for (;;) {
        p = skip_space(p); if (!*p || o->nconditions == 64) return bad(EINVAL);
        condition_t* condition = &o->conditions[o->nconditions++];
        const char* begin = p; while (*p && *p != '=' && *p != '!' && *p != '<' && *p != '>' && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t length = (size_t)(p - begin); if (!length || length >= sizeof(condition->name)) return bad(EINVAL);
        memcpy(condition->name, begin, length); p = skip_space(p);
        if (*p != '=' && *p != '!' && *p != '<' && *p != '>') return bad(EINVAL);
        condition->op[0] = *p++; if (*p == '=') condition->op[1] = *p++;
        if (condition->op[0] == '!' && condition->op[1] != '=') return bad(EINVAL);
        p = skip_space(p); begin = p; int string = 0, escaped = 0, nesting = 0;
        while (*p) {
            if (!string && !nesting && *p == ',') break;
            if (!escaped && *p == '"') string = !string;
            if (!string && (*p == '[' || *p == '{')) nesting++;
            if (!string && (*p == ']' || *p == '}')) nesting--;
            if (string && !escaped && *p == '\\') escaped = 1; else escaped = 0;
            p++;
        }
        const char* end = p; while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;
        length = (size_t)(end - begin); if (!length || string || nesting) return bad(EINVAL);
        if ((*begin >= '0' && *begin <= '9') || *begin == '-' || *begin == '"' || *begin == '{' || *begin == '[' ||
            (length == 4 && !memcmp(begin, "true", 4)) || (length == 5 && !memcmp(begin, "false", 5)) || (length == 4 && !memcmp(begin, "null", 4))) {
            if (jdj_parse(&d->arena, begin, length, &d->limits, &condition->rhs)) return -1;
        } else {
            char* text = reserve(d, length + 1); if (!text) return -1; memcpy(text, begin, length);
            condition->rhs = (jdj_value_t){.kind = JDJ_STRING, .scalar = {(const uint8_t*)text, length}};
        }
        if (condition->op[0] == '<' || condition->op[0] == '>') {
            decimal_t number; if (condition->rhs.kind != JDJ_NUMBER || decimal_parse(condition->rhs.scalar, &number)) return bad(EINVAL);
        }
        if (!*p) break;
        p++;
    }
    return 0;
}
static int options(dataset_t* d, int op, const char* args, options_t* o) {
    const char* p = skip_space(args ? args : ""); int format = 0;
    for (;;) {
        const char* start = p; char option[32];
        if (strncmp(p, "--", 2)) break;
        if (word(&p, option, sizeof(option))) return -1;
        if (!strcmp(option, "--data") || !strcmp(option, "--data-v2")) { if (format++) return bad(EINVAL); o->data_input = o->data_output = 1; }
        else if (!strcmp(option, "--from-data")) { if (format++) return bad(EINVAL); o->data_input = 1; }
        else if (!strcmp(option, "--to-data")) { if (format++) return bad(EINVAL); o->data_output = 1; }
        else if (!strcmp(option, "--json")) { if (format++) return bad(EINVAL); }
        else { p = start; return bad(EINVAL); }
    }
    if (op == OP_WHERE) return conditions(d, p, o);
    if (op == OP_PICK) {
        while (*p) {
            p = skip_space(p); const char* start = p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
            size_t n = (size_t)(p - start);
            if (!n || n >= PIPE_FIELD_MAX || o->nfields == 64 || start[0] == '-') return bad(EINVAL);
            memcpy(o->fields[o->nfields], start, n);
            for (size_t i = 0; i < o->nfields; i++) if (!strcmp(o->fields[i], o->fields[o->nfields])) return bad(EEXIST);
            o->nfields++; p = skip_space(p); if (*p == ',') { p = skip_space(p + 1); if (!*p) return bad(EINVAL); }
        }
        return o->nfields ? 0 : bad(EINVAL);
    }
    if (op == OP_DESCRIBE) return *p ? bad(EINVAL) : 0;
    char token[PIPE_FIELD_MAX];
    if (op == OP_SORT || op == OP_UNIQ) {
        if (word(&p, token, sizeof(token))) return -1;
        if (!strcmp(token, op == OP_SORT ? "-r" : "-c")) {
            if (op == OP_SORT) o->reverse = 1; else o->counts = 1;
            if (word(&p, token, sizeof(token))) return -1;
        }
        if (token[0] == '-' || *p) return bad(EINVAL);
        if (jash_copy_cstr(o->fields[0], sizeof(o->fields[0]), token)) { return bad(E2BIG); } o->nfields = 1; return 0;
    }
    if (op == OP_SUM || op == OP_AVG) {
        if (word(&p, o->fields[0], PIPE_FIELD_MAX) || o->fields[0][0] == '-') return bad(EINVAL);
        o->nfields = 1;
    }
    if (*p) {
        if (word(&p, token, sizeof(token)) || strcmp(token, "-g") || word(&p, o->group, sizeof(o->group)) || *p) return bad(EINVAL);
        if (!strcmp(o->group, op == OP_COUNT ? "count" : op == OP_SUM ? "sum" : "avg")) return bad(EEXIST);
    }
    return 0;
}

static int make_record(dataset_t* d, const char* a, jdj_value_t av, const char* b, jdj_value_t bv, row_t* out) {
    jdj_node_t* first = reserve(d, sizeof(*first) * (b ? 2 : 1)); if (!first) return -1;
    first[0].key = literal(a); first[0].value = av;
    if (b) { first[0].next = &first[1]; first[1].key = literal(b); first[1].value = bv; }
    memset(out, 0, sizeof(*out)); out->json.kind = JDJ_OBJECT; out->json.children = first; out->json.count = b ? 2 : 1; return 0;
}
static int project(dataset_t* d, row_t* row, const options_t* o) {
    jdj_node_t* nodes = reserve(d, sizeof(*nodes) * o->nfields); if (!nodes) return -1;
    jdv_record_input_t native[64]; size_t used = 0;
    for (size_t i = 0; i < o->nfields; i++) {
        const jdj_value_t* v = field(row, o->fields[i]); if (!v) continue;
        nodes[used].key = literal(o->fields[i]); nodes[used].value = *v;
        if (used) nodes[used - 1].next = &nodes[used];
        if (row->has_native) { const jdv_value_t* n = native_field(row, o->fields[i]); if (!n) return bad(EPROTO); native[used].key = nodes[used].key; native[used].value = *n; }
        used++;
    }
    row->json = (jdj_value_t){.kind = JDJ_OBJECT, .children = used ? nodes : NULL, .count = used};
    if (row->has_native && jdv_value_record_limited(&d->arena, native, used, &row->native, &d->limits)) return -1;
    return 0;
}
static const char* sort_field;
static int sort_reverse, sort_error;
static int sort_rows(const void* x, const void* y) {
    const row_t* a = x; const row_t* b = y;
    const jdj_value_t* av = field(a, sort_field); const jdj_value_t* bv = field(b, sort_field);
    int cmp = 0;
    int rc = a->has_native && b->has_native ?
        native_compare(native_field(a, sort_field), native_field(b, sort_field), &cmp, 0) :
        compare(av, bv, &cmp, 0);
    if (rc) { sort_error = errno ? errno : EINVAL; return 0; }
    if (cmp && av && bv && sort_reverse) cmp = -cmp;
    return cmp ? cmp : (a->order > b->order) - (a->order < b->order);
}
static int filter_rows(dataset_t* d, int op, const options_t* o) {
    if (op == OP_PICK) {
        for (size_t i = 0; i < d->count; i++) if (project(d, &d->rows[i], o)) return -1;
        d->declared = d->guaranteed = NULL; return 0;
    }
    if (op == OP_SORT) {
        for (size_t i = 0; i < d->count; i++) {
            if (d->rows[i].has_native) {
                const jdv_value_t* v = native_field(&d->rows[i], o->fields[0]);
                if (v && (v->kind == JDV_RECORD || v->kind == JDV_LIST)) return bad(ENOTSUP);
            } else {
                const jdj_value_t* v = field(&d->rows[i], o->fields[0]);
                if (v && (v->kind == JDJ_OBJECT || v->kind == JDJ_ARRAY)) return bad(ENOTSUP);
            }
        }
        sort_field = o->fields[0]; sort_reverse = o->reverse; sort_error = 0;
        qsort(d->rows, d->count, sizeof(*d->rows), sort_rows);
        return sort_error ? bad(sort_error) : 0;
    }
    size_t out = 0;
    for (size_t i = 0; i < d->count; i++) {
        int keep = 1;
        for (size_t c = 0; c < o->nconditions && keep; c++) {
            const condition_t* condition = &o->conditions[c]; const jdj_value_t* value = field(&d->rows[i], condition->name);
            int cmp = 0; if (!value) { keep = 0; break; }
            if (compare(value, &condition->rhs, &cmp, 0)) return -1;
            switch (condition->op[0]) {
                case '=': keep = cmp == 0; break;
                case '!': keep = cmp != 0; break;
                case '>': keep = value->kind == JDJ_NUMBER && (condition->op[1] ? cmp >= 0 : cmp > 0); break;
                case '<': keep = value->kind == JDJ_NUMBER && (condition->op[1] ? cmp <= 0 : cmp < 0); break;
                default: return bad(EINVAL);
            }
        }
        if (keep) d->rows[out++] = d->rows[i];
    }
    d->count = out; return 0;
}

typedef struct { const jdj_value_t* key; const jdv_value_t* native_key; row_t first; uint64_t count; decimal_t total; unsigned native_kinds; } group_t;
static int reduction_kind(group_t* group, jdv_kind_t kind, int op) {
    if (kind != JDV_I64 && kind != JDV_U64 && kind != JDV_FILESIZE && kind != JDV_DURATION && kind != JDV_TIMESTAMP)
        return bad(EDOM);
    if (kind == JDV_TIMESTAMP && op == OP_SUM) return bad(ENOTSUP);
    unsigned combined = group->native_kinds | (1u << kind);
    unsigned integers = (1u << JDV_I64) | (1u << JDV_U64);
    /* Signed and unsigned dimensionless integers share arithmetic; units do
     * not mix with each other or with a dimensionless integer. */
    if ((combined & ~integers) && (combined & (combined - 1u))) return bad(EDOM);
    group->native_kinds = combined; return 0;
}
static int native_reduction(dataset_t* d, const jdj_value_t* json, unsigned kinds, jdv_value_t* out) {
    jdv_value_t value;
    if (jdj_to_value(&d->arena, json, &d->limits, &value)) return -1;
    if (value.kind == JDV_NOTHING) { *out = value; return 0; }
    if (kinds == (1u << JDV_FILESIZE)) {
        if (value.kind != JDV_U64) return bad(EOVERFLOW);
        value = jdv_value_filesize(value.as.u64);
    } else if (kinds == (1u << JDV_I64) || kinds == (1u << JDV_DURATION) || kinds == (1u << JDV_TIMESTAMP)) {
        if (value.kind == JDV_U64 && value.as.u64 > INT64_MAX) return bad(EOVERFLOW);
        int64_t number = value.kind == JDV_U64 ? (int64_t)value.as.u64 : value.as.i64;
        value = kinds == (1u << JDV_DURATION) ? jdv_value_duration(number) :
            kinds == (1u << JDV_TIMESTAMP) ? jdv_value_timestamp(number) : jdv_value_i64(number);
    }
    *out = value; return 0;
}
#include "pipe_average_fraction.inc"

static int aggregate(dataset_t* d, int op, const options_t* o) {
    const char* grouping = op == OP_UNIQ ? o->fields[0] : o->group;
    size_t cap = 16, count = *grouping ? 0 : 1;
    group_t* groups = calloc(cap, sizeof(*groups)); if (!groups) return -1;
    int rc = -1;
    for (size_t i = 0; i < d->count; i++) {
        row_t* row = &d->rows[i]; const jdj_value_t* key = *grouping ? field(row, grouping) : NULL;
        const jdv_value_t* native_key = *grouping ? native_field(row, grouping) : NULL;
        if (*grouping && !key) continue;
        const jdj_value_t* value = op == OP_SUM || op == OP_AVG ? field(row, o->fields[0]) : NULL;
        if ((op == OP_SUM || op == OP_AVG) && (!value || value->kind == JDJ_NULL)) continue;
        if (value && value->kind != JDJ_NUMBER) { errno = EDOM; goto done; }
        size_t index = 0;
        if (*grouping) {
            for (; index < count; index++) {
                int cmp;
                int compared = native_key && groups[index].native_key ?
                    native_compare(native_key, groups[index].native_key, &cmp, 0) :
                    compare(key, groups[index].key, &cmp, 0);
                if (compared) goto done;
                if (!cmp) break;
            }
            if (index == count) {
                if (count == 4096) { errno = E2BIG; goto done; }
                if (count == cap) { cap *= 2; group_t* more = realloc(groups, cap * sizeof(*more)); if (!more) goto done; groups = more; }
                memset(&groups[count], 0, sizeof(groups[count])); groups[count].key = key; groups[count].native_key = native_key; groups[count].first = *row; count++;
            }
        }
        if (groups[index].count == UINT64_MAX) { errno = EOVERFLOW; goto done; }
        groups[index].count++;
        if (value) {
            const jdv_value_t* native = native_field(row, o->fields[0]);
            jdv_value_t converted;
            /* --to-data avg is a deliberate arithmetic provider over native
             * integer inputs, not a new permissive JSON decimal importer. */
            if (!native && o->data_output && op == OP_AVG) {
                if (jdj_to_value(&d->arena, value, &d->limits, &converted)) goto done;
                native = &converted;
            }
            if (native && reduction_kind(&groups[index], native->kind, op)) goto done;
            decimal_t n; if (decimal_parse(value->scalar, &n) || decimal_add(&groups[index].total, &n)) goto done;
        }
    }
    row_t* output = calloc(count ? count : 1, sizeof(*output)); if (!output) goto done;
    for (size_t i = 0; i < count; i++) {
        if (op == OP_UNIQ && !o->counts) { output[i] = groups[i].first; continue; }
        jdj_value_t value = {0}; const char* label = op == OP_SUM ? "sum" : op == OP_AVG ? "avg" : "count";
        jdv_value_t exact_fraction = jdv_value_nothing(); int fractional = 0;
        if (op == OP_AVG && o->data_output && groups[i].native_kinds &&
            native_average_fraction(d, &groups[i].total, groups[i].count, groups[i].native_kinds,
                                     &exact_fraction, &fractional)) { free(output); goto done; }
        if (op == OP_SUM || (op == OP_AVG && groups[i].count)) {
            if (op == OP_AVG && decimal_divide(&groups[i].total, groups[i].count)) { free(output); goto done; }
            if (number_value(d, &groups[i].total, &value)) { free(output); goto done; }
        } else if (op == OP_AVG) value.kind = JDJ_NULL;
        else if (unsigned_value(d, groups[i].count, &value)) { free(output); goto done; }
        int made = *grouping ? make_record(d, grouping, *groups[i].key, label, value, &output[i]) : make_record(d, label, value, NULL, value, &output[i]);
        if (made) { free(output); goto done; }
        if (o->data_output && (groups[i].native_key || groups[i].native_kinds)) {
            jdv_value_t number = exact_fraction;
            if (!fractional && native_reduction(d, &value, groups[i].native_kinds, &number)) { free(output); goto done; }
            jdv_record_input_t fields[2]; size_t nfields = 0;
            if (*grouping) {
                jdv_value_t group_key;
                if (groups[i].native_key) group_key = *groups[i].native_key;
                else if (jdj_to_value(&d->arena, groups[i].key, &d->limits, &group_key)) { free(output); goto done; }
                fields[nfields++] = (jdv_record_input_t){literal(grouping), group_key};
            }
            fields[nfields++] = (jdv_record_input_t){literal(label), number};
            if (jdv_value_record_limited(&d->arena, fields, nfields, &output[i].native, &d->limits)) { free(output); goto done; }
            output[i].has_native = 1;
        }
    }
    free(d->rows); d->rows = output; d->count = count; d->capacity = count;
    if (op != OP_UNIQ || o->counts) d->declared = d->guaranteed = NULL;
    rc = 0;
 done:
    free(groups); return rc;
}
static const char* kind_name(jdj_kind_t kind) {
    static const char* const names[] = {"null", "bool", "number", "string", "array", "object"};
    return kind <= JDJ_OBJECT ? names[kind] : "unknown";
}
static const char* native_kind_name(jdv_kind_t kind) {
    static const char* const names[] = {"nothing", "bool", "i64", "u64", "filesize", "timestamp", "duration", "string", "bytes", "path", "symbol", "list", "record"};
    return kind <= JDV_RECORD ? names[kind] : "unknown";
}
typedef struct {
    jdv_slice_t key; unsigned kinds; uint64_t count, nulls;
    jdj_value_t samples[3]; jdv_value_t native_samples[3]; size_t nsamples;
} description_t;
static int describe(dataset_t* d, const options_t* opts) {
    description_t* fields = calloc(4096, sizeof(*fields)); if (!fields) return -1;
    size_t count = 0; int rc = -1;
    for (size_t r = 0; r < d->count; r++) for (const jdj_node_t* n = d->rows[r].json.children; n; n = n->next) {
        size_t f = 0; while (f < count && slice_compare(fields[f].key, n->key)) f++;
        if (f == count) { if (count == 4096) { errno = E2BIG; goto done; } fields[count++].key = n->key; }
        const jdv_value_t* native = native_field(&d->rows[r], (const char*)n->key.data);
        fields[f].count++; fields[f].nulls += n->value.kind == JDJ_NULL;
        fields[f].kinds |= 1u << (native ? (unsigned)native->kind : (unsigned)n->value.kind);
        if (fields[f].nsamples < 3) {
            size_t at = fields[f].nsamples++;
            fields[f].samples[at] = n->value;
            if (native) fields[f].native_samples[at] = *native;
        }
    }
    row_t* rows = calloc(count ? count : 1, sizeof(*rows)); if (!rows) goto done;
    for (size_t i = 0; i < count; i++) {
        jdj_node_t* n = reserve(d, 5 * sizeof(*n)); jdj_node_t* samples = reserve(d, 3 * sizeof(*samples));
        if (!n || !samples) { free(rows); goto done; }
        const char* type = "mixed";
        unsigned max_kind = opts->data_input ? JDV_RECORD : JDJ_OBJECT;
        for (unsigned k = 0; k <= max_kind; k++) if (fields[i].kinds == (1u << k))
            type = opts->data_input ? native_kind_name((jdv_kind_t)k) : kind_name((jdj_kind_t)k);
        n[0].key = literal("field"); n[0].value = (jdj_value_t){.kind = JDJ_STRING, .scalar = fields[i].key};
        n[1].key = literal("type"); n[1].value = (jdj_value_t){.kind = JDJ_STRING, .scalar = literal(type)};
        n[2].key = literal("count"); n[3].key = literal("null_count");
        if (unsigned_value(d, fields[i].count, &n[2].value) || unsigned_value(d, fields[i].nulls, &n[3].value)) { free(rows); goto done; }
        n[4].key = literal("samples"); n[4].value.kind = JDJ_ARRAY; n[4].value.count = fields[i].nsamples;
        n[4].value.children = fields[i].nsamples ? samples : NULL;
        for (size_t j = 0; j < fields[i].nsamples; j++) { samples[j].value = fields[i].samples[j]; if (j + 1 < fields[i].nsamples) samples[j].next = &samples[j + 1]; }
        for (size_t j = 0; j < 4; j++) n[j].next = &n[j + 1];
        rows[i].json = (jdj_value_t){.kind = JDJ_OBJECT, .children = n, .count = 5};
        if (opts->data_input && opts->data_output) {
            jdv_record_input_t native[5] = {
                {.key = literal("field"), .value = {.kind = JDV_STRING, .as.slice = fields[i].key}},
                {.key = literal("type"), .value = {.kind = JDV_STRING, .as.slice = literal(type)}},
                {.key = literal("count"), .value = jdv_value_u64(fields[i].count)},
                {.key = literal("null_count"), .value = jdv_value_u64(fields[i].nulls)},
                {.key = literal("samples")}
            };
            if (jdv_value_list_limited(&d->arena, fields[i].native_samples, fields[i].nsamples,
                    &native[4].value, &d->limits) ||
                jdv_value_record_limited(&d->arena, native, 5, &rows[i].native, &d->limits)) {
                free(rows); goto done;
            }
            rows[i].has_native = 1;
        }
    }
    free(d->rows); d->rows = rows; d->count = count; d->capacity = count; d->declared = d->guaranteed = NULL; rc = 0;
 done:
    free(fields); return rc;
}

typedef struct { jash_writer_t writer; void* context; } output_sink_t;
static long output_sink(void* context, const void* bytes, size_t size) { output_sink_t* sink = context; return sink->writer(sink->context, bytes, size); }
static int emit(dataset_t* d, int native, const char* command, jash_writer_t w, void* ctx) {
    if (!native) {
        jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, PIPE_OUTPUT_MAX);
        jash_json_writer_t writer; jash_jsonw_init(&writer, jash_structured_buffer_sink, &buffer);
        int failed = 0;
        for (size_t i = 0; i < d->count; i++) {
            if (jdj_write(&writer, &d->rows[i].json, &d->limits)) { failed = 1; break; }
            jash_jsonw_raw(&writer, "\n");
        }
        if (jash_jsonw_finish(&writer)) failed = 1;
        int rc = failed ? -1 : jash_structured_document(w, ctx, command, buffer.data ? buffer.data : "",
            buffer.used, buffer.required, jash_budget_limit(), 0);
        int saved = errno; jash_structured_buffer_free(&buffer); errno = saved; return rc;
    }
    /* Validate every conversion before the first output byte. Decimal values
     * remain usable in JSON, but cannot masquerade as a JDV integer/string. */
    for (size_t i = 0; i < d->count; i++) if (!d->rows[i].has_native && jdj_to_value(&d->arena, &d->rows[i].json, &d->limits, &d->rows[i].native)) return -1;
    jdv_schema_t any; if (jdv_schema_any(&any)) return -1;
    const jdv_schema_t* declared = d->declared ? d->declared : &any;
    const jdv_schema_t* guaranteed = d->guaranteed ? d->guaranteed : &any;
    jdv_stream_config_t config; jdv_stream_config_init(&config);
    config.declared_schema = declared; config.guaranteed_schema = guaranteed; config.value_limits = d->limits;
    config.boundedness = JDV_STREAM_FINITE; config.stream_limits.max_items = PIPE_ROW_MAX;
    config.stream_limits.max_physical_bytes = config.stream_limits.max_materialized_bytes = PIPE_OUTPUT_MAX;
    jdv_value_t* items = calloc(d->count ? d->count : 1, sizeof(*items));
    uint64_t* credits = calloc(d->count ? d->count : 1, sizeof(*credits));
    void* workspace = malloc(2u * 1024u * 1024u);
    if (!items || !credits || !workspace) { free(items); free(credits); free(workspace); return -1; }
    uint64_t total = 0;
    for (size_t i = 0; i < d->count; i++) {
        items[i] = d->rows[i].native; credits[i] = d->rows[i].credit;
        if (credits[i] > UINT64_MAX - total) { free(items); free(credits); free(workspace); return bad(EOVERFLOW); }
        total += credits[i];
    }
    /* Imported logical credits describe the producer's accounting, not the
     * byte size of this buffered wire. Never truncate or recalculate them. */
    config.stream_limits.max_logical_bytes = total;
    jash_structured_output_t output;
    if (jash_structured_begin(&output, w, ctx)) { free(items); free(credits); free(workspace); return 125; }
    output_sink_t sink = {output.json.sink, output.json.context}; jdv_stream_end_t terminal;
    jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
    outcome.state = JDV_STREAM_COMPLETE; outcome.completeness = JDV_COMPLETENESS_COMPLETE;
    int rc = jdv_emit_sequence(items, d->count, credits, &config, &outcome,
                               output_sink, &sink, workspace, 2u * 1024u * 1024u, &terminal);
    int saved = errno; free(items); free(credits); free(workspace);
    rc = jash_structured_finish(&output, rc ? 125 : 0); errno = saved; return rc;
}
static int run(int op, jash_writer_t w, void* ctx, const char* args) {
    dataset_t data = {0}; options_t opts = {0}; int rc = 0;
    if (init(&data) || options(&data, op, args, &opts)) rc = -1;
    else if (read_rows(&data, opts.data_input)) rc = -1;
    else if (op == OP_WHERE || op == OP_PICK || op == OP_SORT) rc = filter_rows(&data, op, &opts);
    else if (op == OP_DESCRIBE) rc = describe(&data, &opts);
    else rc = aggregate(&data, op, &opts);
    if (!rc) rc = emit(&data, opts.data_output, NAMES[op], w, ctx);
    int error = errno; dispose(&data);
    if (rc) {
        jash_writef(jash_stderr_writer, NULL, "%s: structured operation failed (%s); input must be valid bounded records and typed input must be complete JTV2\n", NAMES[op], strerror(error ? error : EINVAL));
        return rc > 0 ? rc : error == EPIPE || error == ENOSPC ? 125 : 2;
    }
    return 0;
}
#define ENTRY(name, op) int name(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) { (void)s; return run(op, w, ctx, args); }
ENTRY(jash_b_where, OP_WHERE)
ENTRY(jash_b_pick, OP_PICK)
ENTRY(jash_b_sort, OP_SORT)
ENTRY(jash_b_count, OP_COUNT)
ENTRY(jash_b_sum, OP_SUM)
ENTRY(jash_b_avg, OP_AVG)
ENTRY(jash_b_uniq, OP_UNIQ)
ENTRY(jash_b_describe, OP_DESCRIBE)
