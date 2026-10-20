// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Module: builtins/printf — format and print data
// Args: FORMAT [ARG...] — FORMAT is argv[0], rest are arguments

#include "builtin.h"
#include <core/env.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "sys/syscalls.h"

/* SECURITY: hard cap on printf field width / precision. Bounds the pad loops so
 * a hostile format like `printf '%999999999d'` cannot emit ~1e9 characters and
 * hang the shell. 64 KiB is far beyond any legitimate alignment need and still
 * emits in bounded chunks. */
#define JASH_PRINTF_MAX_WIDTH (64 * 1024)

/* Parse integer with optional 0x (hex), 0 (octal) prefix — bash printf semantics */
static unsigned long parse_uint_auto(const char *s) {
    if (!s || !*s) return 0;
    unsigned long val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            int d = -1;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            if (d < 0) break;
            val = val * 16 + d;
            s++;
        }
    } else if (s[0] == '0' && s[1] >= '0' && s[1] <= '7') {
        s++;
        while (*s >= '0' && *s <= '7') { val = val * 8 + (*s - '0'); s++; }
    } else {
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
    }
    return val;
}

static long parse_long(const char *s) {
    if (!s || !*s) return 0;
    long sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    unsigned long val = parse_uint_auto(s);
    return (long)(sign < 0 ? 0UL - val : val);
}

static unsigned long parse_ulong(const char *s, int base) {
    if (!s || !*s) return 0;
    /* If caller passes base=10 but string has 0x/0 prefix, auto-detect */
    if (base == 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' ||
                                       (s[1] >= '0' && s[1] <= '7')))
        return parse_uint_auto(s);
    unsigned long val = 0;
    while (*s) {
        int d = -1;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        if (d < 0 || d >= base) break;
        val = val * base + d;
        s++;
    }
    return val;
}

/* Buffer-writer context for printf -v: captures output into a char array. */
typedef struct { char* buf; size_t pos; size_t cap; } jash_buf_ctx_t;
static long buf_writer(void* ctx, const char* data, size_t len) {
    jash_buf_ctx_t* b = (jash_buf_ctx_t*)ctx;
    size_t written = b->pos + 1 < b->cap ? b->cap - b->pos - 1 : 0;
    if (written > len) written = len;
    if (written) memcpy(b->buf + b->pos, data, written);
    b->pos += written;
    if (b->buf && b->cap > 0) b->buf[b->pos] = '\0';
    return (long)written;
}

static void out_char(char c, jash_writer_t w, void *ctx) {
    w(ctx, &c, 1);
}

static void out_str(const char *s, jash_writer_t w, void *ctx) {
    if (s) w(ctx, s, (unsigned)strlen(s));
}

/* ADR-0036 W3: fuzz-found DoS — jash_stdout_writer does one raw sys_write()
 * per call, so `printf '%999999999d' 1` (width already clamped to 64KiB by
 * JASH_PRINTF_MAX_WIDTH below) still issued 64K one-byte syscalls, taking
 * ~4 seconds for a single command. Batch padding into chunks instead of
 * one out_char() per padding character — same clamp, same output, O(1)
 * writer calls instead of O(width). */
static void out_pad(char c, int count, jash_writer_t w, void *ctx) {
    if (count <= 0) return;
    char buf[4096];
    size_t fill = count < (int)sizeof(buf) ? (size_t)count : sizeof(buf);
    memset(buf, c, fill);
    while (count > 0) {
        int chunk = count < (int)sizeof(buf) ? count : (int)sizeof(buf);
        w(ctx, buf, (unsigned)chunk);
        count -= chunk;
    }
}

static void out_ulong(unsigned long val, int base, int upper,
                       jash_writer_t w, void *ctx) {
    char num[24]; int ni = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (val == 0) { num[ni++] = '0'; }
    else {
        char rev[24]; int ri = 0;
        while (val > 0) { rev[ri++] = dig[val % base]; val /= base; }
        while (ri > 0) num[ni++] = rev[--ri];
    }
    num[ni] = 0;
    out_str(num, w, ctx);
}

