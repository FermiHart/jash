// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/data_value.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct {
    uint8_t bytes[8192];
    size_t used;
    size_t max_write;
    int interrupt_once;
    int failure;
    int return_zero;
    int calls;
} test_sink_t;

static long test_sink(void* context, const void* data, size_t size) {
    test_sink_t* sink = context;
    sink->calls++;
    if (sink->interrupt_once) {
        sink->interrupt_once = 0;
        errno = EINTR;
        return -1;
    }
    if (sink->failure) {
        errno = sink->failure;
        return -1;
    }
    if (sink->return_zero) return 0;
    if (sink->max_write && size > sink->max_write) size = sink->max_write;
    if (size > sizeof(sink->bytes) - sink->used) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(sink->bytes + sink->used, data, size);
    sink->used += size;
    return (long)size;
}

typedef struct {
    test_sink_t output;
    jdv_value_t* value;
    jdv_schema_t* schema;
    int mutated;
} mutating_sink_t;

static long mutating_sink(void* context, const void* data, size_t size) {
    mutating_sink_t* sink = context;
    if (!sink->mutated) {
        sink->mutated = 1;
        if (sink->value) sink->value->as.i64 = 999;
        if (sink->schema) sink->schema->as.scalar = JDV_BOOL;
    }
    return test_sink(&sink->output, data, size);
}

static int same_slice(jdv_slice_t value, const void* data, size_t size) {
    return value.len == size && (!size || memcmp(value.data, data, size) == 0);
}

static int test_values_and_data(void) {
    uint8_t storage[4096], source_storage[512];
    jdv_arena_t arena, source_arena;
    jdv_value_t string, bytes, path, symbol, list, record;
    const uint8_t text[] = {'a', 0, 0xe2, 0x82, 0xac};
    const uint8_t binary[] = {0, 0xff, 1, 0x80};
    const uint8_t raw_path[] = {'x', '/', 0xff};
    const uint8_t bad_utf8[] = {0xc0, 0x80};
    const uint8_t bad_path[] = {'a', 0, 'b'};

    jdv_arena_init(&arena, storage, sizeof(storage));
    jdv_arena_init(&source_arena, source_storage, sizeof(source_storage));
    CHECK(jdv_value_string(&arena, text, sizeof(text), &string) == 0);
    CHECK(string.kind == JDV_STRING && same_slice(string.as.slice, text, sizeof(text)));
    CHECK(jdv_value_string(&arena, bad_utf8, sizeof(bad_utf8), &path) < 0 && errno == EILSEQ);
    CHECK(jdv_value_bytes(&arena, binary, sizeof(binary), &bytes) == 0);
    CHECK(bytes.kind == JDV_BYTES && same_slice(bytes.as.slice, binary, sizeof(binary)));
    CHECK(jdv_value_path(&arena, raw_path, sizeof(raw_path), &path) == 0);
    CHECK(path.kind == JDV_PATH && same_slice(path.as.slice, raw_path, sizeof(raw_path)));
    CHECK(jdv_value_path(&arena, bad_path, sizeof(bad_path), &path) < 0 && errno == EINVAL);
    CHECK(jdv_value_symbol(&arena, "ready", 5, &symbol) == 0);

    jdv_value_t source;
    CHECK(jdv_value_string(&source_arena, "original", 8, &source) == 0);
    jdv_value_t input_items[] = {source, jdv_value_i64(-7), jdv_value_bool(1)};
    CHECK(jdv_value_list(&arena, input_items, 3, &list) == 0);
    memset(source_storage, 'X', sizeof(source_storage));
    input_items[1] = jdv_value_i64(99);
    CHECK(list.kind == JDV_LIST && list.as.list.len == 3);
    CHECK(same_slice(list.as.list.items[0].as.slice, "original", 8));
    CHECK(list.as.list.items[1].as.i64 == -7);

    jdv_record_input_t fields[] = {
        {JDV_LITERAL("z"), jdv_value_filesize(UINT64_MAX)},
        {JDV_LITERAL("a"), list},
    };
    CHECK(jdv_value_record(&arena, fields, 2, &record) == 0);
    fields[0].value = jdv_value_u64(0);
    CHECK(record.kind == JDV_RECORD && record.as.record.len == 2);
    CHECK(same_slice(record.as.record.fields[0].key, "a", 1));
    CHECK(same_slice(record.as.record.fields[1].key, "z", 1));
    CHECK(record.as.record.fields[1].value.kind == JDV_FILESIZE);
    jdv_record_input_t duplicate[] = {
        {JDV_LITERAL("same"), jdv_value_nothing()},
        {JDV_LITERAL("same"), jdv_value_bool(0)},
    };
    CHECK(jdv_value_record(&arena, duplicate, 2, &path) < 0 && errno == EEXIST);

    CHECK(jdv_value_nothing().kind == JDV_NOTHING);
    CHECK(jdv_value_bool(0).kind == JDV_BOOL);
    CHECK(jdv_value_i64(INT64_MIN).as.i64 == INT64_MIN);
    CHECK(jdv_value_u64(UINT64_MAX).as.u64 == UINT64_MAX);
    CHECK(jdv_value_filesize(3).kind == JDV_FILESIZE);
    CHECK(jdv_value_timestamp(-3).kind == JDV_TIMESTAMP);
    CHECK(jdv_value_duration(4).kind == JDV_DURATION);
    CHECK(symbol.kind == JDV_SYMBOL);

    jdv_data_t data = jdv_data_empty();
    CHECK(data.metadata_version == JDV_METADATA_VERSION);
    CHECK(data.struct_size == sizeof(data));
    CHECK(data.cardinality == JDV_DATA_EMPTY);
    CHECK(jdv_data_validate(&data, &JDV_DEFAULT_LIMITS) == 0);
    data = jdv_data_scalar(record);
    CHECK(data.cardinality == JDV_DATA_SCALAR && data.as.scalar.kind == JDV_RECORD);
    CHECK(jdv_data_validate(&data, &JDV_DEFAULT_LIMITS) == 0);
    data = jdv_data_sequence((const jdv_sequence_t*)(uintptr_t)1);
    CHECK(data.cardinality == JDV_DATA_SEQUENCE && data.as.sequence != NULL);
    data = jdv_data_byte_stream((const jdv_byte_stream_t*)(uintptr_t)2);
    CHECK(data.cardinality == JDV_DATA_BYTE_STREAM && data.as.byte_stream != NULL);
    CHECK(jdv_data_validate(&data, &JDV_DEFAULT_LIMITS) == 0);
    data.metadata_version++;
    CHECK(jdv_data_validate(&data, &JDV_DEFAULT_LIMITS) < 0 &&
          errno == EPROTONOSUPPORT);

    uint8_t tiny[3];
    jdv_arena_t small;
    jdv_arena_init(&small, tiny, sizeof(tiny));
    CHECK(jdv_value_bytes(&small, binary, sizeof(binary), &path) < 0 && errno == ENOSPC);
    CHECK(small.used == 0);
    return 0;
}

