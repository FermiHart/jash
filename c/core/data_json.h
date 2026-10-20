// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DATA_JSON_H
#define JASH_CORE_DATA_JSON_H

#include "data_value.h"
#include "json_writer.h"

/* Strict JSON syntax tree. Numbers retain their exact decimal lexeme; strings
 * have a length and may contain NUL. The caller owns the arena. All reachable
 * storage remains valid until it resets that arena. Input need not outlive parse.
 * Objects reject duplicate decoded keys. Parsing is failure-atomic. The input
 * buffer must not overlap the arena's unused storage. Limits are cumulative
 * across a whole value, including nested fields/items and encoded JSON bytes. */
typedef enum { JDJ_NULL, JDJ_BOOL, JDJ_NUMBER, JDJ_STRING, JDJ_ARRAY, JDJ_OBJECT } jdj_kind_t;
typedef struct jdj_node jdj_node_t;
typedef struct {
    jdj_kind_t kind;
    int boolean;
    jdv_slice_t scalar;
    const jdj_node_t* children;
    size_t count;
} jdj_value_t;
struct jdj_node {
    jdv_slice_t key;
    jdj_value_t value;
    const jdj_node_t* next;
};

int jdj_parse(jdv_arena_t*, const void*, size_t, const jdv_limits_t*, jdj_value_t*);
/* Validate an externally constructed AST before conversion or output. Invalid
 * UTF-8, number syntax, duplicate keys, count mismatch, cycles and budget excess
 * are rejected. Write validates the whole AST before touching the writer. */
int jdj_validate(const jdj_value_t*, const jdv_limits_t*);
int jdj_write(jash_json_writer_t*, const jdj_value_t*, const jdv_limits_t*);
const jdj_value_t* jdj_get(const jdj_value_t*, const void*, size_t);
/* Convert JSON-native types. Decimal/exponent lexemes are deliberately ENOTSUP
 * at the JDV boundary: JDV1 has no decimal type. Exact integer range is I64/U64.
 * JSON keys containing NUL cannot be JDV record keys and are refused.
 * Both conversions leave arena usage and output unchanged on failure. */
int jdj_to_value(jdv_arena_t*, const jdj_value_t*, const jdv_limits_t*, jdv_value_t*);
int jdj_from_value(jdv_arena_t*, const jdv_value_t*, const jdv_limits_t*, jdj_value_t*);
/* JSON projection of native types: bytes use {encoding:base64,data:...}; paths
 * use UTF-8 when valid, otherwise that byte tag. Symbols become strings and
 * units become exact integers. This projection is not a native-type roundtrip;
 * JTV2 remains the lossless transport. No tag-shaped user object is inferred. */
int jdv_json_parse(jdv_arena_t*, const void*, size_t, const jdv_limits_t*, jdv_value_t*);
int jdv_json_write(jash_json_writer_t*, const jdv_value_t*, const jdv_limits_t*);

#endif
