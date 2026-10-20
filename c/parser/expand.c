// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/expand — tilde / brace / glob word expansion.

#include "expand.h"
#include "cmdsub.h"   /* JINV-096: jash_meta_shield — a porta unica de blindagem */
#include <stdio.h>
#include <core/env.h>
#include <core/glob.h>
#include <sys/syscalls.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>   /* JINV-118: DT_UNKNOWN / symlink fallback for is-dir */
#include <core/argvbuf.h>  /* JINV-132: the one growable list — no 256 ceiling */

/* ── small helpers ─────────────────────────────────────────────────── */

static int is_word_boundary(char c) {
    return c == ' ' || c == '\t' || c == '\0';
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ── stage 1: tilde expansion ──────────────────────────────────────────
 * A leading ~ at the start of a word expands to $HOME. Also supports
 * ~user → lookup user home. Single-user OS default: ~root → /root,
 * ~guest → /home/guest. */


/* JINV-040 — o til expande DENTRO de uma atribuicao.
 *
 * POSIX 2.6.1: numa atribuicao de variavel o til e expandido depois do `=` e
 * depois de cada `:` nao citado. E como todo mundo escreve caminho:
 *
 *   V=~/bin              bash -> /Users/admin/bin    jash -> ~/bin
 *   PATH=~/a:~/b:$PATH   bash -> expandido           jash -> literal
 *
 * `jash_expand_tilde` so considerava o til elegivel em INICIO DE PALAVRA
 * (depois de espaco/tab), e num `V=~` ele vem depois do `=`. O resultado ficava
 * com um `~` literal — que so falha quando alguem USA o caminho, longe da
 * causa. Como argumento (`echo ~`, `cd ~`) sempre funcionou; a divergencia
 * entre os dois usos foi o que denunciou.
 *
 * Diz se a posicao `p` (dentro de `line`) e um ponto de atribuicao onde o til
 * e elegivel: exige um NOME valido seguido de `=` no inicio da palavra, e
 * entao aceita logo apos aquele `=` ou apos qualquer `:` seguinte. */
static int tilde_assign_point(const char* line, const char* p)
{
    if (p <= line) return 0;
    char prev = p[-1];
    if (prev != '=' && prev != ':') return 0;

    /* Recua ate o inicio da palavra. */
    const char* w = p - 1;
    while (w > line && w[-1] != ' ' && w[-1] != '\t' && w[-1] != '\n') w--;

    /* A palavra tem que comecar com NOME= (letra ou _, depois alnum ou _). */
    if (!((*w >= 'A' && *w <= 'Z') || (*w >= 'a' && *w <= 'z') || *w == '_'))
        return 0;
    const char* q = w + 1;
    while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
           (*q >= '0' && *q <= '9') || *q == '_') q++;
    if (*q != '=') return 0;
    return q < p;   /* o `=` do nome vem ANTES da posicao do til */
}


/* JINV-041 — til no VALOR de uma atribuicao.
 *
 * O JINV-040 fez `export V=~/bin` funcionar: aquele caminho passa pelo
 * expansor de palavra, que enxerga a linha inteira (`V=~/bin`) e reconhece o
 * ponto de atribuicao. A atribuicao SIMPLES (`V=~/bin`) e tratada por outro
 * caminho em eval.c, que recebe so o VALOR ja separado (`~/bin`) — ali nao ha
 * `NAME=` para reconhecer.
 *
 * Nesse contexto a regra POSIX 2.6.1 e mais simples: no valor de uma
 * atribuicao, o til e expandido no INICIO e depois de cada `:` nao citado. E
 * o que faz `PATH=~/bin:~/opt/bin` funcionar.
 *
 * Devolve `val` inalterado quando nao ha nada a fazer.
 * Gate: tests/test_jinv040_tilde_assign.sh */
const char* jash_expand_tilde_value(jash_state_t* s, const char* val,
                                    jash_expand_ctx_t* ctx)
{
    if (!val || !ctx) return val;
    /* Ha til elegivel? (inicio, ou logo apos ':') */
    int achou = 0;
    for (const char* q = val; *q; q++) {
        if (*q != '~') continue;
        if (q != val && q[-1] != ':') continue;
        if (q[1] == '\0' || q[1] == '/' || q[1] == ':') { achou = 1; break; }
    }
    if (!achou) return val;

    const char* home = s ? jash_env_get(s, "HOME") : 0;
    if (!home || !*home) return val;
    size_t hl = strlen(home);

    char* out = ctx->tilde_out;
    size_t cap = sizeof(ctx->tilde_out);
    size_t o = 0;
    const char* q = val;
    while (*q && o + 1 < cap) {
        int elegivel = (q == val || q[-1] == ':');
        if (*q == '~' && elegivel &&
            (q[1] == '\0' || q[1] == '/' || q[1] == ':')) {
            if (o + hl + 1 >= cap) break;      /* nao coube: para de expandir */
            memcpy(out + o, home, hl); o += hl;
            q++;
            continue;
        }
        out[o++] = *q++;
    }
    out[o] = '\0';
    return out;
}

const char* jash_expand_tilde(jash_state_t* s, const char* line, jash_expand_ctx_t* ctx) {
    if (!line) return line;
    /* Quick check: any '~' that is a word-leading char? */
    int found = 0;
    {
        const char* p = line;
        int at_word_start = 1;
        int sq = 0, dq = 0;
        while (*p) {
            if (sq) { if (*p == '\'') sq = 0; p++; at_word_start = 0; continue; }
            if (dq) { if (*p == '"') dq = 0; p++; at_word_start = 0; continue; }
            if (*p == '\'') { sq = 1; p++; at_word_start = 0; continue; }
            if (*p == '"')  { dq = 1; p++; at_word_start = 0; continue; }
            if (*p == ' ' || *p == '\t') { at_word_start = 1; p++; continue; }
            if (*p == '~' && (at_word_start || tilde_assign_point(line, p)) &&
                (is_word_boundary(p[1]) || p[1] == '/')) {
                found = 1; break;   /* JINV-040 */
            }
            at_word_start = 0;
            p++;
        }
    }
    if (!found) return line;

    const char* home = s ? jash_env_get(s, "HOME") : 0;
    if (!home || !*home) home = "/root";

    /* ADR-0036 W4: `out` used to be `static char out[4096]`. This macro
     * keeps every `out[...]`/`sizeof(out)` reference below byte-identical
     * while the storage moves to caller-owned ctx (pure move, no logic
     * change) — undef'd before the function returns. */
    #define out ctx->tilde_out
    unsigned o = 0;
    const char* p = line;
    int at_word_start = 1;
    int sq = 0, dq = 0;
    while (*p && o + 1 < sizeof(out)) {
        if (sq) {
            if (*p == '\'') sq = 0;
            out[o++] = *p++; at_word_start = 0; continue;
        }
        if (dq) {
            if (*p == '"') dq = 0;
            out[o++] = *p++; at_word_start = 0; continue;
        }
        if (*p == '\'') { sq = 1; out[o++] = *p++; at_word_start = 0; continue; }
        if (*p == '"')  { dq = 1; out[o++] = *p++; at_word_start = 0; continue; }
        if (*p == ' ' || *p == '\t') { out[o++] = *p++; at_word_start = 1; continue; }

        if (*p == '~' && (at_word_start || tilde_assign_point(line, p))) {  /* JINV-040 */
            /* ~ alone or ~/... → $HOME */
            if (is_word_boundary(p[1]) || p[1] == '/') {
                const char* h = home;
                while (*h && o + 1 < sizeof(out)) out[o++] = *h++;
                p++;
                at_word_start = 0;
                continue;
            }
            /* ~user → lookup home directory */
            if (p[1] >= 'a' && p[1] <= 'z') {
                const char* uname = p + 1;
                const char* uend = uname;
                while (*uend && (unsigned char)*uend > ' ' && *uend != '/') uend++;
                /* Check known usernames */
                int ulen = (int)(uend - uname);
                const char* user_home = 0;
                if (ulen == 4 && memcmp(uname, "root", 4) == 0) user_home = "/root";
                else if (ulen == 5 && memcmp(uname, "guest", 5) == 0) user_home = "/home/guest";
                else if (ulen == 3 && memcmp(uname, "bin", 3) == 0) user_home = "/bin";
                if (user_home) {
                    while (*user_home && o + 1 < sizeof(out)) out[o++] = *user_home++;
                }
                p = uend;
                at_word_start = 0;
                continue;
            }
        }
        out[o++] = *p++;
        at_word_start = 0;
    }
    out[o] = 0;
    return out;
    #undef out
}