static int test_schema(void) {
    uint8_t storage[4096];
    jdv_arena_t arena;
    jdv_arena_init(&arena, storage, sizeof(storage));

    jdv_schema_t text, nullable_text, number, text_list;
    CHECK(jdv_schema_scalar(JDV_STRING, 0, &text) == 0);
    CHECK(jdv_schema_scalar(JDV_STRING, 1, &nullable_text) == 0);
    CHECK(jdv_schema_scalar(JDV_U64, 0, &number) == 0);
    CHECK(jdv_schema_list(&arena, &text, 0, &text_list) == 0);

    jdv_schema_field_input_t source_fields[] = {
        {JDV_LITERAL("name"), &text, 0},
        {JDV_LITERAL("note"), &nullable_text, 1},
    };
    jdv_schema_field_input_t required_note_fields[] = {
        {JDV_LITERAL("name"), &text, 0},
        {JDV_LITERAL("note"), &nullable_text, 0},
    };
    jdv_schema_field_input_t wrong_fields[] = {
        {JDV_LITERAL("name"), &number, 0},
    };
    jdv_schema_t source, required_note, wrong, open_name, open_source, closed_name;
    CHECK(jdv_schema_record(&arena, source_fields, 2, 0, 0, &source) == 0);
    CHECK(jdv_schema_record(&arena, required_note_fields, 2, 0, 0, &required_note) == 0);
    CHECK(jdv_schema_record(&arena, wrong_fields, 1, 0, 0, &wrong) == 0);
    CHECK(jdv_schema_record(&arena, source_fields, 1, 1, 0, &open_name) == 0);
    CHECK(jdv_schema_record(&arena, source_fields, 2, 1, 0, &open_source) == 0);
    CHECK(jdv_schema_record(&arena, source_fields, 1, 0, 0, &closed_name) == 0);

    CHECK(jdv_schema_relation(&source, &source) == JDV_RELATION_YES);
    CHECK(jdv_schema_relation(&text, &nullable_text) == JDV_RELATION_YES);
    CHECK(jdv_schema_relation(&nullable_text, &text) == JDV_RELATION_NO);
    CHECK(jdv_schema_relation(&source, &required_note) == JDV_RELATION_NO);
    CHECK(jdv_schema_relation(&source, &wrong) == JDV_RELATION_NO);
    CHECK(jdv_schema_relation(&open_name, &closed_name) == JDV_RELATION_NO);
    CHECK(jdv_schema_relation(&open_name, &source) == JDV_RELATION_NO);
    CHECK(jdv_schema_relation(&open_name, &open_source) == JDV_RELATION_RUNTIME);
    CHECK(jdv_schema_relation(&closed_name, &open_name) == JDV_RELATION_YES);
    CHECK(text_list.kind == JDV_SCHEMA_LIST);

    jdv_value_t name, null_value = jdv_value_nothing(), valid_record, invalid_record;
    CHECK(jdv_value_string(&arena, "jash", 4, &name) == 0);
    jdv_record_input_t valid[] = {
        {JDV_LITERAL("name"), name},
        {JDV_LITERAL("note"), null_value},
    };
    jdv_record_input_t invalid[] = {{JDV_LITERAL("name"), jdv_value_u64(1)}};
    CHECK(jdv_value_record(&arena, valid, 2, &valid_record) == 0);
    CHECK(jdv_value_record(&arena, invalid, 1, &invalid_record) == 0);
    CHECK(jdv_schema_accepts(&source, &valid_record));
    CHECK(!jdv_schema_accepts(&source, &invalid_record));

    jdv_schema_set_t set;
    CHECK(jdv_schema_set_init(&set, &source, &closed_name,
                              &JDV_DEFAULT_LIMITS) == 0);
    CHECK(set.declared_schema == &source && set.final_schema == NULL);
    CHECK(jdv_schema_set_transition(&set, &source, NULL,
                                    JDV_VALIDATION_PREFIX,
                                    &JDV_DEFAULT_LIMITS) == 0);
    CHECK(set.observed_schema == &source &&
          set.validation_scope == JDV_VALIDATION_PREFIX);
    return 0;
}

