// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Bounded, allocation-free JSON output. Errors are sticky, never truncation.
#ifndef JASH_CORE_JSON_WRITER_H
#define JASH_CORE_JSON_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef long (*jash_json_sink_t)(void*, const char*, size_t);
typedef struct {
    jash_json_sink_t sink;
    void* context;
    size_t used;
    int error;
    char buffer[1024];
} jash_json_writer_t;

void jash_jsonw_init(jash_json_writer_t*, jash_json_sink_t, void*);
/* raw is for trusted JSON syntax only, never unescaped user data. */
void jash_jsonw_raw(jash_json_writer_t*, const char*);
/* Length-aware trusted syntax. The entire span is emitted verbatim; this
 * does not validate JSON and must not receive unvalidated input fragments. */
void jash_jsonw_raw_n(jash_json_writer_t*, const void*, size_t);
/* Text requires valid UTF-8; malformed text fails with EILSEQ. */
void jash_jsonw_string(jash_json_writer_t*, const char*);
/* Length-aware UTF-8 text, including embedded NUL escaped as \u0000. */
void jash_jsonw_string_n(jash_json_writer_t*, const void*, size_t);
void jash_jsonw_text_or_bytes(jash_json_writer_t*, const void*, size_t);
void jash_jsonw_string_pair(jash_json_writer_t*, const char*, const char*);
void jash_jsonw_string_parts(jash_json_writer_t*, const char* const*, size_t);
/* A UTF-8 string when representable, otherwise the tagged base64 byte object.
 * For filesystem paths and other byte strings; never replaces invalid bytes. */
void jash_jsonw_string_or_bytes(jash_json_writer_t*, const char*);
/* Arbitrary bytes use an explicit base64-tagged object, including embedded NUL. */
void jash_jsonw_bytes(jash_json_writer_t*, const void*, size_t);
void jash_jsonw_u64(jash_json_writer_t*, uint64_t);
void jash_jsonw_i64(jash_json_writer_t*, int64_t);
/* Exactly representable JSON number within +/- (2^53-1), decimal string beyond. */
void jash_jsonw_safe_i64(jash_json_writer_t*, int64_t);
void jash_jsonw_safe_u64(jash_json_writer_t*, uint64_t);
/* Legacy millisecond presentation (rounded to integer), locale-independent.
 * Non-finite input is an error. Large integers use decimal strings. */
void jash_jsonw_rounded(jash_json_writer_t*, double);
/* Returns -1 on sink/encoding failure; errno is the first failure. A failed
 * partial document must not be treated as a completed protocol response. */
int jash_jsonw_finish(jash_json_writer_t*);
long jash_jsonw_fd_sink(void*, const char*, size_t);
typedef struct { char* data; size_t capacity; size_t used; } jash_json_buffer_t;
/* NUL-terminates after each write; refuses insufficient space without truncation. */
long jash_jsonw_buffer_sink(void*, const char*, size_t);
/* Size of the first UTF-8 scalar in a NUL-terminated string, 0 for invalid/EOF.
 * Shared with the input parser; never reads beyond the terminating NUL. */
size_t jash_json_utf8_span(const unsigned char*);
int jash_json_utf8_valid(const char*);
int jash_json_utf8_valid_n(const void*, size_t);

#endif
