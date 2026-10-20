// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_wire.h"

#include <errno.h>
#include <limits.h>
#include <module/blake3.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t tag;
    const uint8_t* data;
    size_t size;
} wire_frame_t;

void jdv_wire_limits_init(jdv_wire_limits_t* limits) {
    if (!limits) return;
    limits->max_wire_bytes = UINT64_C(64) * 1024u * 1024u;
    limits->max_frame_bytes = 16u * 1024u * 1024u;
    limits->max_items = 65536;
    limits->value_limits = JDV_DEFAULT_LIMITS;
}

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

static int read_status(const uint8_t* bytes, int* result) {
    int64_t value = read_i64(bytes);
    if (value < INT_MIN || value > INT_MAX) return fail(EBADMSG);
    *result = (int)value;
    return 0;
}

static int next_frame(const uint8_t* bytes, size_t total, size_t* position,
                      const jdv_wire_limits_t* limits, wire_frame_t* frame) {
    if (*position > total || total - *position < 9) return fail(EBADMSG);
    frame->tag = bytes[(*position)++];
    uint64_t encoded = read_u64(bytes + *position);
    *position += 8;
    if (encoded > SIZE_MAX || encoded > limits->max_frame_bytes)
        return fail(E2BIG);
    frame->size = (size_t)encoded;
    if (frame->size > total - *position) return fail(EBADMSG);
    frame->data = bytes + *position;
    *position += frame->size;
    return 0;
}

static int parse_terminal(const wire_frame_t* frame,
                          const jdv_schema_t* final_schema,
                          const jdv_wire_limits_t* limits,
                          jdv_stream_end_t* end) {
    if (frame->size < 96) return fail(EBADMSG);
    const uint8_t* p = frame->data;
    if (p[5] || p[6] || p[7]) return fail(EBADMSG);
    uint64_t reason_size = read_u64(p + 88);
    if (reason_size != frame->size - 96u) return fail(EBADMSG);
    if (!p[3])
        for (size_t i = 56; i < 88; i++)
            if (p[i]) return fail(EBADMSG);
    if (jdv_stream_end_init(end) < 0) return -1;
    end->state = (jdv_stream_state_t)p[0];
    end->completeness = (jdv_completeness_t)p[1];
    end->truncated = p[2];
    end->final_hash_present = p[3];
    end->final_hash_algorithm = (jdv_hash_algorithm_t)p[4];
    end->items_seen = read_u64(p + 8);
    end->bytes_seen = read_u64(p + 16);
    end->errors_seen = read_u64(p + 24);
    if (read_status(p + 32, &end->producer_status) < 0 ||
        read_status(p + 40, &end->transform_status) < 0 ||
        read_status(p + 48, &end->transport_status) < 0)
        return -1;
    memcpy(end->final_hash, p + 56, JDV_HASH_SIZE);
    end->reason.data = reason_size ? p + 96 : NULL;
    end->reason.len = (size_t)reason_size;
    end->final_schema = final_schema;
    if (jdv_stream_end_validate(end, &limits->value_limits) < 0)
        return fail(EBADMSG);
    return 0;
}

