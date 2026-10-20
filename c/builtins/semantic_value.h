// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Small caller-owned construction helpers; codecs and validation stay in JDV. */
#ifndef JASH_BUILTINS_SEMANTIC_VALUE_H
#define JASH_BUILTINS_SEMANTIC_VALUE_H

#include "value_output.h"
#include <math.h>

typedef struct {
    jdv_arena_t arena;
    void* storage;
    jdv_limits_t limits;
    int error;
} semantic_value_t;

static inline int semantic_value_init(semantic_value_t* s, size_t arena_bytes, size_t value_bytes) {
    memset(s, 0, sizeof(*s));
    s->storage = malloc(arena_bytes);
    if (!s->storage) return -1;
    jdv_arena_init(&s->arena, s->storage, arena_bytes);
    s->limits = JDV_DEFAULT_LIMITS;
    s->limits.max_depth = 64; s->limits.max_nodes = 65536;
    s->limits.max_list_items = 65536; s->limits.max_record_fields = 4096;
    s->limits.max_schema_fields = 4096;
    s->limits.max_aggregate_bytes = value_bytes;
    s->limits.max_encoded_bytes = value_bytes;
    return 0;
}
static inline void semantic_value_free(semantic_value_t* s) { free(s->storage); s->storage = NULL; }
static inline jdv_record_input_t semantic_field(const char* name, jdv_value_t value) {
    return (jdv_record_input_t){.key = {(const uint8_t*)name, strlen(name)}, .value = value};
}
static inline jdv_value_t semantic_record(semantic_value_t* s, const jdv_record_input_t* fields, size_t count) {
    jdv_value_t result = jdv_value_nothing();
    if (s->error || jdv_value_record_limited(&s->arena, fields, count, &result, &s->limits)) s->error = 1;
    return result;
}
static inline jdv_value_t semantic_list(semantic_value_t* s, const jdv_value_t* values, size_t count) {
    jdv_value_t result = jdv_value_nothing();
    if (s->error || jdv_value_list_limited(&s->arena, values, count, &result, &s->limits)) s->error = 1;
    return result;
}
static inline jdv_value_t semantic_bytes(semantic_value_t* s, const void* bytes, size_t count) {
    jdv_value_t result = jdv_value_nothing();
    if (s->error || jdv_value_bytes_limited(&s->arena, bytes, count, &result, &s->limits)) s->error = 1;
    return result;
}
static inline jdv_value_t semantic_text_n(semantic_value_t* s, const void* text, size_t count) {
    if (!jash_json_utf8_valid_n(text, count)) return semantic_bytes(s, text, count);
    jdv_value_t result = jdv_value_nothing();
    if (s->error || jdv_value_string_limited(&s->arena, text, count, &result, &s->limits)) s->error = 1;
    return result;
}
static inline jdv_value_t semantic_text(semantic_value_t* s, const char* text) {
    return text ? semantic_text_n(s, text, strlen(text)) : jdv_value_nothing();
}
static inline jdv_value_t semantic_path(semantic_value_t* s, const char* text) {
    jdv_value_t result = jdv_value_nothing();
    if (!text) return result;
    if (s->error || jdv_value_path_limited(&s->arena, text, strlen(text), &result, &s->limits)) s->error = 1;
    return result;
}
static inline jdv_value_t semantic_symbol(semantic_value_t* s, const char* text) {
    jdv_value_t result = jdv_value_nothing();
    if (!text) return result;
    if (s->error || jdv_value_symbol_limited(&s->arena, text, strlen(text), &result, &s->limits)) s->error = 1;
    return result;
}
/* JDV1 has no float kind. Preserve the exact finite binary64 value in an
 * explicit versioned record; never smuggle an approximation into a string. */
static inline jdv_value_t semantic_binary64(semantic_value_t* s, double number) {
    if (!isfinite(number)) { s->error = 1; return jdv_value_nothing(); }
    int exponent = 0;
    double fraction = frexp(number, &exponent);
    int64_t significand = (int64_t)ldexp(fraction, 53);
    exponent -= 53;
    if (!significand) exponent = 0;
    while (significand && !(significand % 2)) { significand /= 2; exponent++; }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(s, "jash.number.binary64.v1")),
        semantic_field("significand", jdv_value_i64(significand)),
        semantic_field("exponent2", jdv_value_i64(exponent)),
        semantic_field("negative_zero", jdv_value_bool(number == 0 && signbit(number)))
    };
    return semantic_record(s, fields, sizeof(fields) / sizeof(fields[0]));
}

#endif
