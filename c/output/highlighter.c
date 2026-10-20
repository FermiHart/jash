// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — output/highlighter — auto-detect and highlight command output.

#include "highlighter.h"
#include <string.h>
#include <stdlib.h>

#define HIGHLIGHT_BUF_SIZE 4096

typedef enum {
    FMT_UNKNOWN,
    FMT_JSON,
    FMT_LOG,
    FMT_SQL,
    FMT_XML,
    FMT_NPM,
    FMT_GIT,
    FMT_BASH
} format_t;

struct jash_highlighter {
    char buffer[HIGHLIGHT_BUF_SIZE];
    size_t buf_len;
    format_t detected;
    int enabled;
};

/* J6: global flag, accessed via getter/setter functions below.
 * Not thread-safe but jash is single-threaded. */
static int g_highlight_enabled = 1;

jash_highlighter_t* jash_highlighter_create(void) {
    jash_highlighter_t* h = (jash_highlighter_t*)malloc(sizeof(jash_highlighter_t));
    if (!h) return NULL;
    h->buf_len = 0;
    h->detected = FMT_UNKNOWN;
    h->enabled = 1;
    return h;
}

void jash_highlighter_free(jash_highlighter_t* h) {
    if (h) free(h);
}

int jash_highlighting_is_global_enabled(void) {
    return g_highlight_enabled;
}

void jash_highlighting_set_global_enabled(int enabled) {
    g_highlight_enabled = enabled;
}

/* Detect format from buffer content */
static format_t detect_format(const char* buf, size_t len) {
    if (len < 4) return FMT_UNKNOWN;
    
    /* Skip leading whitespace */
    const char* p = buf;
    while (len > 0 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
        len--;
    }
    
    if (len < 1) return FMT_UNKNOWN;
    
    /* JSON: starts with { or [ */
    if (p[0] == '{' || p[0] == '[') return FMT_JSON;
    
    /* XML: starts with < */
    if (p[0] == '<') return FMT_XML;
    
    /* SQL: common keywords */
    if (len >= 6) {
        if ((p[0] == 'S' || p[0] == 's') && (p[1] == 'E' || p[1] == 'e') &&
            (p[2] == 'L' || p[2] == 'l') && (p[3] == 'E' || p[3] == 'e') &&
            (p[4] == 'C' || p[4] == 'c') && (p[5] == 'T' || p[5] == 't')) {
            return FMT_SQL;
        }
        if ((p[0] == 'I' || p[0] == 'i') && (p[1] == 'N' || p[1] == 'n') &&
            (p[2] == 'S' || p[2] == 's') && (p[3] == 'E' || p[3] == 'e') &&
            (p[4] == 'R' || p[4] == 'r') && (p[5] == 'T' || p[5] == 't')) {
            return FMT_SQL;
        }
    }
    
    /* Log: common patterns */
    if (len >= 19) {
        /* YYYY-MM-DD or [LEVEL] */
        if ((p[0] >= '1' && p[0] <= '9') && p[4] == '-' && p[7] == '-') {
            return FMT_LOG;
        }
        if (p[0] == '[' && (p[1] == 'I' || p[1] == 'W' || p[1] == 'E' || p[1] == 'D')) {
            return FMT_LOG;
        }
    }
    
    /* Git: common patterns */
    if (len >= 6) {
        if ((p[0] == 'c' && p[1] == 'o' && p[2] == 'm' && p[3] == 'm' && p[4] == 'i' && p[5] == 't') ||
            (p[0] == 'd' && p[1] == 'i' && p[2] == 'f' && p[3] == 'f' && p[4] == ' ')) {
            return FMT_GIT;
        }
    }
    
    /* NPM: package.json patterns */
    if (len >= 12) {
        if (strstr(p, "npm warn") || strstr(p, "npm ERR") || strstr(p, "npm info")) {
            return FMT_NPM;
        }
    }
    
    /* Bash: shell script patterns */
    if (len >= 2 && p[0] == '#' && p[1] == '!') {
        return FMT_BASH;
    }
    
    return FMT_UNKNOWN;
}

long jash_highlight_chunk(jash_highlighter_t* h, const char* buf, size_t len,
                          jash_writer_t w, void* ctx) {
    if (!h || !h->enabled || !g_highlight_enabled) {
        return w(ctx, buf, len);
    }
    
    /* Buffer until we can detect format */
    if (h->detected == FMT_UNKNOWN) {
        size_t space = HIGHLIGHT_BUF_SIZE - h->buf_len;
        size_t copy = len < space ? len : space;
        memcpy(h->buffer + h->buf_len, buf, copy);
        h->buf_len += copy;
        
        /* Try to detect after buffering at least 64 bytes or full chunk */
        if (h->buf_len >= 64 || copy < len) {
            h->detected = detect_format(h->buffer, h->buf_len);
        }
        
        /* If buffer full, flush as plain text */
        if (h->buf_len >= HIGHLIGHT_BUF_SIZE) {
            long r = w(ctx, h->buffer, h->buf_len);
            h->buf_len = 0;
            return r;
        }
        
        return (long)copy;
    }
    
    /* Format detected — for now, just pass through.
     * TODO: Integrate specific highlighters when they're refactored. */
    return w(ctx, buf, len);
}

long jash_highlight_flush(jash_highlighter_t* h, jash_writer_t w, void* ctx) {
    if (!h || h->buf_len == 0) return 0;
    
    /* Detect format if not yet done */
    if (h->detected == FMT_UNKNOWN) {
        h->detected = detect_format(h->buffer, h->buf_len);
    }
    
    /* For now, just flush as plain text.
     * TODO: Apply highlighting when highlighters are refactored. */
    long r = w(ctx, h->buffer, h->buf_len);
    
    h->buf_len = 0;
    h->detected = FMT_UNKNOWN;
    return r;
}