static int wire_validate_version(const void* data, size_t size,
                                 jdv_arena_t* arena,
                                 const jdv_wire_limits_t* limits,
                                 jdv_wire_info_t* out, int version) {
    if (!data || !arena || !arena->data || arena->used > arena->capacity ||
        !limits || !out || !limits->max_wire_bytes ||
        !limits->max_frame_bytes || !limits->max_items)
        return fail(EINVAL);
    size_t mark = arena->used;
    uintptr_t input = (uintptr_t)data, scratch = (uintptr_t)arena->data;
    if (size > UINTPTR_MAX - input ||
        arena->capacity > UINTPTR_MAX - scratch ||
        (size && arena->capacity &&
         input < scratch + arena->capacity && scratch < input + size))
        return fail(EINVAL);
    if (size > limits->max_wire_bytes) return fail(E2BIG);
    if (size < 4 || memcmp(data, version == 2 ? "JTV2" : "JTV1", 4))
        return fail(EBADMSG);
    const uint8_t* bytes = data;
    size_t position = 4, schema_mark = mark;
    jdv_schema_t declared = {0}, guaranteed = {0}, final = {0};
    jdv_stream_end_t terminal = {0};
    jdv_wire_info_t checked = {0};
    struct blake3 digest;
    blake3_init(&digest);
    const char* domain = version == 2 ? "JTV2-ITEMS-v1" : "JTV1-ITEMS-v1";
    blake3_update(&digest, domain, strlen(domain));
    uint64_t logical_total = 0;
    int phase = 0;
    while (position < size) {
        wire_frame_t frame;
        if (next_frame(bytes, size, &position, limits, &frame) < 0)
            goto invalid;
        if (phase == 0 && frame.tag == 'D') {
            if (jdv_schema_decode_exact_limited(arena, frame.data, frame.size,
                    &declared, NULL, &limits->value_limits) < 0) goto invalid;
            phase = 1;
        } else if (phase == 1 && frame.tag == 'G') {
            if (jdv_schema_decode_exact_limited(arena, frame.data, frame.size,
                    &guaranteed, NULL, &limits->value_limits) < 0) goto invalid;
            jdv_relation_t relation;
            if (jdv_schema_relation_checked(&guaranteed, &declared, &relation,
                    &limits->value_limits) < 0 || relation == JDV_RELATION_NO)
                goto malformed;
            schema_mark = arena->used;
            phase = 2;
        } else if (phase == 2 && frame.tag == 'I') {
            if (checked.items_seen >= limits->max_items) {
                errno = E2BIG;
                goto invalid;
            }
            if (version == 2) {
                if (frame.size < 8) goto malformed;
                uint64_t credit = read_u64(frame.data);
                if (credit > UINT64_MAX - logical_total) {
                    errno = EOVERFLOW;
                    goto invalid;
                }
                logical_total += credit;
                frame.data += 8;
                frame.size -= 8;
            }
            arena->used = schema_mark;
            jdv_value_t item;
            if (jdv_decode_exact_limited(arena, frame.data, frame.size,
                    &item, NULL, &limits->value_limits) < 0) goto invalid;
            jdv_relation_t d, g;
            if (jdv_schema_accepts_checked(&declared, &item, &d,
                    &limits->value_limits) < 0 || d != JDV_RELATION_YES ||
                jdv_schema_accepts_checked(&guaranteed, &item, &g,
                    &limits->value_limits) < 0 || g != JDV_RELATION_YES)
                goto malformed;
            uint8_t content[JDV_HASH_SIZE];
            if (jdv_hash_limited(&item, content,
                                 &limits->value_limits) < 0) goto invalid;
            blake3_update(&digest, content, sizeof(content));
            checked.items_seen++;
        } else if (phase == 2 && frame.tag == 'F') {
            arena->used = schema_mark;
            if (jdv_schema_decode_exact_limited(arena, frame.data, frame.size,
                    &final, NULL, &limits->value_limits) < 0) goto invalid;
            phase = 3;
        } else if ((phase == 2 || phase == 3) && frame.tag == 'T') {
            if (parse_terminal(&frame, phase == 3 ? &final : NULL,
                               limits, &terminal) < 0) goto invalid;
            if (position != size || terminal.items_seen != checked.items_seen ||
                (version == 2 && terminal.bytes_seen != logical_total) ||
                (terminal.state == JDV_STREAM_COMPLETE && phase != 3))
                goto malformed;
            if (phase == 3) {
                jdv_relation_t d, g;
                if (jdv_schema_relation_checked(&final, &declared, &d,
                        &limits->value_limits) < 0 ||
                    jdv_schema_relation_checked(&final, &guaranteed, &g,
                        &limits->value_limits) < 0 ||
                    d == JDV_RELATION_NO || g == JDV_RELATION_NO ||
                    (terminal.state == JDV_STREAM_COMPLETE &&
                     (d != JDV_RELATION_YES || g != JDV_RELATION_YES)))
                    goto malformed;
            }
            if (terminal.final_hash_present) {
                uint8_t hash[JDV_HASH_SIZE];
                blake3_final(&digest, hash, sizeof(hash));
                if (memcmp(hash, terminal.final_hash, sizeof(hash)))
                    goto malformed;
            }
            checked.state = terminal.state;
            checked.completeness = terminal.completeness;
            checked.bytes_seen = terminal.bytes_seen;
            checked.errors_seen = terminal.errors_seen;
            checked.truncated = terminal.truncated;
            checked.final_hash_present = terminal.final_hash_present;
            checked.logical_verified = version == 2;
            memcpy(checked.final_hash, terminal.final_hash, JDV_HASH_SIZE);
            arena->used = mark;
            *out = checked;
            return 0;
        } else goto malformed;
    }
malformed:
    errno = EBADMSG;
invalid:
    {
        int saved = errno ? errno : EBADMSG;
        arena->used = mark;
        errno = saved;
        return -1;
    }
}

int jdv_wire_validate(const void* data, size_t size, jdv_arena_t* arena,
                       const jdv_wire_limits_t* limits, jdv_wire_info_t* out) {
    return wire_validate_version(data, size, arena, limits, out, 1);
}

int jdv_wire_validate_v2(const void* data, size_t size, jdv_arena_t* arena,
                          const jdv_wire_limits_t* limits, jdv_wire_info_t* out) {
    return wire_validate_version(data, size, arena, limits, out, 2);
}
