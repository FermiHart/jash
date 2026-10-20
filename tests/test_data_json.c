// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "core/data_json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "data JSON check failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

static unsigned char storage[512u * 1024u];
static jdv_arena_t arena;
static jdv_limits_t limits;

static int parse(const char* text, jdj_value_t* out) {
    return jdj_parse(&arena, text, strlen(text), &limits, out);
}
static const jdv_value_t* native_field(const jdv_value_t* object, const char* name) {
    if (object->kind != JDV_RECORD) return NULL;
    for (size_t i = 0; i < object->as.record.len; i++) {
        const jdv_record_field_t* f = &object->as.record.fields[i];
        if (f->key.len == strlen(name) && !memcmp(f->key.data, name, f->key.len)) return &f->value;
    }
    return NULL;
}
static int stringify(const jdj_value_t* value, char* text, size_t capacity) {
    jash_json_buffer_t buffer = {text, capacity, 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    if (jdj_write(&writer, value, &limits)) return -1;
    return jash_jsonw_finish(&writer);
}

static int exact_roundtrip(void) {
    const char* json = "{\"min\":-9223372036854775808,\"max\":18446744073709551615,"
        "\"n\":9007199254740993,\"s\":\"a\\u0000b\\ud83d\\ude3a\","
        "\"nested\":{\"a\":[null,true,false,{}]}}";
    jdj_value_t parsed;
    CHECK(parse(json, &parsed) == 0);
    const jdj_value_t* string = jdj_get(&parsed, "s", 1);
    CHECK(string && string->kind == JDJ_STRING && string->scalar.len == 7);
    CHECK(!memcmp(string->scalar.data, "a\0b\xf0\x9f\x98\xba", 7));
    jdv_value_t native;
    CHECK(jdj_to_value(&arena, &parsed, &limits, &native) == 0);
    CHECK(native_field(&native, "min")->kind == JDV_I64);
    CHECK(native_field(&native, "min")->as.i64 == INT64_MIN);
    CHECK(native_field(&native, "max")->kind == JDV_U64);
    CHECK(native_field(&native, "max")->as.u64 == UINT64_MAX);
    CHECK(native_field(&native, "n")->as.u64 == UINT64_C(9007199254740993));
    CHECK(native_field(&native, "s")->as.slice.len == 7);
    jdj_value_t projected;
    CHECK(jdj_from_value(&arena, &native, &limits, &projected) == 0);
    char output[1024];
    CHECK(stringify(&projected, output, sizeof(output)) == 0);
    CHECK(strstr(output, "9007199254740993") && strstr(output, "18446744073709551615"));
    CHECK(strstr(output, "a\\u0000b"));
    CHECK(strstr(output, "[null,true,false,{}]"));
    /* Parse owns its copy; changing the input cannot change the value. */
    char mutable[] = "{\"x\":\"retained\"}";
    CHECK(parse(mutable, &parsed) == 0);
    memset(mutable, 'z', sizeof(mutable) - 1);
    CHECK(stringify(&parsed, output, sizeof(output)) == 0);
    CHECK(!strcmp(output, "{\"x\":\"retained\"}"));
    return 0;
}

static int parse_failures(void) {
    static const char* const bad[] = {
        "", " ", "nul", "nullx", "false true", "01", "-01", "+1", "--1",
        "1.", "1e", "1e+", ".1", "NaN", "Infinity", "[1,]", "[,1]",
        "{\"a\":1,}", "{a:1}", "{\"a\" 1}", "{\"a\":1 \"b\":2}",
        "{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"",
        "\"\\u12\"", "\"\\q\"", "\"a\n\"", "\"abc", "\"abc\\\"",
        "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"",
        "\"\xe2\x82\"", "\"\xff\""
    };
    jdj_value_t sentinel = {.kind = JDJ_BOOL, .boolean = 1};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t mark = arena.used;
        jdj_value_t output = sentinel;
        CHECK(parse(bad[i], &output) == -1);
        CHECK(arena.used == mark && !memcmp(&output, &sentinel, sizeof(output)));
    }
    const char raw_nul[] = {'"', 'a', 0, 'b', '"'};
    jdj_value_t output = sentinel;
    size_t mark = arena.used;
    CHECK(jdj_parse(&arena, raw_nul, sizeof(raw_nul), &limits, &output) == -1);
    CHECK(arena.used == mark && !memcmp(&output, &sentinel, sizeof(output)));
    /* Reject parser input in the writable remainder instead of clobbering it. */
    memcpy(arena.data + arena.used, "[1]", 3);
    CHECK(jdj_parse(&arena, arena.data + arena.used, 3, &limits, &output) == -1);
    CHECK(arena.used == mark);
    return 0;
}