/* ── stage 2: brace expansion ──────────────────────────────────────────
 * Supports:
 *   {a,b,c}      comma list
 *   {1..9}       numeric range (ascending or descending)
 * A brace group expands combinatorially with its surrounding prefix and
 * the rest of the WORD (up to the next whitespace). One level of
 * nesting is handled by recursion on the rewritten line. */

/* Parse an int from p; return end pointer, store value. */
static const char* parse_int(const char* p, int* out, int* ok) {
    int v = 0, neg = 0, n = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; n++; }
    *ok = (n > 0);
    *out = neg ? -v : v;
    return p;
}

#define BRACE_IS_ALPHA(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z'))

/* A sequence expression L..R[..STEP]. Valid iff L and R are BOTH integers
 * (an optional non-zero integer step is allowed — jash supports numeric step
 * ranges like bash 4) OR BOTH single letters (NO step — jash has no alpha
 * step expander, so `{a..e..2}` stays literal, matching bash 3.2). A
 * mixed-type range ({a..3}) or an incomplete one ({a..}) is never valid and
 * stays literal. [s,e) is the text between the braces. BUG-142/143. */
static int is_valid_brace_range(const char* s, const char* e) {
    const char* d1 = 0;
    for (const char* q = s; q + 1 < e; q++)
        if (q[0] == '.' && q[1] == '.') { d1 = q; break; }
    if (!d1 || d1 == s) return 0;              /* need a non-empty left operand */
    const char* rstart = d1 + 2;
    if (rstart >= e) return 0;                 /* need a non-empty right operand */
    const char* d2 = 0;                        /* optional step ".." */
    for (const char* q = rstart; q + 1 < e; q++)
        if (q[0] == '.' && q[1] == '.') { d2 = q; break; }
    const char* rend = d2 ? d2 : e;
    if (rend <= rstart) return 0;
    if (d2) {                                  /* step must be a non-zero int */
        const char* sstart = d2 + 2;
        int oks = 0, vs;
        const char* se = parse_int(sstart, &vs, &oks);
        if (sstart >= e || !oks || se != e || vs == 0) return 0;
    }
    int okL = 0, okR = 0, vv;
    const char* le = parse_int(s, &vv, &okL);
    const char* re = parse_int(rstart, &vv, &okR);
    if (okL && le == d1 && okR && re == rend) return 1;            /* int..int[..step] */
    if (!d2 && (d1 - s) == 1 && (e - rstart) == 1 &&
        BRACE_IS_ALPHA(*s) && BRACE_IS_ALPHA(*rstart)) return 1;   /* a..z (no step)  */
    return 0;
}

/* Find the matching '}' for the '{' at `open`, honoring nesting.
 * Returns pointer to '}' or NULL. */
static const char* match_brace(const char* open) {
    int depth = 0;
    const char* p = open;
    while (*p) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p; }
        p++;
    }
    return 0;
}

/* Locate the first top-level brace group in `line` that is a valid
 * expansion ({a,b} or {N..M}). Sets *wstart/ *wend to the enclosing
 * word boundaries, *bopen/ *bclose to the brace chars. Returns 1 if
 * found. */
static int find_brace_group(const char* line, const char** wstart,
                            const char** bopen, const char** bclose,
                            const char** wend) {
    const char* p = line;
    int sq = 0, dq = 0;
    while (*p) {
        if (sq) { if (*p == '\'') sq = 0; p++; continue; }
        if (dq) { if (*p == '"') dq = 0; p++; continue; }
        if (*p == '\\' && p[1]) { p += 2; continue; }  /* BUG-144: \{ is not a brace */
        if (*p == '\'') { sq = 1; p++; continue; }
        if (*p == '"')  { dq = 1; p++; continue; }
        if (*p == '{') {
            const char* close = match_brace(p);
            if (close && close > p + 1) {
                /* Expandable iff it holds a real top-level ',' OR a valid
                 * sequence range — and contains NO unquoted whitespace (a
                 * space splits the word, so `{a, b}` is literal). Escaped
                 * chars inside are skipped so `\,` is not a separator. */
                int has_comma = 0, has_dots = 0, has_ws = 0, d = 0;
                for (const char* q = p + 1; q < close; q++) {
                    if (*q == '\\' && q + 1 < close) { q++; continue; }   /* BUG-145 */
                    if (*q == '{') d++;
                    else if (*q == '}') d--;
                    else if (*q == ' ' || *q == '\t' || *q == '\n') has_ws = 1;  /* BUG-146 */
                    else if (d == 0 && *q == ',') has_comma = 1;
                    else if (d == 0 && q[0] == '.' && q[1] == '.') has_dots = 1;
                }
                int expandable = !has_ws &&
                    (has_comma || (has_dots && is_valid_brace_range(p + 1, close)));
                if (expandable) {
                    /* find word start: back up to ws or line start */
                    const char* ws = p;
                    while (ws > line && !is_word_boundary(ws[-1])) ws--;
                    /* find word end: forward from close to ws */
                    const char* we = close + 1;
                    while (*we && !is_word_boundary(*we)) we++;
                    *wstart = ws; *bopen = p; *bclose = close; *wend = we;
                    return 1;
                }
            }
        }
        p++;
    }
    return 0;
}

/* Append one expanded brace item (pre + body + suf) to `out`.
 * Returns the new write offset. *first is cleared after first use and
 * controls the separating space. A standalone function (not a macro)
 * to avoid identifier-capture bugs. */
static unsigned emit_brace_item(char* out, unsigned cap, unsigned o,
                                int* first,
                                const char* pre, unsigned pre_len,
                                const char* body, unsigned body_len,
                                const char* suf, unsigned suf_len) {
    /* bash 3.2: an empty list element with no prefix/suffix is dropped
     * entirely ({a,,b} → "a b"); with an affix it yields pre+suf
     * (x{a,,b}y → "xay xy xby"). Skip the wholly-empty item, leaving
     * *first untouched so spacing stays correct. */
    if (pre_len == 0 && body_len == 0 && suf_len == 0) return o;
    if (!*first && o + 1 < cap) out[o++] = ' ';
    *first = 0;
    for (unsigned i = 0; i < pre_len && o + 1 < cap; i++)  out[o++] = pre[i];
    for (unsigned i = 0; i < body_len && o + 1 < cap; i++) out[o++] = body[i];
    for (unsigned i = 0; i < suf_len && o + 1 < cap; i++)  out[o++] = suf[i];
    return o;
}

