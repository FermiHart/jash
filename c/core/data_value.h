// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Bounded semantic values and canonical JDV1/JSC1 wire formats.
#ifndef JASH_CORE_DATA_VALUE_H
#define JASH_CORE_DATA_VALUE_H

#include <stddef.h>
#include <stdint.h>

#define JDV_HASH_SIZE 32u
#define JDV_MAX_DEPTH 64u
#define JDV_METADATA_VERSION 1u
#define JDV_STREAM_END_VERSION 1u
#define JDV_LITERAL(value) ((jdv_slice_t){(const uint8_t*)(value), sizeof(value) - 1u})

/* Maxima are inclusive totals for one operation; the root is depth 0 and one
 * node. Aggregate bytes include all owned slices and field names. */
typedef struct {
    size_t max_depth;
    size_t max_aggregate_bytes;
    size_t max_nodes;
    size_t max_list_items;
    size_t max_record_fields;
    size_t max_schema_fields;
    uint64_t max_encoded_bytes;
} jdv_limits_t;

extern const jdv_limits_t JDV_DEFAULT_LIMITS;

typedef struct {
    uint8_t* data;
    size_t capacity;
    size_t used;
} jdv_arena_t;

typedef struct {
    const uint8_t* data;
    size_t len;
} jdv_slice_t;

typedef enum {
    JDV_NOTHING = 0,
    JDV_BOOL = 1,
    JDV_I64 = 2,
    JDV_U64 = 3,
    JDV_FILESIZE = 4,
    JDV_TIMESTAMP = 5,
    JDV_DURATION = 6,
    JDV_STRING = 7,
    JDV_BYTES = 8,
    JDV_PATH = 9,
    JDV_SYMBOL = 10,
    JDV_LIST = 11,
    JDV_RECORD = 12
} jdv_kind_t;

typedef struct jdv_value jdv_value_t;
typedef struct jdv_record_field jdv_record_field_t;

struct jdv_value {
    jdv_kind_t kind;
    /* Metadata is deliberately outside canonical value identity. */
    uint64_t provenance_id;
    union {
        int boolean;
        int64_t i64;
        uint64_t u64;
        jdv_slice_t slice;
        struct { const jdv_value_t* items; size_t len; } list;
        struct { const jdv_record_field_t* fields; size_t len; } record;
    } as;
};

struct jdv_record_field {
    jdv_slice_t key;
    jdv_value_t value;
};

typedef jdv_record_field_t jdv_record_input_t;

void jdv_arena_init(jdv_arena_t* arena, void* storage, size_t capacity);
/* Failing int-returning constructors leave both the arena and output unchanged. */
jdv_value_t jdv_value_nothing(void);
jdv_value_t jdv_value_bool(int value);
jdv_value_t jdv_value_i64(int64_t value);
jdv_value_t jdv_value_u64(uint64_t value);
jdv_value_t jdv_value_filesize(uint64_t bytes);
jdv_value_t jdv_value_timestamp(int64_t nanoseconds);
jdv_value_t jdv_value_duration(int64_t nanoseconds);
int jdv_value_string(jdv_arena_t*, const void*, size_t, jdv_value_t*);
int jdv_value_bytes(jdv_arena_t*, const void*, size_t, jdv_value_t*);
/* Paths are reversible bytes, not implicitly UTF-8, but can never contain NUL. */
int jdv_value_path(jdv_arena_t*, const void*, size_t, jdv_value_t*);
int jdv_value_symbol(jdv_arena_t*, const void*, size_t, jdv_value_t*);
int jdv_value_list(jdv_arena_t*, const jdv_value_t*, size_t, jdv_value_t*);
int jdv_value_record(jdv_arena_t*, const jdv_record_input_t*, size_t, jdv_value_t*);
int jdv_value_string_limited(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                             const jdv_limits_t*);
int jdv_value_bytes_limited(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                            const jdv_limits_t*);
int jdv_value_path_limited(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                           const jdv_limits_t*);
int jdv_value_symbol_limited(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                             const jdv_limits_t*);
int jdv_value_list_limited(jdv_arena_t*, const jdv_value_t*, size_t,
                           jdv_value_t*, const jdv_limits_t*);
int jdv_value_record_limited(jdv_arena_t*, const jdv_record_input_t*, size_t,
                             jdv_value_t*, const jdv_limits_t*);

typedef struct jdv_sequence jdv_sequence_t;
typedef struct jdv_byte_stream jdv_byte_stream_t;

