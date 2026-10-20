// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "json_writer.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void flush(jash_json_writer_t* w) {
    if (w->error) return;
    size_t done = 0;
    while (done < w->used) {
        errno = 0;
        long n = w->sink(w->context, w->buffer + done, w->used - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > w->used - done) {
            w->error = n < 0 && errno ? errno : EIO;
            return;
        }
        done += (size_t)n;
    }
    w->used = 0;
}

static void append(jash_json_writer_t* w, const char* s, size_t n) {
    while (n && !w->error) {
        if (w->used == sizeof(w->buffer)) flush(w);
        if (w->error) return;
        size_t copy = sizeof(w->buffer) - w->used;
        if (copy > n) copy = n;
        memcpy(w->buffer + w->used, s, copy);
        w->used += copy; s += copy; n -= copy;
    }
}

void jash_jsonw_init(jash_json_writer_t* w, jash_json_sink_t sink, void* context) {
    w->sink = sink;
    w->context = context;
    w->used = 0;
    w->error = sink ? 0 : EINVAL;
}

void jash_jsonw_raw(jash_json_writer_t* w, const char* s) {
    if (!s) { if (!w->error) w->error = EINVAL; return; }
    append(w, s, strlen(s));
}

void jash_jsonw_raw_n(jash_json_writer_t* w, const void* bytes, size_t len) {
    if (w->error) return;
    if (len && !bytes) { w->error = EINVAL; return; }
    append(w, bytes, len);
}

size_t jash_json_utf8_span(const unsigned char* p) {
    if (*p < 128) return *p ? 1u : 0u;
    unsigned c = *p++, value, minimum, count;
    if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
    else return 0;
    for (unsigned i = 0; i < count; i++) {
        if ((*p & 0xc0) != 0x80) return 0;
        value = (value << 6) | (*p++ & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) return 0;
    return count + 1u;
}

static int utf8_valid(const unsigned char* p) {
    while (*p) {
        if (*p < 128) { p++; continue; }
        size_t span = jash_json_utf8_span(p);
        if (!span) return 0;
        p += span;
    }
    return 1;
}

int jash_json_utf8_valid(const char* s) {
    return s && utf8_valid((const unsigned char*)s);
}

int jash_json_utf8_valid_n(const void* bytes, size_t len) {
    if (len && !bytes) return 0;
    const unsigned char* p = bytes;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i++], value, minimum, count;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
        else return 0;
        if (count > len - i) return 0;
        for (unsigned j = 0; j < count; j++) {
            if ((p[i] & 0xc0) != 0x80) return 0;
            value = (value << 6) | (p[i++] & 63);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}

void jash_jsonw_string_n(jash_json_writer_t* w, const void* bytes, size_t len) {
    if (w->error) return;
    if (!jash_json_utf8_valid_n(bytes, len)) { w->error = EILSEQ; return; }
    const char* text = bytes;
    append(w, "\"", 1);
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 32 && c != '"' && c != '\\') continue;
        append(w, text + start, i - start);
        if (c == '"' || c == '\\') {
            char escaped_char[2] = {'\\', (char)c};
            append(w, escaped_char, 2);
        } else {
            static const char hex[] = "0123456789abcdef";
            char escaped_char[6] = {'\\','u','0','0',hex[c >> 4],hex[c & 15]};
            append(w, escaped_char, 6);
        }
        start = i + 1;
    }
    if (len > start) append(w, text + start, len - start);
    append(w, "\"", 1);
}

void jash_jsonw_text_or_bytes(jash_json_writer_t* w, const void* bytes, size_t len) {
    if (jash_json_utf8_valid_n(bytes, len)) jash_jsonw_string_n(w, bytes, len);
    else jash_jsonw_bytes(w, bytes, len);
}

static void escaped(jash_json_writer_t* w, const char* s) {
    const char* run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c != '"' && c != '\\') continue;
        append(w, run, (size_t)(s - run));
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            append(w, esc, sizeof(esc));
        } else {
            static const char hex[] = "0123456789abcdef";
            char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            append(w, esc, sizeof(esc));
        }
        run = s + 1;
    }
    append(w, run, (size_t)(s - run));
}

void jash_jsonw_string_pair(jash_json_writer_t* w, const char* a, const char* b) {
    const char* parts[] = {a, b};
    jash_jsonw_string_parts(w, parts, 2);
}