const char* jash_expand_braces(const char* line, jash_expand_ctx_t* ctx) {
    if (!line) return line;

    /* ADR-0036 W4: inbuf/out moved from `static char` to caller-owned ctx —
     * pure move, see jash_expand_tilde's comment.
     *
     * PERF (audit F4): probe for an expandable group BEFORE copying the
     * whole line into inbuf (the same probe-first pattern jash_expand_tilde
     * uses) — a line without braces, by far the common case, no longer pays
     * a 32KB memcpy per recursion level. The private copy is still required
     * once a group is found, so the output buffer below can never alias the
     * input during the recursive tail-call (unchanged rationale/behavior). */
    #define inbuf ctx->brace_inbuf
    #define out ctx->brace_out

    const char *wstart, *bopen, *bclose, *wend;
    const char* src;
    if (strlen(line) + 1 > sizeof(inbuf)) {
        /* Oversized input (only reachable from callers that bypass
         * jash_expand_word_layer's line gate, e.g. huge for-loop word
         * lists): keep the old order — copy the truncated prefix first,
         * then probe INSIDE it, so every pointer stays within inbuf. */
        unsigned i = 0;
        while (line[i] && i + 1 < sizeof(inbuf)) { inbuf[i] = line[i]; i++; }
        inbuf[i] = 0;
        src = inbuf;
        if (!find_brace_group(src, &wstart, &bopen, &bclose, &wend))
            return line;
    } else {
        if (!find_brace_group(line, &wstart, &bopen, &bclose, &wend)) {
            return line;
        }
        /* A group exists: copy into the private buffer and re-anchor the
         * pointers into it. */
        unsigned i = 0;
        while (line[i] && i + 1 < sizeof(inbuf)) { inbuf[i] = line[i]; i++; }
        inbuf[i] = 0;
        wstart = inbuf + (wstart - line);
        bopen  = inbuf + (bopen  - line);
        bclose = inbuf + (bclose - line);
        wend   = inbuf + (wend   - line);
        src = inbuf;
    }

    unsigned o = 0;

    /* copy everything before the word */
    for (const char* q = src; q < wstart && o + 1 < sizeof(out); q++)
        out[o++] = *q;

    /* word prefix = [wstart, bopen)  word suffix = (bclose, wend) */
    unsigned pre_len = (unsigned)(bopen - wstart);
    unsigned suf_len = (unsigned)(wend - (bclose + 1));
    char pre[256], suf[256];
    if (pre_len >= sizeof(pre)) pre_len = sizeof(pre) - 1;
    if (suf_len >= sizeof(suf)) suf_len = sizeof(suf) - 1;
    for (unsigned i = 0; i < pre_len; i++) pre[i] = wstart[i];
    pre[pre_len] = 0;
    for (unsigned i = 0; i < suf_len; i++) suf[i] = (bclose + 1)[i];
    suf[suf_len] = 0;

    /* inner = (bopen, bclose) */
    const char* inner = bopen + 1;
    unsigned inner_len = (unsigned)(bclose - inner);

    /* Determine: numeric range, character range, step range, or comma list? */
    int is_range = 0, lo = 0, hi = 0, step_val = 0;
    int is_char_range = 0;
    char clo = 0, chi = 0;
    {
        int ok1 = 0, ok2 = 0, ok3 = 0;
        const char* q = parse_int(inner, &lo, &ok1);
        if (ok1 && q[0] == '.' && q[1] == '.') {
            int hi_val = 0;
            const char* r = parse_int(q + 2, &hi_val, &ok2);
            if (ok2) {
                hi = hi_val;
                /* Check for step: {lo..hi..step} */
                if (r[0] == '.' && r[1] == '.') {
                    const char* s = parse_int(r + 2, &step_val, &ok3);
                    if (ok3 && s == bclose && step_val != 0) {
                        is_range = 1;
                    }
                } else if (r == bclose) {
                    is_range = 1;
                    step_val = 0; /* will be computed below */
                }
            }
        }
        /* Character range: {a..z} — single char on each side of .. */
        if (!is_range && inner_len >= 5 && inner[1] == '.' && inner[2] == '.' &&
            inner[3] == '.' && inner_len == 5) {
            /* Actually inner_len = length between braces, so {a..f} has inner = "a..f" (len 4) */
        }
        if (!is_range && inner_len >= 4 && inner_len <= 5) {
            /* Check for char..char pattern */
            const char* dots = 0;
            for (unsigned ci = 0; ci + 1 < inner_len; ci++) {
                if (inner[ci] == '.' && inner[ci+1] == '.') { dots = &inner[ci]; break; }
            }
            if (dots && dots > inner && dots < inner + inner_len - 2) {
                unsigned left_len = (unsigned)(dots - inner);
                unsigned right_len = (unsigned)(inner + inner_len - (dots + 2));
                if (left_len == 1 && right_len == 1) {
                    clo = inner[0];
                    chi = dots[2];
                    is_char_range = 1;
                }
            }
        }
    }

    int first = 1;

    if (is_char_range) {
        /* Character range: {a..f} or {z..a} */
        int cstep = (clo <= chi) ? 1 : -1;
        for (int c = clo; ; c += cstep) {
            char ch[2] = { (char)c, 0 };
            o = emit_brace_item(out, sizeof(out), o, &first,
                                pre, pre_len, ch, 1, suf, suf_len);
            if (c == chi) break;
        }
    }
    else if (is_range) {
        int step = step_val ? step_val : ((lo <= hi) ? 1 : -1);
        if (step > 0 && lo > hi) step = -step;
        if (step < 0 && lo < hi) step = -step;
        int v = lo;
        int safety = 0;
        while (safety++ < 10000) {
            /* Check if v is within range before emitting */
            if (step > 0 && v > hi) break;
            if (step < 0 && v < hi) break;
            char num[16]; int n = 0; int x = v; int neg = 0;
            if (x < 0) { neg = 1; x = -x; }
            char tmp[16]; int t = 0;
            if (x == 0) tmp[t++] = '0';
            while (x > 0) { tmp[t++] = '0' + (x % 10); x /= 10; }
            if (neg) num[n++] = '-';
            while (t > 0) num[n++] = tmp[--t];
            num[n] = 0;
            o = emit_brace_item(out, sizeof(out), o, &first,
                                pre, pre_len, num, (unsigned)n, suf, suf_len);
            if (v == hi) break;
            v += step;
        }
    } else {
        /* comma list — split inner on top-level, UNESCAPED commas (BUG-145). */
        unsigned ci = 0;
        while (ci <= inner_len) {
            unsigned start = ci;
            int d = 0;
            while (ci < inner_len) {
                char c = inner[ci];
                if (c == '\\' && ci + 1 < inner_len) { ci += 2; continue; }
                if (c == '{') d++;
                else if (c == '}') d--;
                else if (c == ',' && d == 0) break;
                ci++;
            }
            o = emit_brace_item(out, sizeof(out), o, &first,
                                pre, pre_len, inner + start, ci - start,
                                suf, suf_len);
            if (ci >= inner_len) break;
            ci++; /* skip comma */
        }
    }

    /* copy everything after the word */
    for (const char* q = wend; *q && o + 1 < sizeof(out); q++)
        out[o++] = *q;
    out[o] = 0;

    /* Recurse to handle additional/nested brace groups. */
    return jash_expand_braces(out, ctx);
    #undef inbuf
    #undef out
}