typedef enum {
    JDV_DATA_EMPTY,
    JDV_DATA_SCALAR,
    JDV_DATA_SEQUENCE,
    JDV_DATA_BYTE_STREAM
} jdv_cardinality_t;

typedef enum {
    JDV_OWNERSHIP_IMMUTABLE_VALUE,
    JDV_OWNERSHIP_LINEAR_STREAM,
    JDV_OWNERSHIP_REPLAYABLE_STREAM,
    JDV_OWNERSHIP_SEEKABLE_ARTIFACT,
    JDV_OWNERSHIP_LEASED_REFERENCE,
    JDV_OWNERSHIP_DURABLE_REFERENCE
} jdv_ownership_t;

typedef struct {
    uint32_t metadata_version;
    uint32_t struct_size;
    jdv_cardinality_t cardinality;
    jdv_ownership_t ownership;
    uint64_t provenance_id;
    union {
        jdv_value_t scalar;
        const jdv_sequence_t* sequence;
        const jdv_byte_stream_t* byte_stream;
    } as;
} jdv_data_t;

jdv_data_t jdv_data_empty(void);
jdv_data_t jdv_data_scalar(jdv_value_t value);
jdv_data_t jdv_data_sequence(const jdv_sequence_t* sequence);
jdv_data_t jdv_data_byte_stream(const jdv_byte_stream_t* stream);
int jdv_data_validate(const jdv_data_t*, const jdv_limits_t*);

typedef enum {
    JDV_STREAM_COMPLETE,
    JDV_STREAM_CANCELLED,
    JDV_STREAM_FAILED,
    JDV_STREAM_INCOMPLETE
} jdv_stream_state_t;

typedef enum {
    JDV_COMPLETENESS_UNKNOWN,
    JDV_COMPLETENESS_COMPLETE,
    JDV_COMPLETENESS_PARTIAL
} jdv_completeness_t;

typedef enum {
    JDV_HASH_NONE,
    JDV_HASH_BLAKE3
} jdv_hash_algorithm_t;

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    jdv_stream_state_t state;
    uint64_t items_seen;
    uint64_t bytes_seen;
    uint64_t errors_seen;
    int producer_status;
    int transform_status;
    int transport_status;
    jdv_slice_t reason;
    const struct jdv_schema* final_schema;
    uint8_t final_hash[JDV_HASH_SIZE];
    int final_hash_present;
    jdv_hash_algorithm_t final_hash_algorithm;
    jdv_completeness_t completeness;
    int truncated;
} jdv_stream_end_t;

int jdv_stream_end_init(jdv_stream_end_t*);
int jdv_stream_end_validate(const jdv_stream_end_t*, const jdv_limits_t*);

typedef enum {
    JDV_SCHEMA_UNKNOWN = 0,
    JDV_SCHEMA_SCALAR = 1,
    JDV_SCHEMA_LIST = 2,
    JDV_SCHEMA_RECORD = 3,
    JDV_SCHEMA_ANY = 4,
    JDV_SCHEMA_INVALID = 5
} jdv_schema_kind_t;

typedef struct jdv_schema jdv_schema_t;
typedef struct jdv_schema_field jdv_schema_field_t;

struct jdv_schema {
    jdv_schema_kind_t kind;
    int nullable;
    union {
        jdv_kind_t scalar;
        const jdv_schema_t* element;
        struct { const jdv_schema_field_t* fields; size_t len; int open; } record;
    } as;
};

struct jdv_schema_field {
    jdv_slice_t name;
    const jdv_schema_t* schema;
    int optional;
};

typedef jdv_schema_field_t jdv_schema_field_input_t;

typedef enum {
    JDV_RELATION_NO = 0,
    JDV_RELATION_YES = 1,
    JDV_RELATION_RUNTIME = 2
} jdv_relation_t;

int jdv_schema_scalar(jdv_kind_t kind, int nullable, jdv_schema_t* out);
int jdv_schema_unknown(jdv_schema_t* out);
int jdv_schema_any(jdv_schema_t* out);
int jdv_schema_invalid(jdv_schema_t* out);
int jdv_schema_list(jdv_arena_t*, const jdv_schema_t* element, int nullable,
                    jdv_schema_t* out);
int jdv_schema_record(jdv_arena_t*, const jdv_schema_field_input_t*, size_t,
                      int open, int nullable, jdv_schema_t* out);
