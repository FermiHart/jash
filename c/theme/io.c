// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: theme/io — writer helpers + tiny printf for shell internals.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "io.h"

/* Decide once whether stderr should carry ANSI color. Cache so each diagnostic
 * doesn't re-stat fd 2 or re-read NO_COLOR. */
static int stderr_color_decision = -1; /* -1=undecided, 0=strip, 1=allow */

static int stderr_wants_color(void) {
    if (stderr_color_decision >= 0) return stderr_color_decision;
    const char* no_color = getenv("NO_COLOR");
    if (no_color && no_color[0]) { stderr_color_decision = 0; return 0; }
    if (!isatty(2)) { stderr_color_decision = 0; return 0; }
    stderr_color_decision = 1;
    return 1;
}

/* Strip CSI sequences (ESC `[` … final byte) and OSC (ESC `]` … BEL / ESC `\`)
 * from buf into out. Returns bytes written. Other ESC forms pass through
 * unchanged (rare for diagnostics; this filter is best-effort). */
static size_t strip_ansi(const char* buf, size_t n, char* out, size_t out_cap) {
    size_t oi = 0;
    for (size_t i = 0; i < n && oi < out_cap; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x1b && i + 1 < n) {
            unsigned char nx = (unsigned char)buf[i + 1];
            if (nx == '[') {
                i += 2;
                while (i < n) {
                    unsigned char x = (unsigned char)buf[i];
                    if (x >= 0x40 && x <= 0x7e) break; /* final byte */
                    i++;
                }
                continue;
            }
            if (nx == ']') {
                i += 2;
                while (i < n) {
                    if ((unsigned char)buf[i] == 0x07) break;
                    if ((unsigned char)buf[i] == 0x1b && i + 1 < n &&
                        (unsigned char)buf[i + 1] == '\\') { i++; break; }
                    i++;
                }
                continue;
            }
        }
        out[oi++] = (char)c;
    }
    return oi;
}

/* Canonical stderr writer — diagnostics go here (POSIX + bash parity).
 * Strips ANSI escapes when stderr is not a tty or NO_COLOR is set, so
 * wrapper captures (CI, agent transcripts, log files) don't get polluted
 * with raw `\x1b[91m...` sequences. Exported to modules for error reporting. */
__attribute__((visibility("default")))
long jash_stderr_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
    if (stderr_wants_color()) return (long)write(2, buf, n);
    char scratch[2048];
    if (n <= sizeof(scratch)) {
        size_t out = strip_ansi(buf, n, scratch, sizeof(scratch));
        return (long)write(2, scratch, out);
    }
    /* Oversize fallback: write in chunks. */
    long total = 0;
    size_t off = 0;
    while (off < n) {
        size_t chunk = (n - off > sizeof(scratch)) ? sizeof(scratch) : (n - off);
        size_t out = strip_ansi(buf + off, chunk, scratch, sizeof(scratch));
        long r = write(2, scratch, out);
        if (r < 0) return r;
        total += r;
        off += chunk;
    }
    return total;
}

static unsigned long io_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

long jash_writes(jash_writer_t w, void* ctx, const char* s) {
    if (!w || !s) return -1;
    unsigned long n = io_strlen(s);
    return w(ctx, s, (size_t)n);
}

/* SECURITY (audit terminal-escape-injection class): write UNTRUSTED text to the
 * terminal with every control byte neutralised, so an attacker-controlled
 * filename (completion), history line (reverse-i-search / load), command output
 * (prompt, highlighter), etc. cannot inject ESC/CSI/OSC sequences that retitle
 * the window, move the cursor, or smuggle keystrokes. Printable bytes and a
 * deliberate whitelist (TAB, LF, CR — needed for layout) pass through verbatim
 * and are emitted in runs; everything else (C0 < 0x20, DEL 0x7F, C1 0x80-0x9F,
 * notably ESC 0x1B) is rendered as a visible ^X (C0/DEL) or \xHH (C1) caret
 * placeholder. NOTE: callers that need to emit their OWN trusted escapes (the
 * shell's colour codes) must still use jash_writes for those and route only the
 * untrusted span through here. F E R M I ∞ H A R T */