/* ── stage 3: glob expansion ───────────────────────────────────────────
 * For each whitespace-delimited word that contains glob metacharacters
 * (* ? [), read the target directory and substitute matching entries.
 * If nothing matches, the literal pattern is kept (bash default,
 * nullglob off). */

/* linux_dirent64 layout (matches kernel encoder):
 *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[]; */
struct jash_dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

/* O_RDONLY=0, O_DIRECTORY=0x10000 */
#define JASH_O_DIRECTORY 0x10000

/* word has glob meta outside quotes? */
static int word_has_glob(const char* w, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        char c = w[i];
        if (c == '*' || c == '?') return 1;
        /* PERF/parity: an unclosed '[' is NOT a bracket expression — bash
         * treats it as a literal. Without this check every `[ $i -lt N ]`
         * loop condition triggered a full opendir/readdir scan of the cwd
         * PER ITERATION (the readdir storm behind the 2-3x bench loss to
         * bash on tight loops). Only count '[' when a closing ']' follows
         * with at least one set character between. F E R M I ∞ H A R T */
        if (c == '[') {
            for (unsigned j = i + 2; j < len; j++)
                if (w[j] == ']') return 1;
            continue;
        }
        /* extglob patterns: @( *( +( ?( !( */
        if ((c == '@' || c == '*' || c == '+' || c == '?' || c == '!') &&
            i + 1 < len && w[i + 1] == '(') return 1;
    }
    return 0;
}

static int pattern_allows_hidden(const char* pat) {
    return pat[0] == '.' || strstr(pat, "/.") != NULL;
}

// JINV-118: pathname expansion used to split a word into ONE directory prefix
// and ONE final-component pattern (split_glob_word), then do a single opendir.
// A wildcard in a NON-final component therefore landed inside the path handed
// to opendir: `c/*/*.c` called opendir("c/*"), which fails, so the word was
// emitted as a literal. Single-level globs worked, which is exactly why this
// survived: the common case is right, so the broken case reads as "no such
// files". For an agent that is the worst failure mode -- an empty result set
// is indistinguishable from "this does not exist in the codebase", and no
// error is ever printed. This walker matches component by component instead,
// recursing only into directories that actually match.
#define JASH_GLOB_MULTI_MAX_DEPTH 64

/* Is `path` a directory? d_type is a fast path; DT_UNKNOWN filesystems (and
 * symlinks, which must be followed like bash does) fall back to stat. */
static int glob_is_dir(const char* path, unsigned char d_type) {
    if (d_type == DT_DIR) return 1;
    if (d_type != DT_UNKNOWN && d_type != DT_LNK) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static char* glob_join_alloc(const char* prefix, const char* name,
                             int drop_dot) {
    if (drop_dot && str_eq(prefix, ".")) prefix = "";
    size_t pl = strlen(prefix), nl = strlen(name);
    int slash = pl > 0 && prefix[pl - 1] != '/';
    char* out = (char*)malloc(pl + (slash ? 1u : 0u) + nl + 1);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, prefix, pl); o = pl;
    if (slash) out[o++] = '/';
    memcpy(out + o, name, nl + 1);
    return out;
}

/* JINV-132: append one full match. Storage grows; the only refusal is the
 * genuine execve ceiling, and it is reported, never silently dropped.
 *
 * The old code stored matches in `char matches[256][256]` — TWO silent lies in
 * one declaration. Match 257 vanished while `matched` stayed 1, so the shell
 * confidently emitted a SHORT file list; and a path longer than 254 bytes was
 * strlcpy-truncated into a DIFFERENT path that may well exist. A wrong-but-
 * plausible file list handed to `rm` is the worst outcome this shell can
 * produce, so neither failure may stay quiet. F E R M I ∞ H A R T */
static int glob_push(jash_argvbuf_t* m, const char* s) {
    unsigned n = (unsigned)strlen(s) + 1;
    if (jash_argvbuf_reserve(m, n) != 0) return 0;
    if (jash_argvbuf_slot(m) != 0)       return 0;
    m->aoff[m->argc++] = m->blen;
    memcpy(m->buf + m->blen, s, n);
    m->blen += n;
    return 1;
}

static int glob_cmp(const void* a, const void* b) {
    return strcoll(*(const char* const*)a, *(const char* const*)b);
}

static int g_glob_error = 0;
static char g_glob_error_msg[160];

int jash_glob_error(void) { return g_glob_error; }
const char* jash_glob_error_msg(void) { return g_glob_error_msg; }
void jash_glob_clear_error(void) {
    g_glob_error = 0;
    g_glob_error_msg[0] = 0;
}

static int glob_emit_matches(jash_argvbuf_t* matches, char* out,
                             unsigned cap, unsigned* o) {
    if (matches->argc > 0) {
        jash_argvbuf_done(matches);
        if (matches->argc > 1)
            qsort(matches->argv, (size_t)matches->argc, sizeof(char*), glob_cmp);
    }
    unsigned saved = *o;
    for (unsigned i = 0; i < matches->argc; i++) {
        if (i > 0) {
            if (*o + 1 >= cap) goto cut;
            out[(*o)++] = ' ';
        }
        for (const char* q = matches->argv[i]; *q; q++) {
            if (*o + 1 >= cap) goto cut;
            if (*q == '"')       out[(*o)++] = (char)0x03;
            else if (*q == '\'') out[(*o)++] = (char)0x04;
            else if (*q == ' ')  out[(*o)++] = (char)0x06;
            else                 out[(*o)++] = jash_meta_shield(*q);
        }
    }
    jash_argvbuf_free(matches);
    return 0;
cut:
    *o = saved;
    jash_argvbuf_free(matches);
    return -1;
}

/* Walk `pat_rest` (a slash-separated pattern) against the tree rooted at `dir`,
 * appending every full match to `m` as `emit_prefix` + matched name. */
static void glob_multi_walk(const char* pat_rest, const char* dir,
                            const char* emit_prefix,
                            jash_argvbuf_t* m, int* overflow,
                            int depth, long* line_steps) {
    if (depth > JASH_GLOB_MULTI_MAX_DEPTH) { *overflow = 1; return; }
    if (*overflow) return;

    /* Split off the first component. */
    const char* slash = strchr(pat_rest, '/');
    size_t cl = slash ? (size_t)(slash - pat_rest) : strlen(pat_rest);
    char* comp = (char*)malloc(cl + 1);
    if (!comp) { *overflow = 1; return; }
    memcpy(comp, pat_rest, cl); comp[cl] = '\0';

    /* Collapse redundant separators: "a//b" and a trailing "/" . */
    if (cl == 0) {
        if (slash) glob_multi_walk(slash + 1, dir, emit_prefix,
                                   m, overflow, depth + 1, line_steps);
        free(comp);
        return;
    }

    int is_last = (slash == NULL);

    // A literal component needs no scan -- descend (or emit) directly. This
    // keeps `c/parser/*.c` as cheap as it was before.
    if (!word_has_glob(comp, cl)) {
        char* child_dir = glob_join_alloc(dir, comp, 1);
        char* child_emit = glob_join_alloc(emit_prefix, comp, 0);
        free(comp);
        if (!child_dir || !child_emit) {
            free(child_dir); free(child_emit); *overflow = 1; return;
        }
        if (is_last) {
            struct stat st;
            if (stat(child_dir, &st) == 0 && !glob_push(m, child_emit))
                *overflow = 1;
            free(child_dir); free(child_emit);
            return;
        }
        glob_multi_walk(slash + 1, child_dir, child_emit,
                        m, overflow, depth + 1, line_steps);
        free(child_dir); free(child_emit);
        return;
    }

    DIR* dh = opendir(dir);
    if (!dh) { free(comp); return; }
    struct dirent* de;
    while ((de = readdir(dh)) != NULL && !*overflow) {
        const char* nm = de->d_name;
        /* POSIX: a leading '.' is only matched by an explicit leading '.' */
        if (nm[0] == '.' && comp[0] != '.') continue;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        if (!jash_glob_match_ctx(comp, nm, line_steps)) continue;

        char* child_dir = glob_join_alloc(dir, nm, 1);
        char* child_emit = glob_join_alloc(emit_prefix, nm, 0);
        if (!child_dir || !child_emit) {
            free(child_dir); free(child_emit); *overflow = 1; break;
        }

        if (is_last) {
            if (!glob_push(m, child_emit)) {
                free(child_dir); free(child_emit); *overflow = 1; break;
            }
        } else if (glob_is_dir(child_dir, de->d_type)) {
            glob_multi_walk(slash + 1, child_dir, child_emit,
                            m, overflow, depth + 1, line_steps);
        }
        free(child_dir); free(child_emit);
    }
    closedir(dh);
    free(comp);
}