static int encode_value(const jdv_value_t* value, test_sink_t* sink) {
    jdv_writer_t writer;
    jdv_writer_init(&writer, test_sink, sink);
    if (jdv_encode(value, &writer) < 0) return -1;
    return jdv_writer_finish(&writer);
}

static int encode_schema(const jdv_schema_t* schema, test_sink_t* sink) {
    jdv_writer_t writer;
    jdv_writer_init(&writer, test_sink, sink);
    if (jdv_schema_encode(schema, &writer) < 0) return -1;
    return jdv_writer_finish(&writer);
}

static int test_canonical_identity_and_io(void) {
    uint8_t arena_storage[8192], decode_storage[8192];
    jdv_arena_t arena, decode_arena;
    jdv_arena_init(&arena, arena_storage, sizeof(arena_storage));
    jdv_arena_init(&decode_arena, decode_storage, sizeof(decode_storage));

    jdv_record_input_t first_fields[] = {
        {JDV_LITERAL("b"), jdv_value_i64(-1)},
        {JDV_LITERAL("a"), jdv_value_u64(UINT64_MAX)},
    };
    jdv_record_input_t second_fields[] = {
        {JDV_LITERAL("a"), jdv_value_u64(UINT64_MAX)},
        {JDV_LITERAL("b"), jdv_value_i64(-1)},
    };
    jdv_value_t first, second;
    CHECK(jdv_value_record(&arena, first_fields, 2, &first) == 0);
    CHECK(jdv_value_record(&arena, second_fields, 2, &second) == 0);
    first.provenance_id = 11;
    second.provenance_id = 99;

    test_sink_t normal = {0}, short_and_interrupted = {.max_write = 2, .interrupt_once = 1};
    CHECK(encode_value(&first, &normal) == 0);
    CHECK(normal.used > 4 && memcmp(normal.bytes, "JDV1", 4) == 0);
    CHECK(encode_value(&second, &short_and_interrupted) == 0);
    CHECK(normal.used == short_and_interrupted.used);
    CHECK(memcmp(normal.bytes, short_and_interrupted.bytes, normal.used) == 0);

    uint8_t first_hash[JDV_HASH_SIZE], second_hash[JDV_HASH_SIZE];
    CHECK(jdv_hash(&first, first_hash) == 0);
    CHECK(jdv_hash(&second, second_hash) == 0);
    CHECK(memcmp(first_hash, second_hash, sizeof(first_hash)) == 0);

    jdv_value_t decoded;
    size_t consumed = 0;
    CHECK(jdv_decode(&decode_arena, normal.bytes, normal.used, &decoded, &consumed) == 0);
    CHECK(consumed == normal.used && decoded.kind == JDV_RECORD);
    test_sink_t round_trip = {0};
    CHECK(encode_value(&decoded, &round_trip) == 0);
    CHECK(round_trip.used == normal.used && memcmp(round_trip.bytes, normal.bytes, normal.used) == 0);
    size_t decode_mark = decode_arena.used;
    CHECK(jdv_decode(&decode_arena, normal.bytes, normal.used - 1, &decoded, NULL) < 0);
    CHECK(decode_arena.used == decode_mark);
    normal.bytes[normal.used] = 0;
    CHECK(jdv_decode(&decode_arena, normal.bytes, normal.used + 1, &decoded, NULL) < 0 &&
          errno == EBADMSG);
    CHECK(decode_arena.used == decode_mark);

    uint8_t noncanonical[8192];
    CHECK(normal.used < sizeof(noncanonical));
    memcpy(noncanonical, normal.bytes, normal.used);
    /* JDV1 record fields must be strictly key-sorted; duplicate "a" is rejected. */
    CHECK(normal.used == 49 && noncanonical[21] == 'a' && noncanonical[39] == 'b');
    noncanonical[39] = 'a';
    CHECK(jdv_decode(&decode_arena, noncanonical, normal.used, &decoded, NULL) < 0 &&
          errno == EBADMSG);
    CHECK(decode_arena.used == decode_mark);

    const uint8_t nul_text[] = {'x', 0, 'y'};
    const uint8_t raw_bytes[] = {0, 0xff};
    const uint8_t raw_path[] = {'/', 0xff};
    jdv_value_t text_value, bytes_value, path_value, symbol_value, all_values;
    CHECK(jdv_value_string(&arena, nul_text, sizeof(nul_text), &text_value) == 0);
    CHECK(jdv_value_bytes(&arena, raw_bytes, sizeof(raw_bytes), &bytes_value) == 0);
    CHECK(jdv_value_path(&arena, raw_path, sizeof(raw_path), &path_value) == 0);
    CHECK(jdv_value_symbol(&arena, "sym", 3, &symbol_value) == 0);
    jdv_value_t scalar_values[] = {
        jdv_value_nothing(), jdv_value_bool(1), jdv_value_i64(INT64_MIN),
        jdv_value_u64(UINT64_MAX), jdv_value_filesize(UINT64_MAX),
        jdv_value_timestamp(INT64_MIN), jdv_value_duration(INT64_MAX),
        text_value, bytes_value, path_value, symbol_value,
    };
    CHECK(jdv_value_list(&arena, scalar_values,
                         sizeof(scalar_values) / sizeof(scalar_values[0]),
                         &all_values) == 0);
    test_sink_t all_encoded = {0};
    CHECK(encode_value(&all_values, &all_encoded) == 0);
    CHECK(jdv_decode(&decode_arena, all_encoded.bytes, all_encoded.used,
                     &decoded, &consumed) == 0);
    CHECK(decoded.kind == JDV_LIST && decoded.as.list.len == 11);
    CHECK(decoded.as.list.items[6].as.i64 == INT64_MAX);
    CHECK(same_slice(decoded.as.list.items[7].as.slice, nul_text, sizeof(nul_text)));
    CHECK(same_slice(decoded.as.list.items[8].as.slice, raw_bytes, sizeof(raw_bytes)));

    test_sink_t failed = {.failure = EPIPE};
    jdv_writer_t writer;
    jdv_writer_init(&writer, test_sink, &failed);
    CHECK(jdv_writer_write(&writer, "abc", 3) < 0 && writer.error == EPIPE);
    int calls = failed.calls;
    CHECK(jdv_writer_write(&writer, "ignored", 7) < 0 && failed.calls == calls);
    CHECK(jdv_writer_finish(&writer) < 0 && errno == EPIPE);

    test_sink_t zero = {.return_zero = 1};
    jdv_writer_init(&writer, test_sink, &zero);
    CHECK(jdv_writer_write(&writer, "abc", 3) < 0 && writer.error == EIO);
    calls = zero.calls;
    CHECK(jdv_writer_write(&writer, "ignored", 7) < 0 && zero.calls == calls);

    jdv_schema_t text, number, text_list, schema1, schema2;
    CHECK(jdv_schema_scalar(JDV_STRING, 0, &text) == 0);
    CHECK(jdv_schema_scalar(JDV_U64, 1, &number) == 0);
    CHECK(jdv_schema_list(&arena, &text, 0, &text_list) == 0);
    jdv_schema_field_input_t schema_fields1[] = {
        {JDV_LITERAL("z"), &number, 1}, {JDV_LITERAL("a"), &text_list, 0},
    };
    jdv_schema_field_input_t schema_fields2[] = {
        {JDV_LITERAL("a"), &text_list, 0}, {JDV_LITERAL("z"), &number, 1},
    };
    CHECK(jdv_schema_record(&arena, schema_fields1, 2, 0, 0, &schema1) == 0);
    CHECK(jdv_schema_record(&arena, schema_fields2, 2, 0, 0, &schema2) == 0);
    CHECK(same_slice(schema1.as.record.fields[0].name, "a", 1));
    CHECK(same_slice(schema1.as.record.fields[1].name, "z", 1));
    test_sink_t encoded_schema1 = {0}, encoded_schema2 = {0};
    CHECK(encode_schema(&schema1, &encoded_schema1) == 0);
    CHECK(encode_schema(&schema2, &encoded_schema2) == 0);
    CHECK(encoded_schema1.used > 4 && memcmp(encoded_schema1.bytes, "JSC1", 4) == 0);
    CHECK(encoded_schema1.used == encoded_schema2.used);
    CHECK(memcmp(encoded_schema1.bytes, encoded_schema2.bytes, encoded_schema1.used) == 0);
    jdv_schema_t decoded_schema;
    CHECK(jdv_schema_decode(&decode_arena, encoded_schema1.bytes, encoded_schema1.used,
                            &decoded_schema, &consumed) == 0);
    CHECK(consumed == encoded_schema1.used);
    CHECK(jdv_schema_relation(&decoded_schema, &schema1) == JDV_RELATION_YES);
    encoded_schema1.bytes[encoded_schema1.used] = 0;
    CHECK(jdv_schema_decode(&decode_arena, encoded_schema1.bytes,
                            encoded_schema1.used + 1, &decoded_schema, NULL) < 0 &&
          errno == EBADMSG);
    return 0;
}