long jash_writes_sanitized(jash_writer_t w, void* ctx, const char* s) {
    if (!w || !s) return -1;
    long total = 0;
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* run = p;     /* start of the current verbatim run */
    for (;;) {
        unsigned char c = *p;
        int safe = (c >= 0x20 && c != 0x7F && c < 0x80) ||
                   c == '\t' || c == '\n' || c == '\r';
        if (c == 0 || !safe) {
            if (p > run) {            /* flush the pending printable run */
                long r = w(ctx, (const char*)run, (size_t)(p - run));
                if (r < 0) return r;
                total += r;
            }
            if (c == 0) break;
            char ph[4];
            size_t pl;
            if (c < 0x20 || c == 0x7F) {            /* C0 / DEL -> ^X */
                ph[0] = '^';
                ph[1] = (char)(c == 0x7F ? '?' : (c + 0x40));
                pl = 2;
            } else {                                /* C1 0x80-0x9F -> \xHH */
                static const char hex[] = "0123456789abcdef";
                ph[0] = '\\'; ph[1] = 'x';
                ph[2] = hex[(c >> 4) & 0xF];
                ph[3] = hex[c & 0xF];
                pl = 4;
            }
            long r = w(ctx, ph, pl);
            if (r < 0) return r;
            total += r;
            p++;
            run = p;
        } else {
            p++;
        }
    }
    return total;
}

/* Render integer into buf (must hold ≥ 32 bytes); return chars written. */
static size_t io_render_int(char* buf, long long v, int unsigned_, int base) {
    char tmp[32];
    size_t ti = 0;
    int negative = 0;
    unsigned long long uv;
    if (!unsigned_ && v < 0) { negative = 1; uv = (unsigned long long)(-v); }
    else                     {                uv = (unsigned long long)v;    }
    if (uv == 0) tmp[ti++] = '0';
    while (uv > 0) {
        unsigned int d = (unsigned int)(uv % (unsigned)base);
        tmp[ti++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        uv /= (unsigned)base;
    }
    size_t out = 0;
    if (negative) buf[out++] = '-';
    while (ti > 0) buf[out++] = tmp[--ti];
    return out;
}

/* Handle a single %-conversion. Reads from ap, writes through (w,ctx).
 * Returns chars written (>=0) or negative on writer error.
 * Supports: width (positive integer), '-' left-align flag, '0' zero-pad. */
static long io_conv(jash_writer_t w, void* ctx, char spec,
                    int long_, int llong_, int width, int precision,
                    int left, int zero, va_list* ap) {
    char buf[64];
    size_t blen = 0;
    int    pad  = 0;
    int    skip_pad = 0;

    switch (spec) {
    case 's': {
        const char* s = va_arg(*ap, const char*);
        if (!s) s = "(null)";
        size_t slen = 0; while (s[slen]) slen++;
        long total = 0;
        /* honor precision truncation */
        size_t outlen = (precision >= 0 && (size_t)precision < slen) ? (size_t)precision : slen;
        if (width > (int)outlen) pad = width - (int)outlen;
        char pc = zero ? '0' : ' ';
        if (!left && pad > 0) {
            for (int i = 0; i < pad; i++) { long r = w(ctx, &pc, 1); if (r<0) return r; total += r; }
        }
        long r = w(ctx, s, outlen);
        if (r < 0) return r;
        total += r;
        if (left && pad > 0) {
            for (int i = 0; i < pad; i++) { long r2 = w(ctx, " ", 1); if (r2<0) return r2; total += r2; }
        }
        return total;
    }
    case 'c': {
        int c = va_arg(*ap, int);
        buf[0] = (char)c;
        return w(ctx, buf, 1);
    }
    case 'd': case 'i': {
        long long v = llong_ ? va_arg(*ap, long long)
                      : (long_ ? va_arg(*ap, long) : va_arg(*ap, int));
        blen = io_render_int(buf, v, 0, 10);
        goto emit_padded_number;
    }
    case 'u': {
        unsigned long long v = llong_ ? va_arg(*ap, unsigned long long)
                               : (long_ ? va_arg(*ap, unsigned long) : va_arg(*ap, unsigned int));
        blen = io_render_int(buf, (long long)v, 1, 10);
        goto emit_padded_number;
    }
    case 'x': {
        unsigned long long v = llong_ ? va_arg(*ap, unsigned long long)
                               : (long_ ? va_arg(*ap, unsigned long) : va_arg(*ap, unsigned int));
        blen = io_render_int(buf, (long long)v, 1, 16);
        goto emit_padded_number;
    }
#ifdef JASH_TARGET_BEYOND
    case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
        /* INV-532: Bear's sovereign va_arg has no SysV SSE save area yet.
         * Do not consume floating varargs in jash's tiny printf on Beyond. */
        const char *unsupported = "[fp]";
        while (unsupported[blen] && blen + 1 < sizeof(buf)) {
            buf[blen] = unsupported[blen];
            blen++;
        }
        goto emit_padded_number;
    }
#else
    case 'f': case 'F': {
        double v = va_arg(*ap, double);
        int prec = (precision >= 0) ? precision : 6;
        if (prec > 15) prec = 15;
        long long int_part = (long long)v;
        double frac = v - (double)int_part;
        if (frac < 0) frac = -frac;
        long long multiplier = 1;
        for (int i = 0; i < prec; i++) multiplier *= 10;
        long long dec_part = (long long)(frac * multiplier + 0.5);
        if (dec_part >= multiplier) { dec_part = 0; int_part += (int_part < 0 ? -1 : 1); }
        blen = io_render_int(buf, int_part, 0, 10);
        buf[blen++] = '.';
        char dec_buf[32];
        int dlen = io_render_int(dec_buf, dec_part, 0, 10);
        int leading = prec - dlen;
        for (int i = 0; i < leading; i++) buf[blen++] = '0';
        for (int i = 0; i < dlen; i++)   buf[blen++] = dec_buf[i];
        goto emit_padded_number;
    }
    case 'g': case 'G': {
        double v = va_arg(*ap, double);
        int prec = (precision >= 0) ? precision : 6;
        snprintf(buf, sizeof(buf), "%.*g", prec, v);
        blen = strlen(buf);
        goto emit_padded_number;
    }
#endif
    case '%':
        return w(ctx, "%", 1);
    default: {
        char dump[3] = { '%', spec, 0 };
        return w(ctx, dump, 2);
    }
    }
    skip_pad = 1;  /* unreachable; silences unused warnings */
    (void)skip_pad;

emit_padded_number: {
        long total = 0;
        if (width > (int)blen) pad = width - (int)blen;
        char pc = zero ? '0' : ' ';
        if (!left && pad > 0) {
            for (int i = 0; i < pad; i++) { long r = w(ctx, &pc, 1); if (r<0) return r; total += r; }
        }
        long r = w(ctx, buf, blen);
        if (r < 0) return r;
        total += r;
        if (left && pad > 0) {
            for (int i = 0; i < pad; i++) { long r2 = w(ctx, " ", 1); if (r2<0) return r2; total += r2; }
        }
        return total;
    }
}