/* SECURITY (audit rank 13, recursion DoS): cap recursive globstar traversal.
 * Paths themselves are dynamically joined and never truncated. */
#define JASH_GLOBSTAR_MAX_DEPTH 64
static int g_globstar_depth = 0;

/* PERF (audit F2): reusable growable path buffers shared by the whole
 * globstar walk — a handful of allocations per expansion instead of 2-3
 * malloc/free per dirent (~300k mallocs on a 100k-file tree). `rel` and
 * `dir` are used with push/pop length marks by the recursion; `emit` is
 * scratch for the current level only. */
typedef struct {
    char*  rel;     size_t rel_cap;
    char*  dir;     size_t dir_cap;
    char*  emit;    size_t emit_cap;
} globstar_bufs_t;

static int globstar_ensure(char** buf, size_t* cap, size_t need) {
    if (*cap >= need) return 1;
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need) ncap *= 2;
    char* nb = (char*)realloc(*buf, ncap);
    if (!nb) return 0;
    *buf = nb; *cap = ncap;
    return 1;
}

/* b->dir[0, dir_len) is the directory being scanned; b->rel[0, rel_len) is
 * the same path relative to the walk root (what the pattern matches). Both
 * buffers stay NUL-terminated at those marks between iterations. */
static void globstar_walk(const char* pattern, const char* emit_prefix,
                          int allow_hidden, jash_argvbuf_t* matches,
                          int* overflow, int* matched,
                          globstar_bufs_t* b, size_t dir_len, size_t rel_len,
                          long* line_steps) {
    DIR* d = opendir(b->dir);
    if (!d) return;

    struct dirent* ent;
    while (!*overflow && (ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        if (name[0] == '.' && !allow_hidden) continue;

        /* push name onto rel */
        size_t name_len = strlen(name);
        size_t child_rel_len = rel_len + (rel_len ? 1u : 0u) + name_len;
        if (!globstar_ensure(&b->rel, &b->rel_cap, child_rel_len + 1)) {
            *overflow = 1; break;
        }
        size_t cr = rel_len;
        if (rel_len) b->rel[cr++] = '/';
        memcpy(b->rel + cr, name, name_len + 1);

        if (jash_glob_match_ctx(pattern, b->rel, line_steps)) {
            size_t prefix_len = strlen(emit_prefix);
            if (!globstar_ensure(&b->emit, &b->emit_cap,
                                 prefix_len + child_rel_len + 1)) {
                *overflow = 1; break;
            }
            memcpy(b->emit, emit_prefix, prefix_len);
            memcpy(b->emit + prefix_len, b->rel, child_rel_len + 1);
            if (!glob_push(matches, b->emit)) {
                *overflow = 1; break;
            }
            *matched = 1;
        }

        /* push name onto dir */
        int add_slash = dir_len > 0 && b->dir[dir_len - 1] != '/';
        size_t child_dir_len = dir_len + (add_slash ? 1u : 0u) + name_len;
        if (!globstar_ensure(&b->dir, &b->dir_cap, child_dir_len + 1)) {
            *overflow = 1; break;
        }
        size_t cd = dir_len;
        if (add_slash) b->dir[cd++] = '/';
        memcpy(b->dir + cd, name, name_len + 1);

        /* PERF (audit F1): d_type fast path with stat fallback (glob_is_dir,
         * same helper the multi-component walker uses) instead of an
         * opendir()+closedir() probe per dirent — 2 syscalls saved per
         * entry. */
        if (glob_is_dir(b->dir, ent->d_type)) {
            /* SECURITY (rank 13): only descend if within the depth cap; the
             * buffers grow on demand, so path truncation (the old infinite-
             * recursion fixed point) cannot occur. */
            if (g_globstar_depth < JASH_GLOBSTAR_MAX_DEPTH &&
                !*overflow) {
                g_globstar_depth++;
                globstar_walk(pattern, emit_prefix,
                              allow_hidden, matches, overflow, matched,
                              b, child_dir_len, child_rel_len, line_steps);
                g_globstar_depth--;
                /* pop: re-terminate at this level's marks (the recursion
                 * may have realloc'd the buffers). */
                b->dir[dir_len] = '\0';
                b->rel[rel_len] = '\0';
            } else if (!*overflow) {
                *overflow = 1;
            }
        }
    }
    closedir(d);
}


/* ── extglob expansion helper ──────────────────────────────────────────
 * Expand @(pat1|pat2|...) against the files in cwd.
 * PERF (audit F5): ONE directory scan, testing all K alternatives per
 * entry — the old code re-scanned the directory per alternative (O(K×N))
 * and could emit the same entry once per matching alternative; now each
 * entry is emitted at most once. Returns number of matches (0 if none). */
static int expand_extglob_at(const char* word, jash_argvbuf_t* matches,
                             int* overflow, long* line_steps) {
    /* Find @( */
    const char* at = strstr(word, "@(");
    if (!at) return 0;

    /* Extract prefix before @( */
    size_t plen = (size_t)(at - word);

    /* Extract alternatives (between @( and )) */
    const char* open = at + 2;
    const char* close = open;
    int depth = 1;
    while (*close && depth > 0) {
        if (*close == '(') depth++;
        else if (*close == ')') depth--;
        if (depth > 0) close++;
    }
    if (depth != 0) return 0;

    /* Extract suffix after ) */
    const char* suffix = close + 1;
    size_t slen = strlen(suffix);

    int matched = 0;
    DIR* dir = opendir(".");
    if (!dir) return 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL && !*overflow) {
        const char* nm = ent->d_name;
        if (nm[0] == '.') continue;
        /* Try each alternative against this entry. */
        const char* alt = open;
        for (;;) {
            const char* alt_end = alt;
            while (alt_end < close && *alt_end != '|') alt_end++;
            size_t alen = (size_t)(alt_end - alt);
            char* fullpat = (char*)malloc(plen + alen + slen + 1);
            if (!fullpat) { *overflow = 1; break; }
            memcpy(fullpat, word, plen);
            memcpy(fullpat + plen, alt, alen);
            memcpy(fullpat + plen + alen, suffix, slen + 1);
            int m = jash_glob_match_ctx(fullpat, nm, line_steps);
            free(fullpat);
            if (m) {
                if (!glob_push(matches, nm)) *overflow = 1;
                else matched++;
                break;   /* one emission per entry, however many alts match */
            }
            if (alt_end == close) break;
            alt = alt_end + 1;
        }
    }
    closedir(dir);
    return matched;
}