static int printf_impl(jash_state_t *s, jash_writer_t w, void *ctx,
                   int in_argc, char **in_argv) {
    if (in_argc == 0) { out_char('\n', w, ctx); return 0; }

    /* The dispatcher supplies quote-aware argv. The format is one complete
     * argument; later arguments containing '%' or escapes are always data. */
    /* JINV-011 — printf nao pode truncar nem descartar argumento em silencio.
     *
     * Era `char argv_buf[64][256]` + strlcpy: TODO argumento acima de 255
     * bytes saia cortado, e do 64o em diante era descartado — os dois em
     * SILENCIO, com rc=0. Reproduzido em 2026-07-27: `printf '%s\n'` com uma
     * string de 300 bytes imprimia 255; com 99 argumentos imprimia 63 linhas.
     * Para um agente que monta JSON/payload com printf, e corrupcao de dados
     * indistinguivel de sucesso.
     *
     * Os 20 usos de argv_buf sao SOMENTE LEITURA (verificado um a um), entao a
     * copia nunca foi necessaria: aponta-se direto para in_argv. O teto de 255
     * bytes DESAPARECE. O teto de contagem vira erro ALTO em vez de descarte
     * mudo. Gate: tests/test_jinv011_printf_ls_args.sh
     */
    /* ═══ JINV-076 — o teto de 1024 argumentos, e a recusa no STDOUT ═══════
     *
     * O JINV-011 acertou o diagnostico (nada de descarte mudo) e parou no meio
     * do caminho: trocou o descarte por uma RECUSA, e mandou a recusa pelo
     * writer do COMANDO — isto e, pelo stdout do printf. Duas coisas erradas
     * numa linha so:
     *
     *  1. O teto era arbitrario. `printf 'a%.0s' $(seq 1 1024)` recusava;
     *     com 1023 funcionava (cliff bissetado). Nada no algoritmo precisa de
     *     teto: `argv_buf` so e LIDO, nunca escrito.
     *
     *  2. A recusa ia para o stdout. A mensagem tem 43 bytes — e 43 era
     *     exatamente o que `wc -c` media do outro lado do cano. Um pipeline
     *     recebia *texto de erro* como se fosse dado, com o formato do
     *     comando. E a mesma classe de corrupcao silenciosa que o JINV-011
     *     dizia estar matando: diagnostico misturado com dado.
     *
     * O conserto e o que o proprio comentario acima ja prescrevia e nao foi
     * feito: apontar direto para `in_argv`. Sem copia, sem array na pilha,
     * sem teto, sem mensagem — e portanto sem mensagem no lugar errado.
     * `argv_buf` continua sendo o nome usado nos ~20 acessos abaixo; muda
     * so o que ele e (um alias em vez de uma copia).
     *
     * Gate: tests/test_jinv076_printf_no_arg_ceiling.sh                    */
    const char* const* argv_buf = (const char* const*)in_argv;
    int argc = in_argc;

    /* printf -v VAR FORMAT [ARGS...]: capture output into variable VAR.
     * When -v is present, redirect through a buffer writer then store. */
    char vname[128]; vname[0] = '\0';
    int vstart = 0;
    if (argc >= 3 && argv_buf[0][0] == '-' && argv_buf[0][1] == 'v' && argv_buf[0][2] == '\0') {
        /* -v VAR FORMAT ... */
        strlcpy(vname, argv_buf[1], sizeof(vname));
        vname[sizeof(vname) - 1] = '\0';
        vstart = 2;  /* format starts at argv[2] */
    }

    /* BUG-160: `--` ends option processing — the NEXT token is the FORMAT, even
     * if it begins with '-' (`printf -- '-n'` prints `-n`). GNU/POSIX behavior;
     * jash printf used to take `--` itself as the format string. */
    if (vstart < argc && strcmp(argv_buf[vstart], "--") == 0) {
        vstart++;
    }
    if (vstart >= argc) {
        jash_writes(jash_stderr_writer, 0, "jash: printf: missing format\n");
        return 2;
    }

    /* Redirect writer to a buffer when -v is active */
    char vbuf[4096]; vbuf[0] = '\0';
    jash_buf_ctx_t vbctx = { vbuf, 0, sizeof(vbuf) };
    if (vname[0]) {
        w   = (jash_writer_t)buf_writer;
        ctx = &vbctx;
    }

    const char *fmt = argv_buf[vstart];
    int ai = vstart + 1;

    /* POSIX: reuse the format as often as needed to consume all arguments.
     * Stop when a full pass consumes no argument (e.g. format has no specifiers). */
    int prev_ai;
    do {
    prev_ai = ai;
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '\\') {
            i++;
            switch (fmt[i]) {
                case 'n': out_char('\n', w, ctx); break;
                case 't': out_char('\t', w, ctx); break;
                case 'r': out_char('\r', w, ctx); break;
                case '\\': out_char('\\', w, ctx); break;
                case '"': out_char('"', w, ctx); break;
                case '\'': out_char('\'', w, ctx); break;
                case 'a': out_char('\a', w, ctx); break;
                case 'b': out_char('\b', w, ctx); break;
                case 'f': out_char('\f', w, ctx); break;
                case 'v': out_char('\v', w, ctx); break;
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    /* BUG-158: format-string octal is `\ooo` — up to THREE
                     * octal digits, the first digit inclusive (bash/POSIX).
                     * The old code keyed only on a leading `\0` and then read
                     * unbounded digits, so `\101` was an unknown escape and
                     * `\0101` over-read into `A`. Now: `\101`→'A', `\0101`→
                     * \b+'1', `\7`→BEL, `\0`→NUL. (`%b` args keep `\0ddd`.) */
                    int val = fmt[i] - '0';
                    int nd = 1;
                    while (nd < 3 && fmt[i+1] >= '0' && fmt[i+1] <= '7') {
                        val = val * 8 + (fmt[++i] - '0');
                        nd++;
                    }
                    out_char((char)val, w, ctx);
                    break;
                }
                case 'x': {
                    int val = 0, nd = 0;
                    while (nd < 2 && isxdigit((unsigned char)fmt[i+1])) {
                        i++;
                        val = val * 16 + ((fmt[i] >= '0' && fmt[i] <= '9') ? fmt[i] - '0' :
                                          (fmt[i] >= 'a' && fmt[i] <= 'f') ? fmt[i] - 'a' + 10 :
                                                                              fmt[i] - 'A' + 10);
                        nd++;
                    }
                    if (nd) out_char((char)val, w, ctx);
                    break;
                }
                case 'e': case 'E': out_char('\x1B', w, ctx); break;
                default:
                    /* BUG-155: an UNKNOWN escape is not an escape — bash emits
                     * the backslash literally, then the char (`\y` → `\y`, not
                     * `y`). A trailing lone backslash emits just '\' without
                     * scanning past the terminating NUL. */
                    out_char('\\', w, ctx);
                    if (fmt[i] == '\0') { i--; break; }
                    out_char(fmt[i], w, ctx);
                    break;
            }
            continue;
        }
        if (fmt[i] == '%' && fmt[i+1]) {
            i++;
            
            /* Parse format specifier: %[flags][width][.precision]type */
            int left_align = 0, pad_zero = 0;
            int width = 0, precision = -1;
            
            /* Parse flags */
            while (fmt[i] == '-' || fmt[i] == '0') {
                if (fmt[i] == '-') left_align = 1;
                else if (fmt[i] == '0') pad_zero = 1;
                i++;
            }
            
            /* Parse width */
            if (fmt[i] == '*') {
                width = (ai < argc) ? (int)parse_long(argv_buf[ai++]) : 0;
                i++;
            } else {
                while (fmt[i] >= '0' && fmt[i] <= '9') {
                    width = width * 10 + (fmt[i] - '0');
                    if (width > JASH_PRINTF_MAX_WIDTH) width = JASH_PRINTF_MAX_WIDTH;
                    i++;
                }
            }
            
            /* Parse precision */
            if (fmt[i] == '.') {
                i++;
                if (fmt[i] == '*') {
                    precision = (ai < argc) ? (int)parse_long(argv_buf[ai++]) : 0;
                    i++;
                } else {
                    precision = 0;
                    while (fmt[i] >= '0' && fmt[i] <= '9') {
                        precision = precision * 10 + (fmt[i] - '0');
                        if (precision > JASH_PRINTF_MAX_WIDTH) precision = JASH_PRINTF_MAX_WIDTH;
                        i++;
                    }
                }
            }

            /* SECURITY: clamp field width/precision. An unbounded width such as
             * `printf '%999999999d'` would otherwise emit ~1e9 pad characters,
             * a single-command DoS. The clamp keeps legitimate alignment intact
             * (no real format needs > 1MiB) while making the pathological case
             * terminate. Also catches negative values from int overflow or a
             * negative `*` argument. F E R M I ∞ H A R T */
            if (width < 0 || width > JASH_PRINTF_MAX_WIDTH) {
                if (width < 0) { left_align = 1; width = -width; }
                if (width > JASH_PRINTF_MAX_WIDTH) width = JASH_PRINTF_MAX_WIDTH;
            }
            if (precision > JASH_PRINTF_MAX_WIDTH) precision = JASH_PRINTF_MAX_WIDTH;
            
            switch (fmt[i]) {
                case 's': {
                    const char *s_val = (ai < argc) ? argv_buf[ai++] : "";
                    int slen = (int)strlen(s_val);
                    
                    /* Apply precision (max length) */
                    if (precision >= 0 && slen > precision) slen = precision;
                    
                    /* Apply width and alignment */
                    int padding = (width > slen) ? width - slen : 0;
                    if (!left_align) {
                        out_pad(' ', padding, w, ctx);
                    }
                    if (slen) w(ctx, s_val, (size_t)slen);
                    if (left_align) {
                        out_pad(' ', padding, w, ctx);
                    }
                    break;
                }
                case 'd': case 'i': {
                    if (ai < argc) {
                        long val = parse_long(argv_buf[ai++]);
                        char num[24];
                        int ni = 0;
                        int is_negative = val < 0;
                        /* Negate unsigned so LONG_MIN has a valid magnitude. */
                        unsigned long magnitude = is_negative ? 0UL - (unsigned long)val : (unsigned long)val;
                        
                        if (magnitude == 0) {
                            num[ni++] = '0';
                        } else {
                            char rev[24]; int ri = 0;
                            while (magnitude > 0) { rev[ri++] = (char)('0' + magnitude % 10); magnitude /= 10; }
                            while (ri > 0) num[ni++] = rev[--ri];
                        }
                        num[ni] = '\0';
                        
                        /* Apply width and alignment */
                        int num_len = ni + (is_negative ? 1 : 0);
                        int padding = (width > num_len) ? width - num_len : 0;
                        
                        if (!left_align && !pad_zero) {
                            out_pad(' ', padding, w, ctx);
                        }
                        if (is_negative) out_char('-', w, ctx);
                        if (!left_align && pad_zero) {
                            out_pad('0', padding, w, ctx);
                        }
                        w(ctx, num, (size_t)ni);
                        if (left_align) {
                            out_pad(' ', padding, w, ctx);
                        }
                    }
                    break;
                }
                case 'b':
                    if (ai < argc) {
                        const char *sp = argv_buf[ai++];
                        int b_stop = 0;
                        while (*sp && !b_stop) {
                            if (*sp == '\\' && sp[1]) {
                                sp++;
                                switch (*sp) {
                                    case 'a': out_char('\a', w, ctx); break;
                                    case 'b': out_char('\b', w, ctx); break;
                                    case 'c': b_stop = 1; break;
                                    case 'e': case 'E': out_char('\x1B', w, ctx); break;
                                    case 'f': out_char('\f', w, ctx); break;
                                    case 'n': out_char('\n', w, ctx); break;
                                    case 'r': out_char('\r', w, ctx); break;
                                    case 't': out_char('\t', w, ctx); break;
                                    case 'v': out_char('\v', w, ctx); break;
                                    case '\\': out_char('\\', w, ctx); break;
                                    case '0': case '1': case '2': case '3':
                                    case '4': case '5': case '6': case '7': {
                                        /* BUG-159: %b octal accepts BOTH forms
                                         * bash does: `\0ddd` (0 prefix + up to 3
                                         * octal digits) AND `\ddd` (up to 3, the
                                         * first digit inclusive). `\0101`→'A',
                                         * `\101`→'A', `\41`→'!'. The old code
                                         * keyed only on `\0` and read unbounded
                                         * digits, so `\101` fell through as an
                                         * unknown escape. */
                                        int val, nd;
                                        if (*sp == '0') { val = 0; nd = 0; }
                                        else            { val = *sp - '0'; nd = 1; }
                                        while (nd < 3 && sp[1] >= '0' && sp[1] <= '7') {
                                            val = val * 8 + (*++sp - '0');
                                            nd++;
                                        }
                                        out_char((char)val, w, ctx);
                                        break;
                                    }
                                    case 'x': {
                                        int val = 0, nd = 0;
                                        while (nd < 2 && isxdigit((unsigned char)sp[1])) {
                                            sp++;
                                            val = val * 16 +
                                                ((*sp >= '0' && *sp <= '9') ? *sp - '0' :
                                                 (*sp >= 'a' && *sp <= 'f') ? *sp - 'a' + 10 :
                                                                              *sp - 'A' + 10);
                                            nd++;
                                        }
                                        out_char((char)val, w, ctx);
                                        break;
                                    }
                                    /* BUG-155: unknown escape keeps its
                                     * backslash (`%b` of `a\yb` → `a\yb`). */
                                    default: out_char('\\', w, ctx); out_char(*sp, w, ctx); break;
                                }
                            } else {
                                const char* start = sp++;
                                while (*sp && *sp != '\\') sp++;
                                w(ctx, start, (size_t)(sp - start));
                                continue;
                            }
                            if (*sp && !b_stop) sp++;
                        }
                    }
                    break;
                case 'u':
                    if (ai < argc) {
                        out_ulong(parse_ulong(argv_buf[ai++], 10), 10, 0, w, ctx);
                    }
                    break;
                case 'x':
                    if (ai < argc) {
                        out_ulong(parse_ulong(argv_buf[ai++], 10), 16, 0, w, ctx);
                    }
                    break;
                case 'X':
                    if (ai < argc) {
                        out_ulong(parse_ulong(argv_buf[ai++], 10), 16, 1, w, ctx);
                    }
                    break;
                case 'o':
                    if (ai < argc) {
                        out_ulong(parse_ulong(argv_buf[ai++], 10), 8, 0, w, ctx);
                    }
                    break;
                case 'c':
                    if (ai < argc) {
                        out_char(argv_buf[ai][0], w, ctx);
                        ai++;
                    }
                    break;
                case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                    /* Was: dumps the raw argument string verbatim, ignoring
                     * width/precision/flags entirely — `printf "%5.2f" 3.14159`
                     * printed "3.14159" instead of " 3.14". Parse the arg as a
                     * double and let libc's snprintf do the actual float
                     * formatting (precision only — width/alignment use the
                     * same manual padding loop as %s/%d below so a pathological
                     * width like 64KB can't blow a fixed-size sprintf buffer). */
                    const char* fv = (ai < argc) ? argv_buf[ai++] : "0";
                    double dval = strtod(fv, NULL);
                    char numspec[16];
                    int si = 0;
                    numspec[si++] = '%';
                    if (precision >= 0) {
                        int pl = snprintf(numspec + si, sizeof(numspec) - si, ".%d", precision);
                        if (pl > 0) si += pl;
                    }
                    if (si < (int)sizeof(numspec) - 1) numspec[si++] = (char)fmt[i];
                    numspec[si] = '\0';

                    char numbuf[128];
                    /* numspec is a format spec this printf builtin REBUILDS
                     * from the user's directive (e.g. "%.3f") — a dynamic
                     * format string is the essence of printf, not a bug.
                     * Suppress locally, not globally (W6 keeps -Wformat=2). */
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wformat-nonliteral"
                    int numlen = snprintf(numbuf, sizeof(numbuf), numspec, dval);
                    #pragma GCC diagnostic pop
                    if (numlen < 0) numlen = 0;
                    if (numlen >= (int)sizeof(numbuf)) numlen = (int)sizeof(numbuf) - 1;

                    int padding = (width > numlen) ? width - numlen : 0;
                    if (!left_align) {
                        char padc = pad_zero ? '0' : ' ';
                        out_pad(padc, padding, w, ctx);
                    }
                    if (numlen) w(ctx, numbuf, (size_t)numlen);
                    if (left_align) {
                        out_pad(' ', padding, w, ctx);
                    }
                    break;
                }
                case '%': out_char('%', w, ctx); break;
                case 'p':
                    if (ai < argc) {
                        out_str("0x", w, ctx);
                        out_ulong((unsigned long)(uintptr_t)argv_buf[ai++], 16, 0, w, ctx);
                    }
                    break;
                case 'q': {
                    /* Shell-quote: emit string safe for reuse as shell input.
                     * Uses single-quoting with '\'' escapes for embedded quotes. */
                    const char* qv = (ai < argc) ? argv_buf[ai++] : "";
                    if (!*qv) {
                        out_char('\'', w, ctx); out_char('\'', w, ctx);
                        break;
                    }
                    int needs_quote = 0;
                    for (const char* qp = qv; *qp && !needs_quote; qp++) {
                        char qc = *qp;
                        if (!((qc >= 'a' && qc <= 'z') || (qc >= 'A' && qc <= 'Z') ||
                              (qc >= '0' && qc <= '9') ||
                              qc == '_' || qc == '-' || qc == '.' || qc == '/' ||
                              qc == '@' || qc == '=' || qc == '+' || qc == ':' || qc == ','))
                            needs_quote = 1;
                    }
                    if (!needs_quote) {
                        out_str(qv, w, ctx);
                    } else {
                        out_char('\'', w, ctx);
                        for (const char* qp = qv; *qp; qp++) {
                            if (*qp == '\'') {
                                w(ctx, "'\\''", 4);
                            } else {
                                const char* start = qp;
                                while (qp[1] && qp[1] != '\'') qp++;
                                w(ctx, start, (size_t)(qp - start + 1));
                            }
                        }
                        out_char('\'', w, ctx);
                    }
                    break;
                }
                default:
                    out_char('%', w, ctx);
                    out_char(fmt[i], w, ctx);
                    break;
            }
            continue;
        }
        int start = i;
        while (fmt[i+1] && fmt[i+1] != '%' && fmt[i+1] != '\\') i++;
        w(ctx, fmt + start, (size_t)(i - start + 1));
    }
    } while (ai < argc && ai != prev_ai);
    /* Store into variable when -v VAR was used */
    if (vname[0] && s) jash_env_set(s, vname, vbuf);
    return 0;
}