long jash_writef(jash_writer_t w, void* ctx, const char* fmt, ...) {
    if (!w || !fmt) return -1;
    va_list ap;
    va_start(ap, fmt);

    long total = 0;
    const char* p = fmt;
    const char* segstart = fmt;

    while (*p) {
        if (*p != '%') { p++; continue; }

        if (p > segstart) {
            long r = w(ctx, segstart, (size_t)(p - segstart));
            if (r < 0) { va_end(ap); return r; }
            total += r;
        }
        p++;

        /* flags */
        int left = 0, zero = 0;
        while (*p == '-' || *p == '0') {
            if (*p == '-') left = 1;
            else           zero = 1;
            p++;
        }

        /* width */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* precision */
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    p++;
                }
            }
        }

        /* length */
        int long_ = 0, llong_ = 0;
        if (*p == 'l') { long_ = 1; p++; if (*p == 'l') { llong_ = 1; p++; } }

        long r = io_conv(w, ctx, *p, long_, llong_, width, precision, left, zero, &ap);
        if (r < 0) { va_end(ap); return r; }
        total += r;

        if (*p) p++;
        segstart = p;
    }

    if (p > segstart) {
        long r = w(ctx, segstart, (size_t)(p - segstart));
        if (r < 0) { va_end(ap); return r; }
        total += r;
    }

    va_end(ap);
    return total;
}