static int unsupported_native_numbers(void) {
    static const char* const bad[] = {"0.1", "1e2", "1E+0", "1.0", "18446744073709551616", "-9223372036854775809"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        jdj_value_t json;
        CHECK(parse(bad[i], &json) == 0);
        char text[80]; CHECK(stringify(&json, text, sizeof(text)) == 0);
        CHECK(!strcmp(text, bad[i]));
        jdv_value_t sentinel = jdv_value_bool(1), output = sentinel;
        size_t mark = arena.used;
        CHECK(jdj_to_value(&arena, &json, &limits, &output) == -1);
        CHECK(errno == (i < 4 ? ENOTSUP : EOVERFLOW));
        CHECK(arena.used == mark && !memcmp(&output, &sentinel, sizeof(output)));
    }
    jdv_value_t output = jdv_value_bool(1);
    size_t mark = arena.used;
    const char* key_with_nul = "{\"a\\u0000b\":1}";
    CHECK(jdv_json_parse(&arena, key_with_nul, strlen(key_with_nul), &limits, &output) == -1);
    CHECK(arena.used == mark && output.kind == JDV_BOOL && output.as.boolean);
    return 0;
}

static int native_projection(void) {
    const unsigned char bytes[] = {'a', 0, 0xff};
    jdv_value_t value;
    CHECK(jdv_value_bytes(&arena, bytes, sizeof(bytes), &value) == 0);
    jdj_value_t json;
    CHECK(jdj_from_value(&arena, &value, &limits, &json) == 0);
    char text[1024]; CHECK(stringify(&json, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "{\"encoding\":\"base64\",\"data\":\"YQD/\"}"));
    /* JSON objects that resemble byte tags remain ordinary user records. */
    CHECK(jdj_to_value(&arena, &json, &limits, &value) == 0 && value.kind == JDV_RECORD);
    CHECK(jdv_value_path(&arena, "bad\xff", 4, &value) == 0);
    CHECK(jdj_from_value(&arena, &value, &limits, &json) == 0);
    CHECK(stringify(&json, text, sizeof(text)) == 0);
    CHECK(!strcmp(text, "{\"encoding\":\"base64\",\"data\":\"YmFk/w==\"}"));
    CHECK(jdv_value_path(&arena, "path/ok", 7, &value) == 0);
    CHECK(jdj_from_value(&arena, &value, &limits, &json) == 0);
    CHECK(json.kind == JDJ_STRING && json.scalar.len == 7);
    value = jdv_value_filesize(UINT64_MAX);
    CHECK(jdj_from_value(&arena, &value, &limits, &json) == 0);
    CHECK(stringify(&json, text, sizeof(text)) == 0 && !strcmp(text, "18446744073709551615"));
    value = jdv_value_bool(1); value.as.boolean = 2;
    size_t mark = arena.used;
    jdj_value_t sentinel = {.kind = JDJ_BOOL, .boolean = 1}; json = sentinel;
    CHECK(jdj_from_value(&arena, &value, &limits, &json) == -1);
    CHECK(arena.used == mark && !memcmp(&json, &sentinel, sizeof(json)));
    return 0;
}

static int rejected_ast(const jdj_value_t* value, const jdv_limits_t* budget) {
    char bytes[1024]; jash_json_buffer_t buffer = {bytes, sizeof(bytes), 0};
    jash_json_writer_t writer; jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    CHECK(jdj_validate(value, budget) == -1);
    CHECK(jdj_write(&writer, value, budget) == -1);
    CHECK(buffer.used == 0 && writer.used == 0);
    jdv_value_t sentinel = jdv_value_bool(1), output = sentinel;
    size_t mark = arena.used;
    CHECK(jdj_to_value(&arena, value, budget, &output) == -1);
    CHECK(arena.used == mark && !memcmp(&output, &sentinel, sizeof(output)));
    return 0;
}
static int public_ast_validation(void) {
    static const char* const numbers[] = {"", "1,2", "NaN", "01", "-", "1e+", "1.0junk", "+1"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        jdj_value_t v = {.kind = JDJ_NUMBER, .scalar = {(const uint8_t*)numbers[i], strlen(numbers[i])}};
        CHECK(rejected_ast(&v, &limits) == 0);
    }
    /* Non-NUL-terminated, truncated UTF-8 must not read beyond the slice. */
    uint8_t* truncated = malloc(1); CHECK(truncated != NULL); truncated[0] = 0xe2;
    jdj_value_t v = {.kind = JDJ_STRING, .scalar = {truncated, 1}};
    CHECK(rejected_ast(&v, &limits) == 0); free(truncated);
    v = (jdj_value_t){.kind = JDJ_BOOL, .boolean = 2}; CHECK(rejected_ast(&v, &limits) == 0);
    jdj_node_t nodes[2] = {{.key = JDV_LITERAL("x"), .value = {.kind = JDJ_NULL}}, {.key = JDV_LITERAL("x"), .value = {.kind = JDJ_NULL}}};
    nodes[0].next = &nodes[1];
    v = (jdj_value_t){.kind = JDJ_OBJECT, .children = nodes, .count = 2};
    CHECK(rejected_ast(&v, &limits) == 0);
    nodes[1].key = JDV_LITERAL("y"); v.count = 1;
    CHECK(rejected_ast(&v, &limits) == 0);
    v.count = 3; CHECK(rejected_ast(&v, &limits) == 0);
    v.count = 2; nodes[1].next = nodes;
    CHECK(rejected_ast(&v, &limits) == 0);
    CHECK(jdj_get(&v, "absent", 6) == NULL);
    nodes[1].next = NULL; nodes[0].value = v; /* Recursive graph, not JSON. */
    CHECK(rejected_ast(&v, &limits) == 0);
    return 0;
}