/* Output helpers emit through a callback; its failure must reach the builtin
 * status even when stdout was redirected by an enclosing function. */
typedef struct {
    jash_writer_t writer;
    void* ctx;
    int failed;
} printf_output_t;

/* Batching writer: printf_impl emits per byte (escapes) and per run
 * (literals), which used to reach the raw writer — up to 7 syscalls for
 * ~8 bytes of output. Accumulate into an 8 KiB stack buffer and emit in
 * chunks plus a final flush: identical bytes, identical truncation point
 * for -v, identical short-write failure. The only observable difference
 * is write() chunking (stdio buffers the same way). */
typedef struct {
    jash_writer_t w;
    void* ctx;
    char buf[8192];
    size_t used;
    int failed;
} printf_batch_t;

static long printf_batch_write(void* ctx, const char* data, size_t len) {
    printf_batch_t* b = (printf_batch_t*)ctx;
    if (b->failed) return -1;
    size_t orig = len;
    while (len > 0) {
        size_t room = sizeof b->buf - b->used;
        if (room == 0) {
            long n = b->w(b->ctx, b->buf, b->used);
            if (n < 0 || (size_t)n != b->used) { b->failed = 1; return -1; }
            b->used = 0;
            continue;
        }
        size_t take = len < room ? len : room;
        memcpy(b->buf + b->used, data, take);
        b->used += take;
        data += take;
        len -= take;
    }
    return b->failed ? -1 : (long)orig;
}