static int test_failure_atomicity_and_limits(void) {
    uint8_t storage[8192], encoded_storage[8192];
    jdv_arena_t arena, encoded_arena;
    jdv_arena_init(&arena, storage, sizeof(storage));
    jdv_arena_init(&encoded_arena, encoded_storage, sizeof(encoded_storage));

    jdv_value_t sentinel, before;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    before = sentinel;
    size_t mark = arena.used;
    const uint8_t bad_utf8[] = {0xc0, 0x80};
    CHECK(jdv_value_string(&arena, bad_utf8, sizeof(bad_utf8), &sentinel) < 0);
    CHECK(arena.used == mark && memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_record_input_t duplicate[] = {
        {JDV_LITERAL("x"), jdv_value_u64(1)},
        {JDV_LITERAL("x"), jdv_value_u64(2)},
    };
    CHECK(jdv_value_record(&arena, duplicate, 2, &sentinel) < 0 && errno == EEXIST);
    CHECK(arena.used == mark && memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_arena_t corrupt_arena = {storage, 1, 2};
    CHECK(jdv_value_list(&corrupt_arena, NULL, 0, &sentinel) < 0 &&
          errno == EOVERFLOW);
    CHECK(corrupt_arena.used == 2 &&
          memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_schema_t text;
    CHECK(jdv_schema_scalar(JDV_STRING, 0, &text) == 0);
    jdv_schema_field_input_t duplicate_schema[] = {
        {JDV_LITERAL("x"), &text, 0},
        {JDV_LITERAL("x"), &text, 1},
    };
    jdv_schema_t schema_sentinel, schema_before;
    memset(&schema_sentinel, 0x5a, sizeof(schema_sentinel));
    schema_before = schema_sentinel;
    CHECK(jdv_schema_record(&arena, duplicate_schema, 2, 0, 0,
                            &schema_sentinel) < 0 && errno == EEXIST);
    CHECK(arena.used == mark &&
          memcmp(&schema_sentinel, &schema_before, sizeof(schema_sentinel)) == 0);

    jdv_limits_t limits = JDV_DEFAULT_LIMITS;
    limits.max_aggregate_bytes = 3;
    CHECK(jdv_value_string_limited(&arena, "four", 4, &sentinel, &limits) < 0 &&
          errno == E2BIG);
    CHECK(arena.used == mark && memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_value_t two_items[] = {jdv_value_nothing(), jdv_value_nothing()};
    limits = JDV_DEFAULT_LIMITS;
    limits.max_list_items = 1;
    CHECK(jdv_value_list_limited(&arena, two_items, 2, &sentinel, &limits) < 0 &&
          errno == E2BIG);
    CHECK(arena.used == mark && memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_record_input_t two_fields[] = {
        {JDV_LITERAL("a"), jdv_value_nothing()},
        {JDV_LITERAL("b"), jdv_value_nothing()},
    };
    limits = JDV_DEFAULT_LIMITS;
    limits.max_record_fields = 1;
    CHECK(jdv_value_record_limited(&arena, two_fields, 2, &sentinel, &limits) < 0 &&
          errno == E2BIG);

    limits = JDV_DEFAULT_LIMITS;
    limits.max_nodes = 2;
    CHECK(jdv_value_list_limited(&arena, two_items, 2, &sentinel, &limits) < 0 &&
          errno == E2BIG);

    jdv_value_t leaf = jdv_value_nothing();
    jdv_value_t inner = jdv_value_nothing();
    inner.kind = JDV_LIST;
    inner.as.list.items = &leaf;
    inner.as.list.len = 1;
    limits = JDV_DEFAULT_LIMITS;
    limits.max_depth = 2;
    CHECK(jdv_value_list_limited(&arena, &inner, 1, &sentinel, &limits) < 0 &&
          errno == E2BIG);
    CHECK(arena.used == mark && memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

    jdv_schema_field_input_t two_schema_fields[] = {
        {JDV_LITERAL("a"), &text, 0},
        {JDV_LITERAL("b"), &text, 0},
    };
    limits = JDV_DEFAULT_LIMITS;
    limits.max_schema_fields = 1;
    CHECK(jdv_schema_record_limited(&arena, two_schema_fields, 2, 0, 0,
                                    &schema_sentinel, &limits) < 0 &&
          errno == E2BIG);
    CHECK(arena.used == mark &&
          memcmp(&schema_sentinel, &schema_before, sizeof(schema_sentinel)) == 0);

    jdv_value_t string;
    CHECK(jdv_value_string(&arena, "payload", 7, &string) == 0);
    test_sink_t encoded = {0};
    CHECK(encode_value(&string, &encoded) == 0);
    jdv_value_t decoded;
    memset(&decoded, 0x3c, sizeof(decoded));
    jdv_value_t decoded_before = decoded;
    size_t consumed = 777;
    size_t decode_mark = encoded_arena.used;
    CHECK(jdv_decode_exact(&encoded_arena, encoded.bytes, encoded.used - 1,
                           &decoded, &consumed) < 0);
    CHECK(encoded_arena.used == decode_mark && consumed == 777 &&
          memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);
    encoded.bytes[encoded.used] = 0;
    CHECK(jdv_decode_exact(&encoded_arena, encoded.bytes, encoded.used + 1,
                           &decoded, &consumed) < 0 && errno == EBADMSG);
    CHECK(encoded_arena.used == decode_mark && consumed == 777 &&
          memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    limits = JDV_DEFAULT_LIMITS;
    limits.max_encoded_bytes = encoded.used - 1;
    CHECK(jdv_decode_exact_limited(&encoded_arena, encoded.bytes, encoded.used,
                                   &decoded, &consumed, &limits) < 0 &&
          errno == E2BIG);
    CHECK(encoded_arena.used == decode_mark && consumed == 777 &&
          memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    test_sink_t no_output = {0};
    jdv_writer_t writer;
    jdv_writer_init(&writer, test_sink, &no_output);
    CHECK(jdv_encode_limited(&string, &writer, &limits) < 0 && errno == E2BIG);
    CHECK(no_output.calls == 0 && no_output.used == 0);

    limits = JDV_DEFAULT_LIMITS;
    limits.max_aggregate_bytes = 6;
    CHECK(jdv_decode_exact_limited(&encoded_arena, encoded.bytes, encoded.used,
                                   &decoded, &consumed, &limits) < 0 &&
          errno == E2BIG);
    CHECK(encoded_arena.used == decode_mark && consumed == 777 &&
          memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    const uint8_t huge_list[] = {
        'J','D','V','1', JDV_LIST,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    CHECK(jdv_decode_exact(&encoded_arena, huge_list, sizeof(huge_list),
                           &decoded, &consumed) < 0 && errno == E2BIG);
    CHECK(encoded_arena.used == decode_mark && consumed == 777 &&
          memcmp(&decoded, &decoded_before, sizeof(decoded)) == 0);

    jdv_schema_t record_schema;
    CHECK(jdv_schema_record(&arena, two_schema_fields, 2, 0, 0,
                            &record_schema) == 0);
    test_sink_t encoded_schema = {0};
    CHECK(encode_schema(&record_schema, &encoded_schema) == 0);
    jdv_schema_t decoded_schema;
    memset(&decoded_schema, 0xc3, sizeof(decoded_schema));
    jdv_schema_t decoded_schema_before = decoded_schema;
    consumed = 313;
    decode_mark = encoded_arena.used;
    limits = JDV_DEFAULT_LIMITS;
    limits.max_schema_fields = 1;
    CHECK(jdv_schema_decode_exact_limited(
              &encoded_arena, encoded_schema.bytes, encoded_schema.used,
              &decoded_schema, &consumed, &limits) < 0 && errno == E2BIG);
    CHECK(encoded_arena.used == decode_mark && consumed == 313 &&
          memcmp(&decoded_schema, &decoded_schema_before,
                 sizeof(decoded_schema)) == 0);
    CHECK(jdv_schema_decode_exact(&encoded_arena, encoded_schema.bytes,
                                  encoded_schema.used - 1, &decoded_schema,
                                  &consumed) < 0);
    CHECK(encoded_arena.used == decode_mark && consumed == 313 &&
          memcmp(&decoded_schema, &decoded_schema_before,
                 sizeof(decoded_schema)) == 0);
    return 0;
}

static int test_signed_golden_vectors(void) {
    static const struct {
        uint8_t tag;
        uint8_t bytes[8];
        int64_t value;
    } vectors[] = {
        {JDV_I64, {0,0,0,0,0,0,0,0}, 0},
        {JDV_I64, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}, -1},
        {JDV_TIMESTAMP, {0x80,0,0,0,0,0,0,0}, INT64_MIN},
        {JDV_DURATION, {0x7f,0xff,0xff,0xff,0xff,0xff,0xff,0xff}, INT64_MAX},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t frame[13] = {'J','D','V','1', 0, 0,0,0,0,0,0,0,0};
        frame[4] = vectors[i].tag;
        memcpy(frame + 5, vectors[i].bytes, 8);
        uint8_t storage[128];
        jdv_arena_t arena;
        jdv_arena_init(&arena, storage, sizeof(storage));
        jdv_value_t decoded;
        size_t consumed = 0;
        CHECK(jdv_decode_exact(&arena, frame, sizeof(frame), &decoded,
                               &consumed) == 0);
        CHECK(consumed == sizeof(frame) && decoded.kind == vectors[i].tag &&
              decoded.as.i64 == vectors[i].value);

        test_sink_t encoded = {0};
        CHECK(encode_value(&decoded, &encoded) == 0);
        CHECK(encoded.used == sizeof(frame) &&
              memcmp(encoded.bytes, frame, sizeof(frame)) == 0);
    }
    return 0;
}

static int test_schema_states_and_contracts(void) {
    uint8_t storage[4096];
    jdv_arena_t arena;
    jdv_arena_init(&arena, storage, sizeof(storage));
    jdv_schema_t unknown, any, invalid, text;
    CHECK(jdv_schema_unknown(&unknown) == 0);
    CHECK(jdv_schema_any(&any) == 0);
    CHECK(jdv_schema_invalid(&invalid) == 0);
    CHECK(jdv_schema_scalar(JDV_STRING, 0, &text) == 0);

    jdv_value_t value;
    CHECK(jdv_value_string(&arena, "x", 1, &value) == 0);
    jdv_relation_t relation = JDV_RELATION_NO;
    CHECK(jdv_schema_accepts_checked(&any, &value, &relation,
                                     &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_YES);
    CHECK(jdv_schema_accepts_checked(&invalid, &value, &relation,
                                     &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_NO);
    CHECK(jdv_schema_accepts_checked(&unknown, &value, &relation,
                                     &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_RUNTIME);
    CHECK(!jdv_schema_accepts(&unknown, &value));
    CHECK(jdv_schema_relation_checked(&text, &any, &relation,
                                      &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_YES);
    CHECK(jdv_schema_relation_checked(&unknown, &text, &relation,
                                      &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_RUNTIME);
    CHECK(jdv_schema_relation_checked(&any, &text, &relation,
                                      &JDV_DEFAULT_LIMITS) == 0 &&
          relation == JDV_RELATION_NO);

    jdv_schema_t malformed = text;
    malformed.kind = (jdv_schema_kind_t)99;
    relation = JDV_RELATION_RUNTIME;
    CHECK(jdv_schema_accepts_checked(&malformed, &value, &relation,
                                     &JDV_DEFAULT_LIMITS) < 0 && errno == EINVAL);
    CHECK(relation == JDV_RELATION_RUNTIME);
    CHECK(jdv_schema_relation_checked(&malformed, &text, &relation,
                                      &JDV_DEFAULT_LIMITS) < 0 && errno == EINVAL);
    CHECK(relation == JDV_RELATION_RUNTIME);

    jdv_schema_set_t set;
    CHECK(jdv_schema_set_init(&set, &any, &text, &JDV_DEFAULT_LIMITS) == 0);
    CHECK(set.validation_scope == JDV_VALIDATION_NONE);
    CHECK(jdv_schema_set_transition(&set, &text, NULL, JDV_VALIDATION_SAMPLED,
                                    &JDV_DEFAULT_LIMITS) == 0);
    jdv_schema_set_t set_before = set;
    CHECK(jdv_schema_set_transition(&set, &text, NULL, JDV_VALIDATION_NONE,
                                    &JDV_DEFAULT_LIMITS) < 0);
    CHECK(memcmp(&set, &set_before, sizeof(set)) == 0);
    CHECK(jdv_schema_set_transition(&set, &text, &text, JDV_VALIDATION_COMPLETE,
                                    &JDV_DEFAULT_LIMITS) == 0);
    set_before = set;
    CHECK(jdv_schema_set_transition(&set, &text, &text, JDV_VALIDATION_FAILED,
                                    &JDV_DEFAULT_LIMITS) < 0 && errno == EALREADY);
    CHECK(memcmp(&set, &set_before, sizeof(set)) == 0);

    jdv_schema_set_t bad_set, bad_before;
    memset(&bad_set, 0x66, sizeof(bad_set));
    bad_before = bad_set;
    CHECK(jdv_schema_set_init(&bad_set, &text, &any,
                              &JDV_DEFAULT_LIMITS) < 0 && errno == EDOM);
    CHECK(memcmp(&bad_set, &bad_before, sizeof(bad_set)) == 0);

    test_sink_t encoded = {0};
    CHECK(encode_schema(&unknown, &encoded) == 0);
    CHECK(encoded.used == 6 && memcmp(encoded.bytes, "JSC1\0\0", 6) == 0);
    jdv_schema_t decoded;
    size_t consumed = 0;
    CHECK(jdv_schema_decode_exact(&arena, encoded.bytes, encoded.used,
                                  &decoded, &consumed) == 0);
    CHECK(decoded.kind == JDV_SCHEMA_UNKNOWN && consumed == encoded.used);

    jdv_stream_end_t end;
    CHECK(jdv_stream_end_init(&end) == 0);
    CHECK(jdv_stream_end_validate(&end, &JDV_DEFAULT_LIMITS) == 0);
    end.state = JDV_STREAM_COMPLETE;
    end.producer_status = 1;
    end.transform_status = 2;
    end.transport_status = 3;
    end.final_schema = &text;
    end.final_hash_present = 1;
    end.final_hash_algorithm = JDV_HASH_BLAKE3;
    end.completeness = JDV_COMPLETENESS_COMPLETE;
    CHECK(jdv_stream_end_validate(&end, &JDV_DEFAULT_LIMITS) == 0);
    CHECK(end.producer_status == 1 && end.transform_status == 2 &&
          end.transport_status == 3);
    end.version++;
    CHECK(jdv_stream_end_validate(&end, &JDV_DEFAULT_LIMITS) < 0 &&
          errno == EPROTONOSUPPORT);
    end.version = JDV_STREAM_END_VERSION;
    end.state = JDV_STREAM_INCOMPLETE;
    end.reason = JDV_LITERAL("limited");
    end.completeness = JDV_COMPLETENESS_PARTIAL;
    end.truncated = 1;
    end.final_schema = NULL;
    end.final_hash_present = 0;
    end.final_hash_algorithm = JDV_HASH_NONE;
    CHECK(jdv_stream_end_validate(&end, &JDV_DEFAULT_LIMITS) == 0);
    return 0;
}

static int test_writer_and_reentrant_sink(void) {
    test_sink_t overflow_sink = {0};
    jdv_writer_t writer;
    jdv_writer_init(&writer, test_sink, &overflow_sink);
    writer.bytes_written = UINT64_MAX;
    CHECK(jdv_writer_write(&writer, "x", 1) < 0 && errno == EOVERFLOW);
    CHECK(writer.error == EOVERFLOW && overflow_sink.calls == 0);

    jdv_value_t value = jdv_value_i64(-7);
    test_sink_t baseline = {0};
    CHECK(encode_value(&value, &baseline) == 0);
    mutating_sink_t mutation = {
        .output = {.max_write = 2},
        .value = &value,
    };
    jdv_writer_init(&writer, mutating_sink, &mutation);
    CHECK(jdv_encode(&value, &writer) == 0 && jdv_writer_finish(&writer) == 0);
    CHECK(mutation.mutated && value.as.i64 == 999);
    CHECK(mutation.output.used == baseline.used &&
          memcmp(mutation.output.bytes, baseline.bytes, baseline.used) == 0);

    jdv_schema_t schema;
    CHECK(jdv_schema_scalar(JDV_STRING, 0, &schema) == 0);
    test_sink_t schema_baseline = {0};
    CHECK(encode_schema(&schema, &schema_baseline) == 0);
    mutating_sink_t schema_mutation = {
        .output = {.max_write = 1},
        .schema = &schema,
    };
    jdv_writer_init(&writer, mutating_sink, &schema_mutation);
    CHECK(jdv_schema_encode(&schema, &writer) == 0 &&
          jdv_writer_finish(&writer) == 0);
    CHECK(schema_mutation.mutated && schema.as.scalar == JDV_BOOL);
    CHECK(schema_mutation.output.used == schema_baseline.used &&
          memcmp(schema_mutation.output.bytes, schema_baseline.bytes,
                 schema_baseline.used) == 0);
    return 0;
}

int main(void) {
    CHECK(test_values_and_data() == 0);
    CHECK(test_schema() == 0);
    CHECK(test_canonical_identity_and_io() == 0);
    CHECK(test_failure_atomicity_and_limits() == 0);
    CHECK(test_signed_golden_vectors() == 0);
    CHECK(test_schema_states_and_contracts() == 0);
    CHECK(test_writer_and_reentrant_sink() == 0);
    puts("PASS: semantic value core D01-D04");
    return 0;
}