static int long_number_lexemes(void) {
    /* Cross both the old 511-byte chunk and the writer's 1024-byte buffer.
     * Compare exact lexemes; conversion to floating point is not an oracle. */
    char input[4096], output[4096];
    for (unsigned form = 0; form < 3; form++) {
        size_t length = 0;
        if (form == 0) { memset(input, '7', 1800); length = 1800; }
        else {
            const char* prefix = form == 1 ? "0." : "1e+";
            length = strlen(prefix); memcpy(input, prefix, length);
            memset(input + length, '0', 1800); length += 1800;
            input[length++] = '2';
        }
        input[length] = 0;
        jdj_value_t parsed;
        CHECK(parse(input, &parsed) == 0);
        CHECK(parsed.kind == JDJ_NUMBER && parsed.scalar.len == length);
        CHECK(stringify(&parsed, output, sizeof(output)) == 0);
        CHECK(strlen(output) == length && !memcmp(output, input, length));
        jdv_value_t sentinel = jdv_value_bool(1), native = sentinel;
        size_t mark = arena.used;
        CHECK(jdj_to_value(&arena, &parsed, &limits, &native) == -1);
        CHECK(errno == (form == 0 ? EOVERFLOW : ENOTSUP));
        CHECK(arena.used == mark && !memcmp(&native, &sentinel, sizeof(native)));
        jdv_limits_t small = limits; small.max_encoded_bytes = length - 1;
        CHECK(rejected_ast(&parsed, &small) == 0);

        /* A bad late suffix must reject the whole value before publishing a
         * plausible numeric prefix; it cannot be hidden beyond a chunk. */
        input[length++] = 'x'; input[length] = 0;
        jdj_value_t invalid = {.kind = JDJ_NUMBER,
            .scalar = {(const uint8_t*)input, length}};
        CHECK(rejected_ast(&invalid, &limits) == 0);
        CHECK(parse(input, &parsed) == -1 && arena.used == mark);
    }
    memset(input, '1', 1800); memcpy(input + 1800, "e+", 3);
    jdj_value_t incomplete = {.kind = JDJ_NUMBER,
        .scalar = {(const uint8_t*)input, 1802}};
    CHECK(rejected_ast(&incomplete, &limits) == 0);
    return 0;
}

static int cumulative_bounds(void) {
    struct example { const char* json; int limit; } examples[] = {
        {"{\"x\":\"a\",\"y\":\"b\"}", 0},
        {"[[1],[2]]", 1},
        {"{\"x\":{\"y\":1}}", 2},
        {"[1,2]", 3},
        {"[[1]]", 4},
        {"\"\\u0000\"", 5}
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        jdj_value_t v; CHECK(parse(examples[i].json, &v) == 0);
        jdv_limits_t small = limits;
        switch (examples[i].limit) {
            case 0: small.max_aggregate_bytes = 3; break;
            case 1: small.max_list_items = 3; break;
            case 2: small.max_record_fields = 1; break;
            case 3: small.max_nodes = 2; break;
            case 4: small.max_depth = 2; break;
            case 5: small.max_encoded_bytes = 7; break;
        }
        CHECK(rejected_ast(&v, &small) == 0);
        size_t mark = arena.used; jdj_value_t out = {.kind = JDJ_BOOL, .boolean = 1};
        CHECK(jdj_parse(&arena, examples[i].json, strlen(examples[i].json), &small, &out) == -1);
        CHECK(arena.used == mark && out.kind == JDJ_BOOL && out.boolean);
    }
    unsigned char tiny[1]; jdv_arena_t short_arena; jdv_arena_init(&short_arena, tiny, sizeof(tiny));
    jdj_value_t out = {.kind = JDJ_BOOL, .boolean = 1};
    CHECK(jdj_parse(&short_arena, "[1]", 3, &limits, &out) == -1 && errno == ENOSPC);
    CHECK(short_arena.used == 0 && out.kind == JDJ_BOOL && out.boolean);
    return 0;
}