static int printf_batch_flush(printf_batch_t* b) {
    if (b->failed || b->used == 0) return b->failed ? -1 : 0;
    long n = b->w(b->ctx, b->buf, b->used);
    if (n < 0 || (size_t)n != b->used) { b->failed = 1; return -1; }
    b->used = 0;
    return 0;
}

static long printf_checked_write(void* ctx, const char* data, size_t len) {
    printf_output_t* out = ctx;
    if (out->failed) return -1;
    size_t done = 0;
    while (done < len) {
        long n = out->writer(out->ctx, data + done, len - done);
        if (n <= 0 || (size_t)n > len - done) {
            out->failed = 1;
            return -1;
        }
        done += (size_t)n;
    }
    return (long)len;
}

int jash_b_printf(jash_state_t* s, jash_writer_t w, void* ctx,
                   int argc, char** argv) {
    printf_output_t out = { w, ctx, 0 };
    printf_batch_t batch = { printf_checked_write, &out, {0}, 0, 0 };
    int rc = printf_impl(s, printf_batch_write, &batch, argc, argv);
    if (printf_batch_flush(&batch) != 0) out.failed = 1;
    if (out.failed) {
        jash_writes(jash_stderr_writer, 0, "jash: printf: write error\n");
        if (rc == 0) rc = 1;
    }
    return rc;
}