const char* jash_expand_glob(const char* line, jash_expand_ctx_t* ctx) {
    if (!line) return line;
    ctx->glob_error = 0;
    ctx->glob_error_msg[0] = 0;
    jash_glob_clear_error();

    /* SECURITY (audit F6): ONE aggregate matcher step budget shared by every
     * glob match this line performs — the per-call cap alone let a hostile
     * pattern spend 4M steps PER dirent of a large directory. On exhaustion
     * the matcher answers "no match"; the walks below detect it
     * (glob_line_steps < 0) and refuse via the existing overflow path. */
    long glob_line_steps = JASH_GLOB_LINE_STEPS;

    #define out ctx->glob_out
    unsigned o = 0;
    const char* p = line;
    int sq = 0, dq = 0;

    while (*p && o + 1 < sizeof(out)) {
        /* copy leading whitespace */
        if (*p == ' ' || *p == '\t') { out[o++] = *p++; continue; }

        /* capture one word [wstart, wend) tracking quotes */
        const char* wstart = p;
        int wsq = 0, wdq = 0;
        while (*p) {
            if (wsq) { if (*p == '\'') wsq = 0; p++; continue; }
            if (wdq) { if (*p == '"') wdq = 0; p++; continue; }
            if (*p == '\'') { wsq = 1; p++; continue; }
            if (*p == '"')  { wdq = 1; p++; continue; }
            if (*p == ' ' || *p == '\t') break;
            p++;
        }
        unsigned wlen = (unsigned)(p - wstart);
        (void)sq; (void)dq;

        /* does this word contain unquoted glob meta? quotes already
         * consumed above; if the word had quotes we conservatively
         * still attempt glob (rare corner case, acceptable).
         *
         * PERF (audit F3): no per-word malloc when there is nothing to
         * expand. The un-shield below (BUG-118/JINV-096) only rewrites the
         * bytes 0x03/0x04/0x12..0x19, none of which is glob meta — so when
         * the slice carries none of them, word_has_glob on the raw slice is
         * the SAME test as on the un-shielded copy, and the word can be
         * copied verbatim without ever hitting the heap. A shielded 0x17
         * could hide a '(' that completes an extglob operator, so any
         * shield byte falls back to the old un-shield-then-test path. */
        int has_shield = 0;
        for (unsigned i = 0; i < wlen; i++) {
            unsigned char c = (unsigned char)wstart[i];
            if (c == 0x03 || c == 0x04 || (c >= 0x12 && c <= 0x19)) {
                has_shield = 1; break;
            }
        }
        if (!has_shield && !word_has_glob(wstart, wlen)) {
            /* copy verbatim */
            for (unsigned i = 0; i < wlen && o + 1 < sizeof(out); i++)
                out[o++] = wstart[i];
            continue;
        }

        char* word = (char*)malloc((size_t)wlen + 1);
        if (!word) {
            ctx->glob_error = g_glob_error = 1;
            snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                     "jash: glob: out of memory for pattern — refusing");
            snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                     ctx->glob_error_msg);
            return line;
        }
        memcpy(word, wstart, wlen); word[wlen] = 0;
        unsigned cw = wlen;
        /* BUG-118: the PATTERN may carry shielded quote chars from an expanded
         * value ($pat="*'*" → the ' is shielded 0x04) — un-shield here so the
         * pattern matches real filenames containing quotes. The verbatim
         * (no-match) copy below still uses `wstart`, keeping shields for the
         * downstream tokenizers. */
        for (char* q = word; *q; q++) {
            if (*q == (char)0x03) *q = '"';
            else if (*q == (char)0x04) *q = '\'';
            /* JINV-096: mesma razao, para o metacaractere de estrutura vindo de
             * um valor — `V='a>b'; echo $V*` tem que casar o arquivo `a>b.txt`
             * de verdade. A copia verbatim abaixo continua usando `wstart`, ou
             * seja, mantem a blindagem para os tokenizadores a jusante. */
            else if (*q >= (char)0x12 && *q <= (char)0x19) {   /* JINV-096 */
                static const char meta[] = "|<>&;()~";
                *q = meta[*q - 0x12];
            }
        }

        if (!word_has_glob(word, cw)) {
            /* copy verbatim */
            for (unsigned i = 0; i < wlen && o + 1 < sizeof(out); i++)
                out[o++] = wstart[i];
            free(word);
            continue;
        }

        /* extglob @(pat|pat|...) expansion */
        if (strstr(word, "@(")) {
            jash_argvbuf_t ext = {0};
            int overflow = 0;
            int matched = expand_extglob_at(word, &ext, &overflow,
                                            &glob_line_steps);
            if (glob_line_steps < 0) overflow = 1;   /* F6: budget spent */
            if (overflow || (matched && glob_emit_matches(&ext, out,
                                                          sizeof(out), &o) != 0)) {
                jash_argvbuf_free(&ext);
                ctx->glob_error = g_glob_error = 1;
                snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                         "jash: extglob: result exceeds safe storage — refusing");
                snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                         ctx->glob_error_msg);
                free(word);
                return line;
            }
            /* BUG-167: nullglob — drop the word entirely instead of keeping
             * the literal pattern when there's no match. */
            if (!matched && !ctx->nullglob) {
                jash_argvbuf_free(&ext);
                for (unsigned i = 0; i < wlen && o + 1 < sizeof(out); i++)
                    out[o++] = wstart[i];
            } else if (!matched) {
                jash_argvbuf_free(&ext);
            }
            free(word);
            continue;
        }

        if (strstr(word, "**")) {
            int matched = 0;
            int overflow = 0;
            jash_argvbuf_t matches = {0};
            int allow_hidden = pattern_allows_hidden(word);
            if (word[0] == '/') {
                const char* ds = strstr(word, "**");
                const char* slash = ds;
                while (slash > word && slash[-1] != '/') slash--;
                if (slash > word && slash[-1] == '/') slash--;

                size_t dl = (size_t)(slash - word);
                char* start_dir = NULL;
                char* start_rel = NULL;
                if (dl == 0) {
                    start_dir = strdup("/");
                    start_rel = strdup("");
                } else {
                    size_t rl = dl - 1;
                    start_dir = (char*)malloc(dl + 1);
                    start_rel = (char*)malloc(rl + 1);
                    if (start_dir) { memcpy(start_dir, word, dl); start_dir[dl] = 0; }
                    if (start_rel) { memcpy(start_rel, word + 1, rl); start_rel[rl] = 0; }
                }
                if (!start_dir || !start_rel) {
                    free(start_dir); free(start_rel);
                    overflow = 1;
                } else {
                    /* seed the reusable buffers with the walk root */
                    globstar_bufs_t b = {0};
                    size_t sdl = strlen(start_dir), srl = strlen(start_rel);
                    if (!globstar_ensure(&b.dir, &b.dir_cap, sdl + 1) ||
                        !globstar_ensure(&b.rel, &b.rel_cap, srl + 1)) {
                        overflow = 1;
                    } else {
                        memcpy(b.dir, start_dir, sdl + 1);
                        memcpy(b.rel, start_rel, srl + 1);
                        globstar_walk(word + 1, "/",
                                      allow_hidden, &matches, &overflow,
                                      &matched, &b, sdl, srl,
                                      &glob_line_steps);
                    }
                    free(b.rel); free(b.dir); free(b.emit);
                    free(start_dir); free(start_rel);
                }
            } else {
                globstar_bufs_t b = {0};
                if (!globstar_ensure(&b.dir, &b.dir_cap, 2) ||
                    !globstar_ensure(&b.rel, &b.rel_cap, 1)) {
                    overflow = 1;
                } else {
                    memcpy(b.dir, ".", 2);
                    b.rel[0] = '\0';
                    globstar_walk(word, "", allow_hidden,
                                  &matches, &overflow, &matched,
                                  &b, 1, 0, &glob_line_steps);
                }
                free(b.rel); free(b.dir); free(b.emit);
            }
            if (glob_line_steps < 0) overflow = 1;   /* F6: budget spent */
            if (overflow) {
                jash_argvbuf_free(&matches);
                ctx->glob_error = 1;
                snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                         "jash: globstar: path/match storage exhausted — refusing");
                g_glob_error = 1;
                snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                         ctx->glob_error_msg);
                free(word);
                return line;
            }
            if (matched && glob_emit_matches(&matches, out, sizeof(out), &o) != 0) {
                ctx->glob_error = 1;
                snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                         "jash: globstar: expanded line exceeds %zu bytes — refusing",
                         sizeof(out) - 1);
                g_glob_error = 1;
                snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                         ctx->glob_error_msg);
                free(word);
                return line;
            }
            /* BUG-167: nullglob — drop instead of keeping the literal. */
            if (!matched && !ctx->nullglob) {
                jash_argvbuf_free(&matches);
                for (unsigned i = 0; i < wlen && o + 1 < sizeof(out); i++)
                    out[o++] = wstart[i];
            } else if (!matched) {
                jash_argvbuf_free(&matches);
            }
            free(word);
            continue;
        }

        /* perform directory scan — opendir/readdir (POSIX) instead of raw
         * sys_getdents64, which is Linux-only and returns -ENOSYS on macOS,
         * silently killing ALL glob expansion there. F E R M I ∞ H A R T */
        jash_argvbuf_t mb = {0};
        int overflow = 0;
        int matched = 0;

        /* JINV-118: a wildcard BEFORE the last '/' cannot be handed to
         * opendir() as part of a path — walk it component by component. */
        if (word[0] == '/')
            glob_multi_walk(word + 1, "/", "/", &mb, &overflow, 0,
                            &glob_line_steps);
        else
            glob_multi_walk(word, ".", "", &mb, &overflow, 0,
                            &glob_line_steps);
        if (glob_line_steps < 0) overflow = 1;   /* F6: budget spent */
        matched = mb.argc > 0;

        /* JINV-132: a SHORT file list is the most dangerous thing this
         * function can return — `rm *` on 256 of 4000 names deletes the
         * wrong set and looks like it worked. So the only two outcomes are
         * ALL the matches or none, and "none" says why. Same contract the
         * line-length ceiling above already uses: refuse and explain, never
         * truncate. F E R M I ∞ H A R T */
        if (overflow) {
            jash_argvbuf_free(&mb);
            ctx->glob_error = g_glob_error = 1;
            snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                     "jash: glob: match storage exhausted — refusing");
            snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                     ctx->glob_error_msg);
            free(word);
            return line;
        }

        {
            /* sort matches alphabetically (bash-compatible) */
            if (mb.argc > 0) {
                jash_argvbuf_done(&mb);
                if (mb.argc > 1)
                    qsort(mb.argv, (size_t)mb.argc, sizeof(char*), glob_cmp);
            }

            /* emit sorted matches. BUG-118: a matched filename is DATA — a
             * literal ' or " in it must not be re-parsed as quote syntax by
             * the downstream tokenizers, so shield them (unshielded per-token
             * after tokenization, like param-expansion values).
             *
             * JINV-132: the line buffer is the SECOND place a list could be
             * cut in half. Emit atomically: rewind to `o_save` and refuse if
             * the whole list doesn't fit, because half a list here is the
             * same wrong-files-deleted failure as half a list above. */
            unsigned o_save = o;
            int cut = 0;
            for (unsigned i = 0; i < mb.argc && !cut; i++) {
                if (i > 0) {
                    if (o + 1 >= sizeof(out)) { cut = 1; break; }
                    out[o++] = ' ';
                }
                for (const char* q = mb.argv[i]; *q; q++) {
                    if (o + 1 >= sizeof(out)) { cut = 1; break; }
                    if (*q == '"')       out[o++] = (char)0x03;
                    else if (*q == '\'') out[o++] = (char)0x04;
                    /* JINV-009: o espaço ENTRE matches (emitido acima) é
                     * separador de campo; o espaço DENTRO de um nome é DADO.
                     * Blindar só este — senão o tokenizador a jusante parte
                     * "meu arquivo.txt" em duas palavras. */
                    else if (*q == ' ')  out[o++] = (char)0x06;
                    /* JINV-096: o NOME DE ARQUIVO tambem e dado, e ninguem o
                     * digitou nesta linha. Medido: com um arquivo chamado
                     * `a|tee X` no diretorio, `echo *` EXECUTAVA o tee — a
                     * mesma familia, com o glob no lugar da variavel. */
                    else                 out[o++] = jash_meta_shield(*q);   /* JINV-096 */
                }
            }
            jash_argvbuf_free(&mb);

            if (cut) {
                o = o_save;
                ctx->glob_error = g_glob_error = 1;
                snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                         "jash: glob: expanded line exceeds %zu bytes — refusing",
                         sizeof(out) - 1);
                snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                         ctx->glob_error_msg);
                free(word);
                return line;
            }
        }

        if (!matched && !ctx->nullglob) {
            /* nullglob off: keep the literal pattern. BUG-167: when
             * nullglob is on, drop the word entirely instead. */
            for (unsigned i = 0; i < wlen && o + 1 < sizeof(out); i++)
                out[o++] = wstart[i];
        }
        free(word);
    }
    if (*p) {
        ctx->glob_error = g_glob_error = 1;
        snprintf(ctx->glob_error_msg, sizeof ctx->glob_error_msg,
                 "jash: glob: aggregate expanded line exceeds %zu bytes — refusing",
                 sizeof(out) - 1);
        snprintf(g_glob_error_msg, sizeof g_glob_error_msg, "%s",
                 ctx->glob_error_msg);
        return line;
    }
    out[o] = 0;
    return out;
    #undef out
}