typedef struct { size_t bytes; int interrupt; int failure; } delivery_t;
static long delivery(void* context, const char* bytes, size_t size) {
    (void)bytes; delivery_t* sink = context;
    if (!sink->interrupt++) { errno = EINTR; return -1; }
    if (sink->failure) { errno = EPIPE; return -1; }
    if (size > 3) size = 3;
    sink->bytes += size; return (long)size;
}
static int checked_delivery(void) {
    jdj_value_t v; CHECK(parse("{\"s\":\"a\\u0000b\",\"n\":9007199254740993}", &v) == 0);
    jash_json_writer_t writer; delivery_t sink = {0};
    jash_jsonw_init(&writer, delivery, &sink);
    CHECK(jdj_write(&writer, &v, &limits) == 0 && jash_jsonw_finish(&writer) == 0);
    CHECK(sink.bytes == strlen("{\"s\":\"a\\u0000b\",\"n\":9007199254740993}"));
    sink = (delivery_t){.failure = 1}; jash_jsonw_init(&writer, delivery, &sink);
    CHECK(jdj_write(&writer, &v, &limits) == 0);
    CHECK(jash_jsonw_finish(&writer) == -1 && errno == EPIPE && sink.bytes == 0);
    CHECK(jdj_write(&writer, &v, &limits) == -1 && errno == EPIPE);
    return 0;
}

static uint32_t next_random(uint32_t* state) { *state = *state * 1664525u + 1013904223u; return *state; }
static int mutation_corpus(void) {
    static const char* const seeds[] = {"{\"x\":[true,null,123,\"a\\u0000b\"]}", "[1,-2,3.4,5e6]", "\"\\ud83d\\ude3a\"", "{\"a\":1,\"b\":2}", "null"};
    uint32_t random = UINT32_C(0x51ea9b3c);
    size_t accepted = 0;
    for (size_t trial = 0; trial < 5000; trial++) {
        const char* seed = seeds[next_random(&random) % (sizeof(seeds) / sizeof(seeds[0]))];
        unsigned char input[128]; size_t length = strlen(seed); memcpy(input, seed, length);
        unsigned edits = next_random(&random) % 5;
        for (unsigned e = 0; e < edits; e++) {
            size_t at = next_random(&random) % (length + 1);
            unsigned mode = next_random(&random) % 3;
            if (mode == 0 && at < length) input[at] = (unsigned char)next_random(&random);
            else if (mode == 1 && at < length) { memmove(input + at, input + at + 1, length - at - 1); length--; }
            else if (length < sizeof(input)) { memmove(input + at + 1, input + at, length - at); input[at] = (unsigned char)next_random(&random); length++; }
        }
        arena.used = 0;
        jdj_value_t json, again;
        if (jdj_parse(&arena, input, length, &limits, &json)) { CHECK(arena.used == 0); continue; }
        accepted++;
        char canonical[1024], canonical_again[1024];
        CHECK(stringify(&json, canonical, sizeof(canonical)) == 0);
        CHECK(parse(canonical, &again) == 0);
        CHECK(stringify(&again, canonical_again, sizeof(canonical_again)) == 0);
        CHECK(!strcmp(canonical, canonical_again));
    }
    CHECK(accepted > 100 && accepted < 4000);
    return 0;
}

int main(void) {
    jdv_arena_init(&arena, storage, sizeof(storage));
    limits = JDV_DEFAULT_LIMITS;
    CHECK(exact_roundtrip() == 0);
    CHECK(parse_failures() == 0);
    CHECK(unsupported_native_numbers() == 0);
    CHECK(native_projection() == 0);
    CHECK(public_ast_validation() == 0);
    CHECK(long_number_lexemes() == 0);
    CHECK(cumulative_bounds() == 0);
    CHECK(checked_delivery() == 0);
    CHECK(mutation_corpus() == 0);
    puts("data JSON: strict grammar, exact values, public AST validation, bounds, checked delivery, 5000 mutations passed");
    return 0;
}