int jdv_schema_scalar_limited(jdv_kind_t, int, jdv_schema_t*,
                              const jdv_limits_t*);
int jdv_schema_unknown_limited(jdv_schema_t*, const jdv_limits_t*);
int jdv_schema_any_limited(jdv_schema_t*, const jdv_limits_t*);
int jdv_schema_invalid_limited(jdv_schema_t*, const jdv_limits_t*);
int jdv_schema_list_limited(jdv_arena_t*, const jdv_schema_t*, int,
                            jdv_schema_t*, const jdv_limits_t*);
int jdv_schema_record_limited(jdv_arena_t*, const jdv_schema_field_input_t*,
                              size_t, int, int, jdv_schema_t*,
                              const jdv_limits_t*);
int jdv_schema_accepts_checked(const jdv_schema_t*, const jdv_value_t*,
                               jdv_relation_t* result,
                               const jdv_limits_t*);
int jdv_schema_accepts(const jdv_schema_t*, const jdv_value_t*);
int jdv_schema_relation_checked(const jdv_schema_t* source,
                                const jdv_schema_t* destination,
                                jdv_relation_t* result,
                                const jdv_limits_t*);
jdv_relation_t jdv_schema_relation(const jdv_schema_t* source,
                                   const jdv_schema_t* destination);

typedef enum {
    JDV_VALIDATION_NONE,
    JDV_VALIDATION_SAMPLED,
    JDV_VALIDATION_PREFIX,
    JDV_VALIDATION_COMPLETE,
    JDV_VALIDATION_FAILED
} jdv_validation_scope_t;

typedef struct {
    const jdv_schema_t* declared_schema;
    const jdv_schema_t* guaranteed_schema;
    const jdv_schema_t* observed_schema;
    const jdv_schema_t* final_schema;
    jdv_validation_scope_t validation_scope;
} jdv_schema_set_t;

int jdv_schema_set_init(jdv_schema_set_t*, const jdv_schema_t* declared,
                        const jdv_schema_t* guaranteed,
                        const jdv_limits_t*);
int jdv_schema_set_transition(jdv_schema_set_t*,
                              const jdv_schema_t* observed,
                              const jdv_schema_t* final_schema,
                              jdv_validation_scope_t,
                              const jdv_limits_t*);

typedef long (*jdv_sink_t)(void* context, const void* data, size_t size);

typedef struct {
    jdv_sink_t sink;
    void* context;
    uint64_t bytes_written;
    int error;
} jdv_writer_t;

void jdv_writer_init(jdv_writer_t*, jdv_sink_t, void* context);
/* Short writes are completed, EINTR is retried, and the first error is sticky. */
int jdv_writer_write(jdv_writer_t*, const void*, size_t);
int jdv_writer_finish(jdv_writer_t*);

int jdv_encode(const jdv_value_t*, jdv_writer_t*);
int jdv_encode_limited(const jdv_value_t*, jdv_writer_t*, const jdv_limits_t*);
/* Exact decoders consume one complete frame and reject every trailing byte.
 * On failure, arena, output and consumed are all unchanged. */
int jdv_decode_exact(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                     size_t* consumed);
int jdv_decode_exact_limited(jdv_arena_t*, const void*, size_t, jdv_value_t*,
                             size_t* consumed, const jdv_limits_t*);
/* Compatibility alias for jdv_decode_exact(). */
int jdv_decode(jdv_arena_t*, const void*, size_t, jdv_value_t*, size_t* consumed);
int jdv_hash(const jdv_value_t*, uint8_t out[JDV_HASH_SIZE]);
int jdv_hash_limited(const jdv_value_t*, uint8_t out[JDV_HASH_SIZE],
                     const jdv_limits_t*);
int jdv_schema_encode(const jdv_schema_t*, jdv_writer_t*);
int jdv_schema_encode_limited(const jdv_schema_t*, jdv_writer_t*,
                              const jdv_limits_t*);
int jdv_schema_decode_exact(jdv_arena_t*, const void*, size_t, jdv_schema_t*,
                            size_t* consumed);
int jdv_schema_decode_exact_limited(jdv_arena_t*, const void*, size_t,
                                    jdv_schema_t*, size_t* consumed,
                                    const jdv_limits_t*);
/* Compatibility alias for jdv_schema_decode_exact(). */
int jdv_schema_decode(jdv_arena_t*, const void*, size_t, jdv_schema_t*,
                      size_t* consumed);

#endif