/* ANSI-C quoting: $'...' — process C-style escapes, emit a regular
 * single-quoted token. Supports \a \b \e \E \f \n \r \t \v \\ \' \" \?
 * \0NN \NNN (octal, up to 3 digits) \xHH \uHHHH. Returns a static buffer if
 * any $'...' was found, else NULL (caller leaves input untouched).
 * F E R M I ∞ H A R T */
const char* jash_expand_ansi_c(const char* line, jash_expand_ctx_t* ctx) {
    if (!line) return NULL;
    /* BUG-014: only a `$'` at TOP LEVEL starts an ANSI-C quote. A `$'` inside a
     * single- or double-quoted region is literal (e.g. echo 'dollar: $'). Track
     * quote state in both the probe and the copy loop so it isn't mis-detected. */
    const char* probe = line;
    int psq = 0, pdq = 0, seen = 0;
    while (*probe) {
        char c = *probe;
        if (psq) { if (c == '\'') psq = 0; probe++; continue; }
        if (pdq) {
            if (c == '\\' && probe[1]) { probe += 2; continue; }
            if (c == '"') pdq = 0;
            probe++; continue;
        }
        if (c == '$' && probe[1] == '\'') { seen = 1; break; }
        if (c == '\'') psq = 1;
        else if (c == '"') pdq = 1;
        probe++;
    }
    if (!seen) return NULL;

    #define buf ctx->ansi_out
    unsigned bi = 0;
    const char* p = line;
    int sq = 0, dq = 0;
    while (*p && bi + 1 < sizeof(buf)) {
        if (!sq && !dq && p[0] == '$' && p[1] == '\'') {
            p += 2;
            buf[bi++] = '\'';
            while (*p && *p != '\'' && bi + 4 < sizeof(buf)) {
                if (*p != '\\') { buf[bi++] = *p++; continue; }
                p++;
                if (!*p) break;
                char esc = *p++;
                int emit = -1;
                switch (esc) {
                    case 'a': emit = '\a'; break;
                    case 'b': emit = '\b'; break;
                    case 'e': case 'E': emit = 0x1b; break;
                    case 'f': emit = '\f'; break;
                    case 'n': emit = '\n'; break;
                    case 'r': emit = '\r'; break;
                    case 't': emit = '\t'; break;
                    case 'v': emit = '\v'; break;
                    case '\\': emit = '\\'; break;
                    case '\'': emit = '\''; break;
                    case '"':  emit = '"';  break;
                    case '?':  emit = '?';  break;
                    case 'x': {
                        int v = 0, n = 0;
                        while (n < 2 && *p) {
                            char c = *p;
                            int d = -1;
                            if (c >= '0' && c <= '9') d = c - '0';
                            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                            if (d < 0) break;
                            v = (v << 4) | d; n++; p++;
                        }
                        if (n) emit = v & 0xff;
                        break;
                    }
                    case '0': case '1': case '2': case '3':
                    case '4': case '5': case '6': case '7': {
                        int v = esc - '0', n = 1;
                        while (n < 3 && *p >= '0' && *p <= '7') {
                            v = (v << 3) | (*p - '0'); n++; p++;
                        }
                        emit = v & 0xff;
                        break;
                    }
                    default:
                        /* Unknown escape: preserve backslash + char (bash behavior). */
                        if (bi + 2 < sizeof(buf)) { buf[bi++] = '\\'; buf[bi++] = esc; }
                        continue;
                }
                if (emit >= 0) {
                    if (emit == '\'') {
                        /* Close current sq, emit escaped quote, reopen sq:
                         * standard POSIX 'a'\''b' encoding for an embedded '. */
                        if (bi + 4 < sizeof(buf)) {
                            buf[bi++] = '\''; buf[bi++] = '\\';
                            buf[bi++] = '\''; buf[bi++] = '\'';
                        }
                    } else {
                        buf[bi++] = (char)emit;
                    }
                }
            }
            if (*p == '\'') p++;
            if (bi + 1 < sizeof(buf)) buf[bi++] = '\'';
        } else {
            /* Copy verbatim, tracking quote state so an in-quote `$'` is not
             * mistaken for an ANSI-C quote on the next iteration. */
            char c = *p;
            if (sq) {
                if (c == '\'') sq = 0;
            } else if (dq) {
                if (c == '\\' && p[1]) {
                    buf[bi++] = *p++;
                    if (bi + 1 < sizeof(buf)) buf[bi++] = *p++;
                    continue;
                }
                if (c == '"') dq = 0;
            } else {
                if (c == '\'') sq = 1;
                else if (c == '"') dq = 1;
            }
            buf[bi++] = *p++;
        }
    }
    buf[bi] = '\0';
    return buf;
    #undef buf
}