void jash_jsonw_string_parts(jash_json_writer_t* w, const char* const* parts, size_t n) {
    if (w->error) return;
    if (n && !parts) { w->error = EINVAL; return; }
    for (size_t i = 0; i < n; i++) {
        if (!parts[i]) { w->error = EINVAL; return; }
        if (!utf8_valid((const unsigned char*)parts[i])) { w->error = EILSEQ; return; }
    }
    append(w, "\"", 1);
    for (size_t i = 0; i < n; i++) escaped(w, parts[i]);
    append(w, "\"", 1);
}

void jash_jsonw_string(jash_json_writer_t* w, const char* s) {
    jash_jsonw_string_pair(w, "", s);
}

void jash_jsonw_string_or_bytes(jash_json_writer_t* w, const char* s) {
    if (w->error) return;
    if (!s) { jash_jsonw_raw(w, "null"); return; }
    if (utf8_valid((const unsigned char*)s)) {
        append(w, "\"", 1); escaped(w, s); append(w, "\"", 1);
    } else jash_jsonw_bytes(w, s, strlen(s));
}

void jash_jsonw_u64(jash_json_writer_t* w, uint64_t value) {
    char number[20];
    size_t n = sizeof(number);
    do { number[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    append(w, number + n, sizeof(number) - n);
}

void jash_jsonw_i64(jash_json_writer_t* w, int64_t value) {
    if (value < 0) {
        append(w, "-", 1);
        jash_jsonw_u64(w, (uint64_t)(-(value + 1)) + 1u);
    } else jash_jsonw_u64(w, (uint64_t)value);
}

void jash_jsonw_safe_i64(jash_json_writer_t* w, int64_t value) {
    int quoted = value < -INT64_C(9007199254740991) || value > INT64_C(9007199254740991);
    if (quoted) append(w, "\"", 1);
    jash_jsonw_i64(w, value);
    if (quoted) append(w, "\"", 1);
}

void jash_jsonw_safe_u64(jash_json_writer_t* w, uint64_t value) {
    int quoted = value > UINT64_C(9007199254740991);
    if (quoted) append(w, "\"", 1);
    jash_jsonw_u64(w, value);
    if (quoted) append(w, "\"", 1);
}

void jash_jsonw_rounded(jash_json_writer_t* w, double value) {
    if (w->error) return;
    if (!isfinite(value)) { w->error = EDOM; return; }
    char number[384]; /* includes the 309 decimal digits of DBL_MAX */
    int n = snprintf(number, sizeof(number), "%.0f", value);
    if (n < 0 || (size_t)n >= sizeof(number)) { w->error = EOVERFLOW; return; }
    int quoted = value < -9007199254740991.0 || value > 9007199254740991.0;
    if (quoted) append(w, "\"", 1);
    append(w, number, (size_t)n);
    if (quoted) append(w, "\"", 1);
}

void jash_jsonw_bytes(jash_json_writer_t* w, const void* data, size_t n) {
    if (n && !data) { if (!w->error) w->error = EINVAL; return; }
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* p = data;
    jash_jsonw_raw(w, "{\"encoding\":\"base64\",\"data\":\"");
    while (n && !w->error) {
        unsigned a = p[0], b = n > 1 ? p[1] : 0, c = n > 2 ? p[2] : 0;
        char q[4] = {alphabet[a >> 2], alphabet[((a & 3) << 4) | (b >> 4)],
                     n > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=',
                     n > 2 ? alphabet[c & 63] : '='};
        append(w, q, sizeof(q));
        size_t consumed = n > 3 ? 3 : n;
        p += consumed; n -= consumed;
    }
    jash_jsonw_raw(w, "\"}");
}

int jash_jsonw_finish(jash_json_writer_t* w) {
    flush(w);
    if (!w->error) return 0;
    errno = w->error;
    return -1;
}

long jash_jsonw_fd_sink(void* context, const char* s, size_t n) {
    return (long)write(*(const int*)context, s, n);
}

long jash_jsonw_buffer_sink(void* context, const char* s, size_t n) {
    jash_json_buffer_t* b = context;
    if (!b->data || b->used >= b->capacity || n >= b->capacity - b->used) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(b->data + b->used, s, n);
    b->used += n;
    b->data[b->used] = '\0';
    return (long)n;
}