/* ── orchestration ─────────────────────────────────────────────────── */

/* A sufficient (not necessary) proof that all word stages preserve the text.
 * Quotes alone are inert here: argv consumers remove them later. Decline every
 * expansion introducer, escape, control/sentinel and non-ASCII byte rather than
 * duplicating quote/expansion parsing. Recompute for each current input; no cache. */
static int word_layer_is_inert(const char* line) {
    for (const unsigned char* p = (const unsigned char*)line; *p; p++) {
        if (*p < 0x20 || *p >= 0x7f) return 0;
        switch (*p) {
            case '$': case '`': case '~': case '{': case '}':
            case '*': case '?': case '[': case ']': case '(':
            case ')': case '\\':
                return 0;
        }
    }
    return 1;
}

const char* jash_expand_word_layer(jash_state_t* s, const char* line, jash_expand_ctx_t* ctx) {
    if (!line) return line;

    /* JINV-026 — uma linha longa demais para estes buffers sai INTEIRA e sem
     * expansao, nunca cortada.
     *
     * Os buffers de brace/glob sao de 32768 e vivem na PILHA (o
     * jash_expand_ctx_t inteiro ja custa ~104 KB por chamada), entao nao da
     * para simplesmente eleva-los. Mas cortar aqui era pior do que parece:
     * o corte cai no MEIO da linha, e o que se perde e o final — incluindo o
     * proprio `|`. Medido: `echo "$(cat arquivo_de_60KB)" | wc -c` perdia o
     * estagio `| wc -c` e virava outro comando, em silencio.
     *
     * Perder a expansao de til/chaves/glob e visivel e recuperavel; perder um
     * estagio do pipeline muda o significado do comando sem avisar. Entao
     * acima do teto a linha passa INTACTA, com um aviso — a estrutura do que o
     * usuario escreveu e preservada.
     * Gate: tests/test_jinv026_long_line_structure.sh */
    {
        size_t n = strlen(line);
        size_t cap = sizeof(ctx->brace_inbuf);
        if (sizeof(ctx->brace_out) < cap) cap = sizeof(ctx->brace_out);
        if (sizeof(ctx->glob_out)  < cap) cap = sizeof(ctx->glob_out);
        if (n + 1 >= cap) {
            fprintf(stderr,
                    "jash: linha de %zu bytes excede o teto de expansao (%zu) — "
                    "executando SEM expansao de til/chaves/glob, e sem cortar "
                    "(cortar perderia o final da linha, inclusive pipes)\n",
                    n, cap - 1);
            return line;
        }
    }

    if (word_layer_is_inert(line)) {
        /* Preserve the glob stage's observable reset, including its absence
         * under set -f. Size refusal above must precede this reset as before. */
        if (!s->shell_opts.noglob) {
            ctx->nullglob = s->shell_opts.nullglob;
            ctx->glob_error = 0;
            ctx->glob_error_msg[0] = '\0';
            jash_glob_clear_error();
        }
        return line;
    }

    const char* cur = line;

    /* $'...' ANSI-C quoting: runs FIRST so escape codes are interpreted before
     * any other layer sees the bytes. After this, the body is a regular
     * single-quoted token that downstream code already handles literally. */
    const char* a = jash_expand_ansi_c(cur, ctx);
    if (a) cur = a;

    const char* t = jash_expand_tilde(s, cur, ctx);
    if (t) cur = t;

    const char* b = jash_expand_braces(cur, ctx);
    if (b) cur = b;

    /* set -f (noglob): skip pathname expansion entirely. F E R M I ∞ H A R T */
    if (!s->shell_opts.noglob) {
        ctx->nullglob = s->shell_opts.nullglob;  /* BUG-167 */
        const char* g = jash_expand_glob(cur, ctx);
        if (g) cur = g;
    }

    return cur;
}
