// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/param_expand — parameter expansion ${var...}

#include "param_expand.h"
#include <core/limits.h>
#include "eval.h"
#include "cmdsub.h"   /* JINV-087: sentinelas de campo/branco protegido */
#include "arithmetic.h"
#include <core/state.h>
#include <core/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

/* Depth governor for nested parameter expansions — must match eval.c */
#ifndef JASH_EVAL_MAX_DEPTH
#define JASH_EVAL_MAX_DEPTH 200
#endif

/* $SECONDS baseline — set on first access, never changes after that. */
static time_t g_seconds_base = 0;
static int    g_rand_seeded  = 0;

static int g_param_error = 0;
#define JASH_PARAM_EXPAND_CAP JASH_LINE_CAP
#define JASH_PARAM_EXPAND_DEPTH 8
static char* g_result_stack[JASH_PARAM_EXPAND_DEPTH];
static int g_stack_top = 0;
/* THREAD-SAFETY NOTE (F E R M I ∞ H A R T, 2026-06):
 * These static buffers follow the strtok/strerror pattern: return value
 * is valid until the next call to the same function. Jash is single-threaded
 * with a depth governor (JASH_EVAL_MAX_DEPTH), so reentrancy is bounded.
 * For true thread-safety, refactor to pass out-buffer parameter.
 * Priority: LOW — no current bugs, only theoretical. */
static char g_param_error_msg[256];

static const char* param_expand_impl(const char* str, void* state, int known_present);

static void positional_refuse_output(void) {
    g_param_error = 1;
    snprintf(g_param_error_msg, sizeof g_param_error_msg,
             "jash: positional expansion exceeds %u bytes — refusing",
             (unsigned)JASH_LINE_CAP - 1);
}

static void positional_refuse_memory(void) {
    g_param_error = 1;
    snprintf(g_param_error_msg, sizeof g_param_error_msg,
             "jash: positional expansion out of memory — refusing");
}

int jash_param_expand_error(void) {
    return g_param_error;
}

const char* jash_param_expand_error_msg(void) {
    return g_param_error_msg;
}

void jash_param_expand_clear_error(void) {
    g_param_error = 0;
    g_param_error_msg[0] = 0;
}

int jash_has_param_expansion(const char* str) {
    /* Single source of truth: jash_expansion_mask() replicates this exact
     * quote/trigger rule (see cmdsub.c) while answering all probes in one
     * pass. Delegating keeps one implementation, not two. */
    if (!str) return 0;
    return (jash_expansion_mask(str) & JASH_EXP_PARAM) != 0;
}

/* ADR-0036 W5: is `str` safe to serve from var_cache? Only bare $VAR /
 * ${VAR...} forms are — those read the env table, which s->env_generation
 * tracks exactly (bumped on every set/unset/append). $?, $$, $-, $#, $@, $*,
 * $_, $! and $1..$9 read live process/positional/shell-option state that
 * env_generation does NOT cover, so any string touching them is excluded
 * here and falls back to full re-expansion every call — identical to
 * pre-cache behavior for those forms, just no speedup (not a regression). */
static int pe_is_cacheable(const char* str) {
    for (const char* p = str; *p; p++) {
        if (*p != '$' || !p[1]) continue;
        const char* q = p + 1;
        if (*q == '{') q++;
        char c = *q;
        if (c == '?' || c == '$' || c == '#' || c == '!' || c == '-' ||
            c == '*' || c == '@' || c == '_' || (c >= '0' && c <= '9'))
            return 0;
    }
    return 1;
}

static void copy_expanded_text(jash_state_t* s, const char* src,
                               char* dst, size_t dst_size) {
    if (!src) { strncpy(dst, "", dst_size - 1); dst[dst_size - 1] = 0; return; }
    if (!jash_has_param_expansion(src)) {
        size_t len = strlen(src);
        if (len >= dst_size) len = dst_size - 1;
        for (size_t i = 0; i < len; i++) dst[i] = src[i];
        dst[len] = 0;
        return;
    }
    const char* expanded = param_expand_impl(src, s, 1);
    if (expanded) {
        size_t len = strlen(expanded);
        if (len >= dst_size) len = dst_size - 1;
        for (size_t i = 0; i < len; i++) dst[i] = expanded[i];
        dst[len] = 0;
    } else {
        size_t len = strlen(src);
        if (len >= dst_size) len = dst_size - 1;
        for (size_t i = 0; i < len; i++) dst[i] = src[i];
        dst[len] = 0;
    }
}

/* JINV-065 — orçamento de passos para o casador de ${var#pat} e irmãos.
 * Gêmeo do que está em `c/parser/case_stmt.c`; ver lá o racional completo.
 * Aqui o teto importa em dobro: `jash_strip_prefix`/`jash_strip_suffix` chamam
 * o casador UMA VEZ POR COMPRIMENTO de prefixo/sufixo (até 512 chamadas), então
 * o custo do casamento entra multiplicado por N. O estouro GRITA no stderr e o
 * laço de quem chama para — nunca um "não casou" mudo. */
#define JASH_PE_GLOB_MAX_STEPS 4000000L
static long g_pe_glob_steps;
static int  g_pe_glob_over;

/* Pattern matching: *, ?, and [set] bracket expressions */
static int pe_glob_match_rec(const char* pattern, const char* text) {
    if (!pattern || !text) return 0;
    if (++g_pe_glob_steps > JASH_PE_GLOB_MAX_STEPS) {  /* JINV-065 */
        g_pe_glob_over = 1;
        return 0;
    }

    while (*pattern && *text) {
        if (*pattern == '\\' && pattern[1]) {
            /* BUG-134: backslash escapes the next char to a literal match. */
            if (pattern[1] == *text) { pattern += 2; text++; continue; }
            return 0;
        } else if (*pattern == '*') {
            /* JINV-065 — colapsa a CORRIDA inteira de `*` antes de sair
             * pesquisando. `*****` É `*`; recursar em `pattern + 1` dá a cada
             * asterisco da corrida uma varredura própria de todo o resto do
             * texto, e as varreduras se multiplicam (C(n,k)). O idioma correto
             * já existia no rabo desta função (linha do `while (*pattern ==
             * '*')` no fim), onde só roda com o texto esgotado e portanto não
             * protege nada. A parada de `\\*` é de propósito: asterisco
             * escapado é literal e não faz parte da corrida. */
            const char* rest = pattern + 1;
            while (*rest == '*') rest++;
            if (!*rest) return 1;
            while (*text) {
                if (pe_glob_match_rec(rest, text)) return 1;
                if (g_pe_glob_over) return 0;  /* JINV-065: desenrola já */
                text++;
            }
            return pe_glob_match_rec(rest, text);
        } else if (*pattern == '[' && strchr(pattern + 1, ']')) {
            /* BUG-134: an unterminated `[` (no closing `]`) is a literal `[`,
             * so it falls through to the literal-char branch below. */
            /* Bracket expression: [abc], [a-z], [^abc], [!abc], [[:class:]] */
            int negate = 0;
            const char* p = pattern + 1;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int matched = 0;
            unsigned char tc = (unsigned char)*text;
            while (*p && *p != ']') {
                /* POSIX character class: [[:alpha:]] etc. */
                if (p[0] == '[' && p[1] == ':') {
                    const char* cls = p + 2;
                    const char* end = cls;
                    while (*end && !(end[0] == ':' && end[1] == ']')) end++;
                    unsigned clen = (unsigned)(end - cls);
                    if (end[0] == ':' && end[1] == ']') {
                        if      (clen == 5 && !__builtin_strncmp(cls, "alpha", 5)) { if (isalpha(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "digit", 5)) { if (isdigit(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "alnum", 5)) { if (isalnum(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "lower", 5)) { if (islower(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "upper", 5)) { if (isupper(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "space", 5)) { if (isspace(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "blank", 5)) { if (tc == ' ' || tc == '\t') matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "print", 5)) { if (isprint(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "graph", 5)) { if (isgraph(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "cntrl", 5)) { if (iscntrl(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "punct", 5)) { if (ispunct(tc)) matched = 1; }
                        else if (clen == 5 && !__builtin_strncmp(cls, "xdigit",6)) { if (isxdigit(tc)) matched = 1; }
                        p = end + 2;
                    } else {
                        /* Malformed [:class:] — treat [ as literal */
                        if (tc == (unsigned char)*p) matched = 1;
                        p++;
                    }
                } else if (p[1] == '-' && p[2] && p[2] != ']') {
                    /* Range: a-z */
                    if (tc >= (unsigned char)*p && tc <= (unsigned char)p[2])
                        matched = 1;
                    p += 3;
                } else {
                    if (tc == (unsigned char)*p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if ((matched && !negate) || (!matched && negate)) {
                pattern = p;
                text++;
            } else {
                return 0;
            }
        } else if (*pattern == '?' || *pattern == *text) {
            pattern++;
            text++;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return !*pattern && !*text;
}

/* JINV-065 — porta única de entrada: zera o orçamento e, se estourar, RECUSA o
 * padrão em voz alta. Contrato para quem chama: com `g_pe_glob_over` de pé, o 0
 * devolvido significa "não sei", não "não casou". */
static int jash_glob_match(const char* pattern, const char* text) {
    g_pe_glob_steps = 0;
    g_pe_glob_over  = 0;
    int r = pe_glob_match_rec(pattern, text);
    if (g_pe_glob_over) {
        fprintf(stderr,
                "jash: ${...}: padrao recusado por custo — o casamento "
                "estourou o orcamento de %ld passos: '%s'\n",
                (long)JASH_PE_GLOB_MAX_STEPS, pattern ? pattern : "");
        return 0;
    }
    return r;
}

/* Bounded copy of n bytes into the fixed 512-byte result[] buffers used by the
 * strip-prefix/suffix expansions, always NUL-terminated. SECURITY: the callers
 * copied a variable value of arbitrary length into result[512] via strcpy() or
 * strncpy(result, str, i) (i ranges up to strlen(str)) — a >511-byte ${var#..}
 * / ${var%..} operand overflowed the static buffer. Clamp here. Sibling of the
 * ${var/old/new} old_str clamp. */
static void jash_strip_cpy(char *result, const char *src, int n) {
    if (n < 0) n = 0;
    if (n > 511) n = 511;          /* sizeof(result) - 1 */
    if (src) { for (int i = 0; i < n; i++) result[i] = src[i]; }
    result[n] = 0;
}

/* Strip prefix matching pattern (shortest match if strip_long=0) */
/* Ring buffer: allows up to JASH_EVAL_MAX_DEPTH nested strip operations
 * without reentrancy bugs. Each call uses the next slot, wrapping around. */
static int g_strip_prefix_slot = 0;
static char g_strip_prefix_bufs[JASH_EVAL_MAX_DEPTH][512];

static char* jash_strip_prefix(const char* str, const char* pattern, int strip_long) {
    char* result = g_strip_prefix_bufs[g_strip_prefix_slot++ & (JASH_EVAL_MAX_DEPTH - 1)];

    if (!str || !pattern) {
        jash_strip_cpy(result, str, str ? (int)strlen(str) : 0);
        return result;
    }

    int str_len = strlen(str);

    if (strip_long) {
        /* ## Longest prefix match: find longest prefix that matches pattern.
         * Try from longest (str_len) to shortest (0). */
        for (int i = str_len; i >= 0; i--) {
            char prefix[512];
            if (i >= (int)sizeof(prefix)) continue;
            jash_strip_cpy(prefix, str, i);
            if (jash_glob_match(pattern, prefix)) {
                jash_strip_cpy(result, str + i, str_len - i);
                return result;
            }
            if (g_pe_glob_over) break;  /* JINV-065: padrão recusado, pare */
        }
    } else {
        /* # Shortest prefix match: find shortest prefix that matches pattern.
         * Try from shortest (0) to longest (str_len). */
        for (int i = 0; i <= str_len; i++) {
            char prefix[512];
            if (i >= (int)sizeof(prefix)) continue;
            jash_strip_cpy(prefix, str, i);
            if (jash_glob_match(pattern, prefix)) {
                jash_strip_cpy(result, str + i, str_len - i);
                return result;
            }
            if (g_pe_glob_over) break;  /* JINV-065: padrão recusado, pare */
        }
    }

    jash_strip_cpy(result, str, str_len);
    return result;
}

/* Strip suffix matching pattern */
static int g_strip_suffix_slot = 0;
static char g_strip_suffix_bufs[JASH_EVAL_MAX_DEPTH][512];

static char* jash_strip_suffix(const char* str, const char* pattern, int strip_long) {
    char* result = g_strip_suffix_bufs[g_strip_suffix_slot++ & (JASH_EVAL_MAX_DEPTH - 1)];

    if (!str || !pattern) {
        jash_strip_cpy(result, str, str ? (int)strlen(str) : 0);
        return result;
    }

    int str_len = strlen(str);

    if (strip_long) {
        /* Longest match: try from the start */
        for (int i = 0; i <= str_len; i++) {
            if (jash_glob_match(pattern, str + i)) {
                jash_strip_cpy(result, str, i);
                return result;
            }
            if (g_pe_glob_over) break;  /* JINV-065: padrão recusado, pare */
        }
    } else {
        /* Shortest match: try from the end */
        for (int i = str_len; i >= 0; i--) {
            if (jash_glob_match(pattern, str + i)) {
                jash_strip_cpy(result, str, i);
                return result;
            }
            if (g_pe_glob_over) break;  /* JINV-065: padrão recusado, pare */
        }
    }

    jash_strip_cpy(result, str, str_len);
    return result;
}


/* Minimal glob matcher: returns 1 if pattern fully matches str */
static int subst_glob_match(const char* pat, const char* str) {
    while (*pat) {
        if (*pat == '\\' && pat[1]) {
            /* BUG-134: backslash escapes the next char to a literal match. */
            if (!*str || pat[1] != *str) return 0;
            pat += 2; str++;
        } else if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*str) {
                if (subst_glob_match(pat, str)) return 1;
                str++;
            }
            return subst_glob_match(pat, str);
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else if (*pat == '[' && strchr(pat + 1, ']')) {
            /* BUG-134: unterminated `[` is a literal (handled by else). */
            if (!*str) return 0;
            const char* p = pat + 1;
            int negate = (*p == '!' || *p == '^');
            if (negate) p++;
            int matched = 0;
            unsigned char tc = (unsigned char)*str;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (tc >= (unsigned char)p[0] && tc <= (unsigned char)p[2]) matched = 1;
                    p += 3;
                } else {
                    if (tc == (unsigned char)*p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (negate ? matched : !matched) return 0;
            pat = p; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == 0;
}

/* BUG-134: prepare a ${var#pat} / ${var/pat/rep} PATTERN for glob matching.
 * Performs quote removal, but a glob metacharacter that was QUOTED ("*", '*')
 * or BACKSLASH-escaped (\*) stays LITERAL — emitted with a leading backslash
 * the matcher reads as "match this char literally". Unquoted metacharacters
 * remain active globs. jash's quote-shield sentinels (0x01–0x04) are literal
 * data, so they are unshielded to their character and kept literal too. */
static void prep_pattern(const char* in, char* out, size_t cap) {
    size_t o = 0; int sq = 0, dq = 0;
    #define PP_EMIT_LIT(ch) do { \
        char _c = (char)(ch); \
        int _meta = (_c == '*' || _c == '?' || _c == '[' || _c == '\\'); \
        if (_meta && o + 1 < cap) out[o++] = '\\'; \
        if (o + 1 < cap) out[o++] = _c; \
    } while (0)
    for (const char* p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == 0x01) { PP_EMIT_LIT('$');  continue; }  /* shielded $  */
        if (c == 0x02) { PP_EMIT_LIT('`');  continue; }  /* shielded `  */
        if (c == 0x03) { PP_EMIT_LIT('"');  continue; }  /* shielded "  */
        if (c == 0x04) { PP_EMIT_LIT('\''); continue; }  /* shielded '  */
        if (c == 0x05) { PP_EMIT_LIT('\\'); continue; }  /* shielded \ (BUG-097) */
        /* JINV-096: o metacaractere blindado tambem e DADO literal aqui — um
         * padrao `${v#*|}` montado de um valor tem que casar o `|` de verdade.
         * (Nao sao metacaracteres de GLOB, entao saem sem barra.) */
        if (c == 0x12) { PP_EMIT_LIT('|');  continue; }
        if (c == 0x13) { PP_EMIT_LIT('<');  continue; }
        if (c == 0x14) { PP_EMIT_LIT('>');  continue; }
        if (c == 0x15) { PP_EMIT_LIT('&');  continue; }
        if (c == 0x16) { PP_EMIT_LIT(';');  continue; }
        if (c == 0x17) { PP_EMIT_LIT('(');  continue; }
        if (c == 0x18) { PP_EMIT_LIT(')');  continue; }
        if (c == 0x19) { PP_EMIT_LIT('~');  continue; }
        if (!sq && c == '\\' && p[1]) { p++; PP_EMIT_LIT(*p); continue; }
        if (!dq && c == '\'') { sq = !sq; continue; }
        if (!sq && c == '"')  { dq = !dq; continue; }
        if (sq || dq) { PP_EMIT_LIT((char)c); continue; }  /* quoted → literal */
        if (o + 1 < cap) out[o++] = (char)c;               /* unquoted → active */
    }
    out[o] = 0;
    #undef PP_EMIT_LIT
}

/* Find the leftmost, longest glob match of pat in str starting at position start.
 * Sets *match_start and *match_len. Returns 1 on success, 0 if no match. */
static int subst_find_match(const char* str, const char* pat, size_t from,
                             size_t* match_start, size_t* match_len) {
    size_t slen = strlen(str);
    /* A `\` marks an escaped-literal metachar (BUG-134); force the escape-aware
     * glob path rather than the fast literal strstr, which can't read escapes. */
    int has_glob = (strchr(pat, '*') || strchr(pat, '?') ||
                    strchr(pat, '[') || strchr(pat, '\\'));

    if (!has_glob) {
        /* Literal pattern: fast strstr */
        const char* m = strstr(str + from, pat);
        if (!m) return 0;
        *match_start = (size_t)(m - str);
        *match_len = strlen(pat);
        return 1;
    }

    /* Glob pattern: try each start position, find leftmost then longest */
    for (size_t s = from; s <= slen; s++) {
        /* Try lengths from slen-s down to 0 for greedy matching */
        for (size_t l = slen - s + 1; l > 0; l--) {
            /* Build substring */
            char sub[1024];
            if (l >= sizeof sub) continue;
            memcpy(sub, str + s, l);
            sub[l] = 0;
            if (subst_glob_match(pat, sub)) {
                *match_start = s;
                *match_len = l;
                return 1;
            }
        }
        /* Also try empty match at this position */
        if (subst_glob_match(pat, "")) {
            *match_start = s;
            *match_len = 0;
            return 1;
        }
    }
    return 0;
}

/* Perform string substitution (${var/old/new}) */
static void jash_substitute_inplace(char* dest, size_t dest_size, const char* str,
                                     const char* old, const char* new, int replace_all) {
    if (!str || !old || strlen(old) == 0) {
        if (dest_size > 0) {
            strncpy(dest, str ? str : "", dest_size - 1);
            dest[dest_size - 1] = '\0';
        }
        return;
    }

    size_t out_pos = 0;
    size_t from = 0;
    size_t slen = strlen(str);

    while (from <= slen && out_pos < dest_size - 1) {
        size_t ms, ml;
        if (!subst_find_match(str, old, from, &ms, &ml)) {
            /* No more matches, copy remainder */
            for (size_t i = from; i < slen && out_pos < dest_size - 1; i++)
                dest[out_pos++] = str[i];
            break;
        }

        /* Copy text before match */
        for (size_t i = from; i < ms && out_pos < dest_size - 1; i++)
            dest[out_pos++] = str[i];

        /* Copy replacement */
        if (new) {
            for (size_t i = 0; new[i] && out_pos < dest_size - 1; i++)
                dest[out_pos++] = new[i];
        }

        from = ms + (ml == 0 ? 1 : ml);
        if (ml == 0 && from <= slen && out_pos < dest_size - 1)
            dest[out_pos++] = str[from - 1]; /* advance one char on zero-len match */

        if (!replace_all) {
            for (size_t i = (ml == 0 ? from : ms + ml); i < slen && out_pos < dest_size - 1; i++)
                dest[out_pos++] = str[i];
            break;
        }
    }

    dest[out_pos] = 0;
}

/* Perform string slicing (${var:start:length}) */
static void jash_slice_inplace(char* dest, size_t dest_size, const char* str, int start, int length) {
    if (!str) {
        if (dest_size > 0) dest[0] = 0;
        return;
    }
    
    int str_len = (int)strlen(str);
    
    /* Handle negative start (from end) */
    if (start < 0) {
        start = str_len + start;
    }
    
    /* Clamp start */
    if (start < 0) start = 0;
    if (start > str_len) start = str_len;
    
    /* If length not specified, use rest of string */
    if (length < 0) {
        length = str_len - start;
    }
    
    /* Clamp length */
    if (start + length > str_len) {
        length = str_len - start;
    }
    
    if (length <= 0) {
        if (dest_size > 0) dest[0] = 0;
        return;
    }
    
    if ((size_t)length >= dest_size) {
        length = (int)dest_size - 1;
    }
    
    strncpy(dest, str + start, length);
    dest[length] = 0;
}

/* ---- array-aware expansions ----
 *
 * Arrays are stored as flat env keys of the form  base[index].
 * This handles the collective forms that a plain env_get cannot:
 *   ${arr[@]} ${arr[*]}   -> all values, space-joined
 *   ${!arr[@]}            -> all indices/keys, space-joined
 *   ${#arr[@]} ${#arr[*]} -> element count
 *   ${#name}              -> length of scalar (or of element name[i])
 *   ${!name}              -> indirect expansion (value of the var named by name)
 *
 * `full` is the entire inner body between ${ and }.  Returns 1 if it
 * produced output in `out`, 0 if `full` is not one of these forms (caller
 * falls through to the generic operator path). */
static int jash_positional_join(jash_state_t* s, char sep, char* out, size_t cap);
static size_t jash_positional_join_required(jash_state_t* s, char sep);

static const char* jash_special_var(jash_state_t* s, const char* name,
                                    char* buf, size_t cap) {
    if (!name || !buf || cap == 0) return NULL;
    if (strcmp(name, "BASHPID") == 0 || strcmp(name, "BASH_PID") == 0) {
        snprintf(buf, cap, "%ld", s ? s->pid : 0);
        return buf;
    }
    /* BUG-168: $PPID was never registered — $$ works (s->pid, handled as a
     * single-character special elsewhere) but its named sibling $PPID had no
     * handler at all here, so it silently expanded to empty. */
    if (strcmp(name, "PPID") == 0) {
        snprintf(buf, cap, "%ld", (long)getppid());
        return buf;
    }
    if (strcmp(name, "BASH_SOURCE") == 0) {
        strncpy(buf, "jash", cap - 1);
        buf[cap - 1] = 0;
        return buf;
    }
    if (strcmp(name, "BASH_LINENO") == 0 || strcmp(name, "LINENO") == 0) {
        snprintf(buf, cap, "%d", jash_get_lineno());
        return buf;
    }
    if (strcmp(name, "BASH_SUBSHELL") == 0) {
        strncpy(buf, "0", cap - 1);
        buf[cap - 1] = 0;
        return buf;
    }
    if (strcmp(name, "BASH_VERSION") == 0) {
        /* Do NOT report a bash version. Scripts that test
         * `[[ "${BASH_VERSION%%.*}" -ge 4 ]]` would otherwise see jash's
         * 0.1 and disable bash-4 features even when running under bash.
         * Empty signals "not bash" which is the truthful answer. */
        return NULL;
    }
    /* $SECONDS — elapsed seconds since shell start */
    if (strcmp(name, "SECONDS") == 0) {
        time_t now = time(NULL);
        if (!g_seconds_base) g_seconds_base = now;
        snprintf(buf, cap, "%ld", (long)(now - g_seconds_base));
        return buf;
    }
    /* $RANDOM — 0..32767 pseudo-random, re-generated each read (bash compat) */
    if (strcmp(name, "RANDOM") == 0) {
        if (!g_rand_seeded) {
            srand((unsigned)(time(NULL) ^ (s ? (unsigned long)s->pid : 0)));
            g_rand_seeded = 1;
        }
        snprintf(buf, cap, "%d", rand() & 0x7fff);
        return buf;
    }
    /* $SRANDOM — 32-bit cryptographic random (bash 5.1+) */
    if (strcmp(name, "SRANDOM") == 0) {
#if defined(__APPLE__) || defined(__FreeBSD__)
        snprintf(buf, cap, "%u", arc4random());
#else
        /* Fallback: two RAND_MAX-range values stitched together */
        if (!g_rand_seeded) {
            srand((unsigned)(time(NULL) ^ (s ? (unsigned long)s->pid : 0)));
            g_rand_seeded = 1;
        }
        unsigned v = ((unsigned)rand() << 16) ^ (unsigned)rand();
        snprintf(buf, cap, "%u", v);
#endif
        return buf;
    }
    return NULL;
}

static int jash_str_ends(const char* s, const char* suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Collect array elements for base name into out, joined by space.
 * want_keys: 0 = values, 1 = keys.  Returns element count. */
/* BUG-104: public element-count wrapper for the arithmetic parser (${#a[@]}
 * inside $(( )) — arithmetic.c must not re-enter jash_param_expand, whose
 * static result buffer may hold an in-progress outer expansion). */
static int jash_array_collect(jash_state_t* s, const char* base,
                              char* out, size_t cap, int want_keys, int at_sign, int in_dq);
/* BUG-065: contar NAO pode depender de caber num buffer.
 *
 * Era `char tmp[1024]` — a contagem coletava os elementos e devolvia quantos
 * couberam. Com 300 elementos de ~4 bytes cada, 1024/4 = 256, e era exatamente
 * isso que `${#A[@]}` respondia enquanto `${A[299]}` devolvia o elemento sem
 * hesitar: o armazenamento tinha os 300, a leitura mentia.
 *
 * O buffer maior nao e a correcao — seria o mesmo defeito com outro numero.
 * jash_array_collect aceita out == NULL: nesse modo so conta, sem escrever em
 * lugar nenhum, e o tamanho do array deixa de ter teto de bytes.
 * F E R M I ∞ H A R T */
int jash_array_count(jash_state_t* s, const char* base) {
    return jash_array_collect(s, base, NULL, 0, 0, 0, 0);
}

/* out == NULL  =>  modo SO-CONTAGEM: percorre e devolve `count` sem escrever
 * em lugar nenhum. E como jash_array_count() pergunta o tamanho sem depender de
 * o resultado caber num buffer (BUG-065). */
static int jash_array_collect(jash_state_t* s, const char* base,
                              char* out, size_t cap, int want_keys, int at_sign, int in_dq) {
    size_t base_len = strlen(base);
    if (!out) cap = 0;                    /* nenhuma escrita cabe: so conta */
    /* Gather matching entries.
     *
     * BUG-065: JA_MAX era 256 enquanto o store (MAX_ARRAY_ENTRIES) aceita 4096.
     * Um array de 300 era guardado inteiro e LIDO pela metade — `${#A[@]}` dizia
     * 256, `for x in "${A[@]}"` processava 256 e terminava rc 0. Dimensionado
     * pelo store: enquanto ele aceitar 4096, a leitura nao pode enxergar menos.
     *
     * Heap, nao pilha: o buffer cresce com o numero de matches (dobrando,
     * com teto JA_MAX). O modo so-contagem nao aloca nem ordena. */
    #define JA_MAX 4096   /* == MAX_ARRAY_ENTRIES do store */
    typedef struct {
        const char* key;
        const char* val;
        long numeric_index;
        int env_index;
    } jash_array_item_t;
    size_t jcap = 0;
    jash_array_item_t* items = NULL;
    int allnum = 1, count = 0;
    jash_env_array_iter_t iter;
    jash_env_array_iter_init(&iter, s, base);
    for (int i; count < JA_MAX && (i = jash_env_array_iter_next(&iter)) >= 0; ) {
        if (!out) {
            count++;
            continue;
        }
        const char *k = 0, *v = 0;
        if (!jash_env_at(s, (uint32_t)i, &k, &v)) continue;
        size_t kl = strlen(k);
        if (kl <= base_len + 2 || k[kl - 1] != ']') continue;
        if ((size_t)count == jcap) {
            size_t ncap = jcap * 2;
            if (ncap == 0) ncap = 16;
            if (ncap > JA_MAX) ncap = JA_MAX;
            jash_array_item_t* grown = (jash_array_item_t*)realloc(
                items, sizeof(*items) * ncap);
            if (!grown) {
                free(items);
                jash_writef(jash_stderr_writer, 0,
                    "jash: ${%s[...]}: sem memoria para os elementos — "
                    "leitura recusada, nao truncada\n", base);
                return 0;
            }
            items = grown;
            jcap = ncap;
        }
        /* parse numeric index between the brackets */
        const char* idx = k + base_len + 1;
        unsigned long magnitude = 0;
        int neg = 0, digits = 0, overflow = 0;
        const char* q = idx;
        if (*q == '-') { neg = 1; q++; }
        unsigned long limit = neg ? (unsigned long)LONG_MAX + 1UL :
                                    (unsigned long)LONG_MAX;
        while (q < k + kl - 1 && *q >= '0' && *q <= '9') {
            unsigned long digit = (unsigned long)(*q - '0');
            if (magnitude > (limit - digit) / 10UL) overflow = 1;
            else if (!overflow) magnitude = magnitude * 10UL + digit;
            q++;
            digits = 1;
        }
        int numeric = digits && !overflow && q == k + kl - 1;
        items[count].key = k;
        items[count].val = v;
        items[count].numeric_index = numeric ?
            (neg ? (magnitude == (unsigned long)LONG_MAX + 1UL ?
                    LONG_MIN : -(long)magnitude) : (long)magnitude) : 0;
        items[count].env_index = i;
        if (!numeric) allnum = 0;
        count++;
    }
    if (!out) return count;
    /* Numeric arrays use index order. Associative/mixed arrays retain the old
     * env-table order even though the auxiliary chain is newest-first. */
    {
        for (int a = 1; a < count; a++) {
            jash_array_item_t item = items[a];
            int b = a - 1;
            while (b >= 0 && (allnum ?
                   (items[b].numeric_index > item.numeric_index ||
                    (items[b].numeric_index == item.numeric_index &&
                     items[b].env_index > item.env_index)) :
                   items[b].env_index > item.env_index)) {
                items[b + 1] = items[b];
                b--;
            }
            items[b + 1] = item;
        }
    }
    size_t op = 0;
    /* Use \x1E (ASCII Record Separator) inside DQ with [@] to mark word boundaries
     * between array elements. \x01 cannot be used — it's the cmdsub protected-dollar
     * sentinel and gets converted back to '$' by jash_cmdsub_unprotect(). */
    char sep;
    if (at_sign && in_dq) {
        sep = '\x1E';  /* "${arr[@]}" — each element a separate word */
    } else if (!at_sign && in_dq) {
        /* "${arr[*]}" — join with first char of IFS (bash semantics) */
        const char* ifs_v = s ? jash_env_get(s, "IFS") : NULL;
        sep = ifs_v ? *ifs_v : ' '   /* BUG-117c: set-but-empty IFS -> 0 = no separator */;
    } else {
        sep = ' ';
    }
    for (int a = 0; a < count; a++) {
        if (a > 0 && sep && op + 1 < cap) out[op++] = sep;
        if (want_keys) {
            /* index text is between the brackets of items[a].key */
            const char* k = items[a].key; size_t kl = strlen(k);
            const char* idx = k + base_len + 1;
            const char* idxend = k + kl - 1;
            for (const char* q = idx; q < idxend && op + 1 < cap; q++) out[op++] = *q;
        } else {
            for (const char* vp = items[a].val; *vp && op + 1 < cap; vp++) out[op++] = *vp;
        }
    }
    if (out) out[op] = 0;
    free(items);
    return count;
    #undef JA_MAX
}

/* Forward declaration: needed by jash_expand_array_form for operator-per-element. */
static void jash_apply_expansion(jash_state_t* s, const char* var_name,
                                  char* result, size_t result_size,
                                  const char* var_value, int var_is_set,
                                  const char* op_str);

/* Returns 1 if `full` (inner ${...} body) is an array/count/indirect form
 * and writes the result to out. */
static int jash_expand_array_form(jash_state_t* s, const char* full,
                                  char* out, size_t cap, int in_dq) {
    if (!full || !*full) return 0;

    /* ${#} ${*} ${@} — positional count / list (braced forms). */
    if (full[0] == '#' && full[1] == 0) {
        snprintf(out, cap, "%d", jash_positional_join(s, ' ', NULL, 0));
        return 1;
    }
    if ((full[0] == '*' || full[0] == '@') && full[1] == 0) {
        jash_positional_join(s, ' ', out, cap);
        return 1;
    }

    /* BUG-133: ${*:-word} ${@:-word} ${*:+word} ${*-word} ${*+word} — the
     * default/alternate modifiers applied to $* / $@. Must be handled BEFORE
     * the `:` slice branch below: a `:` followed by one of -+=? is a modifier,
     * not a slice offset. Without this, `${*:-DEF}` fell into the slice parser
     * (`-DEF` read as a bogus offset) and printed the wrong thing. */
    if (full[0] == '*' || full[0] == '@') {
        const char* op = full + 1;
        int colon = (*op == ':' &&
                     (op[1] == '-' || op[1] == '+' || op[1] == '=' || op[1] == '?'));
        int bare  = ((*op == '-' || *op == '+' || *op == '=' || *op == '?') && op[1]);
        if (colon || bare) {
            char opc = colon ? op[1] : op[0];
            const char* word = colon ? op + 2 : op + 1;
            int pc = jash_positional_join(s, ' ', NULL, 0);
            size_t joined_len = pc > 0 ? jash_positional_join_required(s, ' ') : 0;
            int is_set   = (pc > 0);
            int nonempty = (joined_len != 0);
            /* with `:` the test is emptiness; without, it is set-ness */
            int present  = colon ? nonempty : is_set;
            switch (opc) {
                case '-':
                    if (present) jash_positional_join(s, ' ', out, cap);
                    else copy_expanded_text(s, word, out, cap);
                    return 1;
                case '+':
                    if (present) copy_expanded_text(s, word, out, cap);
                    else out[0] = 0;
                    return 1;
                case '?':
                    if (present) jash_positional_join(s, ' ', out, cap);
                    else {
                        snprintf(g_param_error_msg, sizeof(g_param_error_msg),
                                 "%c: %s", full[0], word);
                        g_param_error = 1;
                        out[0] = 0;
                    }
                    return 1;
                /* assignment (=) is not valid on the positional list in bash;
                 * fall through to the slice/other handling for anything else. */
            }
        }
    }

    /* ${@:start} / ${@:start:count} / ${*:start} — positional slice */
    if ((full[0] == '@' || full[0] == '*') && full[1] == ':') {
        int is_at = (full[0] == '@');
        const char* sp = full + 2;
        /* Handle negative offset syntax: ${@: -N} or ${@:(-N)} */
        int neg_start = 0;
        while (*sp == ' ' || *sp == '\t') sp++;
        if (*sp == '(') sp++;
        if (*sp == '-') { neg_start = 1; sp++; }
        int start = 0;
        while (*sp >= '0' && *sp <= '9') { start = start * 10 + (*sp - '0'); sp++; }
        if (*sp == ')') sp++;
        int pcount = jash_positional_join(s, '\0', NULL, 0);
        if (neg_start) start = pcount - start;
        if (start < 0) start = 0;
        int slice_count = -1;
        if (*sp == ':') {
            sp++;
            slice_count = 0;
            while (*sp >= '0' && *sp <= '9') { slice_count = slice_count * 10 + (*sp - '0'); sp++; }
        }

        /* Collect positionals, picking [start..start+count) (1-based: start=1 → $1) */
        char out_sep;
        if (is_at && in_dq) {
            out_sep = '\x1E';
        } else if (!is_at && in_dq) {
            const char* ifs_v = s ? jash_env_get(s, "IFS") : NULL;
            out_sep = ifs_v ? *ifs_v : ' '   /* BUG-117c: set-but-empty IFS -> 0 = no separator */;
        } else {
            out_sep = ' ';
        }
        size_t op = 0; int emitted = 0;
        for (int pi = 1; pi <= JASH_POS_MAX; pi++) {
            char pk[8]; snprintf(pk, sizeof pk, "%d", pi);
            const char* pv = s ? jash_env_get(s, pk) : NULL;
            if (!pv) break;
            if (pi >= start && (slice_count < 0 || emitted < slice_count)) {
                if (emitted > 0 && out_sep) {
                    if (op + 1 >= cap) { positional_refuse_output(); out[0] = 0; return 1; }
                    out[op++] = out_sep;
                }
                for (const char* pq = pv; *pq; pq++) {
                    if (op + 1 >= cap) { positional_refuse_output(); out[0] = 0; return 1; }
                    out[op++] = *pq;
                }
                emitted++;
            }
        }
        out[op] = 0;
        return 1;
    }

    /* ${#...} */
    if (full[0] == '#') {
        const char* inner = full + 1;
        if (!*inner) return 0; /* ${#} = $# arg count, handled elsewhere */
        /* BUG-136: ${#@} / ${#*} = number of positional parameters ( = $# ). */
        if ((inner[0] == '@' || inner[0] == '*') && inner[1] == '\0') {
            int c = jash_positional_join(s, ' ', NULL, 0);
            snprintf(out, cap, "%d", c);
            return 1;
        }
        if (jash_str_ends(inner, "[@]") || jash_str_ends(inner, "[*]")) {
            /* element count */
            char base[64]; size_t bl = strlen(inner) - 3;
            if (bl >= sizeof base) bl = sizeof base - 1;
            memcpy(base, inner, bl); base[bl] = 0;
            char tmp[1024];
            int c = jash_array_collect(s, base, tmp, sizeof tmp, 0, 0, in_dq);
            snprintf(out, cap, "%d", c);
            return 1;
        }
        /* ${#name} or ${#name[idx]} — length of that value */
        char special[128];
        const char* v = jash_special_var(s, inner, special, sizeof special);
        if (!v) v = jash_env_get(s, inner);
        if (!v && s) {
            /* BUG-085: ${#name} on an indexed array is ${#name[0]} in bash —
             * same fallback the bare-$a fix (BUG-077) gave value lookups. */
            int ident = inner[0] != '\0';
            for (const char* q = inner; *q; q++)
                if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||
                      (*q>='0'&&*q<='9')||*q=='_')) { ident = 0; break; }
            if (ident) {
                char e0[80];
                snprintf(e0, sizeof(e0), "%s[0]", inner);
                v = jash_env_get(s, e0);
            }
        }
        snprintf(out, cap, "%zu", v ? strlen(v) : 0u);
        return 1;
    }

    /* ${!...} */
    if (full[0] == '!') {
        const char* inner = full + 1;
        if (!*inner) return 0;
        if (jash_str_ends(inner, "[@]") || jash_str_ends(inner, "[*]")) {
            int is_at = jash_str_ends(inner, "[@]");
            char base[64]; size_t bl = strlen(inner) - 3;
            if (bl >= sizeof base) bl = sizeof base - 1;
            memcpy(base, inner, bl); base[bl] = 0;
            jash_array_collect(s, base, out, cap, 1 /* keys */, is_at, in_dq);
            return 1;
        }

        /* ${!prefix*} / ${!prefix@} — variable names beginning with prefix. */
        size_t il = strlen(inner);
        if (il > 0 && (inner[il - 1] == '*' || inner[il - 1] == '@')) {
            char prefix[64];
            size_t pl = il - 1;
            if (pl >= sizeof prefix) pl = sizeof prefix - 1;
            memcpy(prefix, inner, pl);
            prefix[pl] = 0;
            size_t op = 0;
            uint32_t n = s ? jash_env_count(s) : 0;
            for (uint32_t i = 0; i < n; i++) {
                const char *k = 0, *v = 0;
                if (!jash_env_at(s, i, &k, &v) || !k) continue;
                if (strncmp(k, prefix, pl) != 0) continue;
                if (op > 0 && op + 1 < cap) out[op++] = ' ';
                for (const char* q = k; *q && op + 1 < cap; q++) out[op++] = *q;
            }
            out[op] = 0;
            return 1;
        }
        /* ${!name} indirect — value of the variable named by $name.
         * BUG-132: an operator may follow the name (${!v:-def}, ${!v#pat},
         * ${!v/a/b}, …). Bash resolves the indirection to the target NAME,
         * then applies the operator to that target. Split the name (a valid
         * identifier) from the operator suffix and dispatch the suffix through
         * jash_apply_expansion against the resolved target's value. */
        char iname[128]; unsigned ni = 0;
        const char* iq = inner;
        while (*iq && (isalnum((unsigned char)*iq) || *iq == '_') && ni + 1 < sizeof(iname))
            iname[ni++] = *iq++;
        iname[ni] = 0;
        const char* lvl1 = (ni > 0) ? jash_env_get(s, iname) : NULL;
        if (!lvl1) { out[0] = 0; return 1; }
        if (*iq) {
            /* operator suffix present — apply it to ${lvl1<op>} */
            const char* tv = jash_env_get(s, lvl1);
            strncpy(out, tv ? tv : "", cap - 1);
            out[cap - 1] = 0;
            jash_apply_expansion(s, lvl1, out, cap, tv ? tv : "", tv != NULL, iq);
            return 1;
        }
        const char* lvl2 = jash_env_get(s, lvl1);
        strncpy(out, lvl2 ? lvl2 : "", cap - 1);
        out[cap - 1] = 0;
        return 1;
    }

    /* ${arr[@]/pat/repl}, ${arr[@]^^}, ${arr[@]#pat}, etc.
     * Operator applied element-wise to every array member. */
    {
        const char* at_pos  = strstr(full, "[@]");
        const char* star_pos = strstr(full, "[*]");
        const char* bp = NULL; int is_at_op = 0;
        if (at_pos  && (!star_pos || at_pos  < star_pos)) { bp = at_pos;  is_at_op = 1; }
        else if (star_pos)                                  { bp = star_pos; is_at_op = 0; }

        /* Slice syntax ${arr[@]:N:M} — digit immediately after : — is handled
         * by the element-slice block below; skip the operator path. */
        int is_arr_slice = (bp && bp[3] == ':' &&
                            (bp[4] >= '0' && bp[4] <= '9'));
        if (bp && bp[3] != '\0' && !is_arr_slice) {
            /* Has operator suffix after [@] or [*] */
            const char* op = bp + 3;
            char base[64];
            size_t bl = (size_t)(bp - full);
            if (bl >= sizeof base) bl = sizeof base - 1;
            memcpy(base, full, bl); base[bl] = 0;

            char out_sep;
            if (is_at_op && in_dq) {
                out_sep = '\x1E';
            } else if (!is_at_op && in_dq) {
                const char* ifs_v = s ? jash_env_get(s, "IFS") : NULL;
                out_sep = ifs_v ? *ifs_v : ' '   /* BUG-117c: set-but-empty IFS -> 0 = no separator */;
            } else {
                out_sep = ' ';
            }

            /* Collect numeric indices in order.
             *
             * O teto era `int indices[256]` com o laco parando em nidx < 256, e
             * o efeito nao era um erro: era uma CONTAGEM ERRADA. Medido em
             * 2026-08-05 — `A=($(seq 1 300)); echo ${#A[@]}` respondia 256
             * enquanto `${A[299]}` devolvia 300 sem hesitar. O armazenamento
             * tinha os 300 (MAX_ARRAY_ENTRIES = 4096); so a leitura mentia.
             * Um `for` sobre esse array processava 256 de 300 e terminava rc 0.
             *
             * Dimensionado pelo store, nao por um numero solto: enquanto o
             * armazenamento aceitar 4096, a leitura nao pode enxergar menos.
             * Heap porque 4096 ints sao 16 KB — pilha demais para uma funcao
             * de expansao que reentra. F E R M I ∞ H A R T */
            /* Na pilha e nao no heap: sao 16 KB, e este caminho tem 13 pontos
             * de retorno — um malloc aqui precisaria de free em todos, e a
             * primeira tentativa desta correcao vazou em quatro deles e nem
             * compilou. 16 KB de pilha e mais barato que 13 chances de errar. */
            #define JASH_ARR_IDX_MAX 4096   /* == MAX_ARRAY_ENTRIES do store */
            int indices[JASH_ARR_IDX_MAX];
            int nidx = 0;
            size_t base_len = strlen(base);
            jash_env_array_iter_t iter;
            jash_env_array_iter_init(&iter, s, base);
            for (int i; nidx < JASH_ARR_IDX_MAX &&
                        (i = jash_env_array_iter_next(&iter)) >= 0; ) {
                const char *k = 0, *v2 = 0;
                if (!jash_env_at(s, (uint32_t)i, &k, &v2)) continue;
                size_t kl = strlen(k);
                if (kl > base_len + 2 && k[kl-1] == ']') {
                    const char* is2 = k + base_len + 1;
                    int num = 0; int dig = 0; int overflow = 0;
                    while (is2 < k + kl - 1 && *is2 >= '0' && *is2 <= '9') {
                        int digit = *is2++ - '0';
                        if (num > (INT_MAX - digit) / 10) overflow = 1;
                        else if (!overflow) num = num * 10 + digit;
                        dig = 1;
                    }
                    if (dig && !overflow && is2 == k + kl - 1) {
                        /* insertion sort */
                        int ins = nidx++;
                        while (ins > 0 && indices[ins-1] > num) {
                            indices[ins] = indices[ins-1]; ins--;
                        }
                        indices[ins] = num;
                    }
                }
            }

            /* ${arr[@]@A}: full assignment representation, not element-wise.
             * Values are single-quoted via @Q to avoid jash stripping " chars. */
            if (op[0] == '@' && op[1] == 'A' && op[2] == '\0') {
                size_t op2 = 0;
                const char pfx[] = "declare -a ";
                for (const char* q = pfx; *q && op2 < cap-1; ) out[op2++] = *q++;
                for (const char* q = base; *q && op2 < cap-1; ) out[op2++] = *q++;
                if (op2 < cap-1) out[op2++] = '=';
                if (op2 < cap-1) out[op2++] = '(';
                for (int ai = 0; ai < nidx && op2 < cap-1; ai++) {
                    char key[128]; snprintf(key, sizeof key, "%s[%d]", base, indices[ai]);
                    const char* elem = s ? jash_env_get(s, key) : NULL; if (!elem) elem = "";
                    char qval[256];
                    jash_apply_expansion(s, base, qval, sizeof(qval), elem, 1, "@Q");
                    char entry[320]; snprintf(entry, sizeof entry, "[%d]=%s", indices[ai], qval);
                    if (ai > 0 && op2 < cap-1) out[op2++] = ' ';
                    for (const char* q = entry; *q && op2 < cap-1; ) out[op2++] = *q++;
                }
                if (op2 < cap-1) out[op2++] = ')';
                out[op2] = 0;
                return 1;
            }

            /* BUG-110: zero elements — bash treats @ and star subscripts on
             * an unset/empty array as UNSET for the value operators, so
             * ${a[@]:-word} yields word. Route through the scalar operator
             * machinery with an unset empty value (also := / :? / :+). */
            if (nidx == 0) {
                jash_apply_expansion(s, base, out, cap, "", 0, op);
                return 1;
            }

            size_t op2 = 0;
            for (int ai = 0; ai < nidx && op2 < cap - 1; ai++) {
                char key[128];
                snprintf(key, sizeof key, "%s[%d]", base, indices[ai]);
                const char* elem = s ? jash_env_get(s, key) : NULL;
                if (!elem) elem = "";
                char elem_r[512];
                jash_apply_expansion(s, base, elem_r, sizeof elem_r, elem, 1, op);
                if (ai > 0 && out_sep && op2 < cap - 1) out[op2++] = out_sep;
                for (const char* q = elem_r; *q && op2 < cap - 1; op2++) out[op2] = *q++;
            }
            out[op2] = 0;
            return 1;
        }
    }

    /* ${arr[@]} / ${arr[*]} — all values; optional :start:count slice */
    if (jash_str_ends(full, "[@]") || jash_str_ends(full, "[*]")) {
        char base[64]; size_t bl = strlen(full) - 3;
        if (bl >= sizeof base) bl = sizeof base - 1;
        memcpy(base, full, bl); base[bl] = 0;

        /* Special variables that act as single-element pseudo-arrays. */
        if (strcmp(base, "BASH_SOURCE") == 0) {
            strncpy(out, "jash", cap - 1);
            out[cap - 1] = 0;
            return 1;
        }
        if (strcmp(base, "BASH_LINENO") == 0 || strcmp(base, "LINENO") == 0) {
            strncpy(out, "0", cap - 1);
            out[cap - 1] = 0;
            return 1;
        }

        jash_array_collect(s, base, out, cap, 0 /* values */, jash_str_ends(full, "[@]"), in_dq);
        return 1;
    }

    /* ${arr[N]} — specific numeric index (e.g. ${BASH_SOURCE[0]}) */
    {
        size_t fl = strlen(full);
        if (fl > 3 && full[fl - 1] == ']') {
            /* Find the last '[' */
            const char* lb = NULL;
            for (size_t i = fl - 2; i > 0; i--) {
                if (full[i] == '[') { lb = full + i; break; }
            }
            if (lb) {
                /* Guard: base (before '[') must be a valid shell identifier.
                 * If it contains '^', ',', '/', '%', '#', etc. then 'full'
                 * is an operator+pattern expression like "x^^[aeiou]", not an
                 * array subscript expression. */
                int base_ok = 1;
                for (const char* bq = full; bq < lb; bq++) {
                    unsigned char bc = (unsigned char)*bq;
                    if (bc != '_' && !isalnum(bc)) { base_ok = 0; break; }
                }
                if (!base_ok) goto after_arr_subscript;

                /* Check index is purely numeric (positive or negative) */
                const char* idx = lb + 1;
                const char* idxend = full + fl - 1;
                int is_neg = (idx < idxend && *idx == '-');
                const char* idx_start = is_neg ? idx + 1 : idx;
                if (idx_start < idxend && *idx_start >= '0' && *idx_start <= '9') {
                    int all_digits = 1;
                    for (const char* q = idx_start; q < idxend; q++) {
                        if (*q < '0' || *q > '9') { all_digits = 0; break; }
                    }
                    if (all_digits) {
                        /* Parse the magnitude */
                        long idx_mag = 0;
                        for (const char* q = idx_start; q < idxend; q++)
                            idx_mag = idx_mag * 10 + (*q - '0');

                        char base[64];
                        size_t bl = (size_t)(lb - full);
                        if (bl >= sizeof base) bl = sizeof base - 1;
                        memcpy(base, full, bl); base[bl] = 0;

                        /* Pseudo-array special variables. */
                        if (strcmp(base, "BASH_SOURCE") == 0) {
                            strncpy(out, "jash", cap - 1);
                            out[cap - 1] = 0;
                            return 1;
                        }
                        if (strcmp(base, "BASH_LINENO") == 0 || strcmp(base, "LINENO") == 0) {
                            strncpy(out, "0", cap - 1);
                            out[cap - 1] = 0;
                            return 1;
                        }

                        int actual_idx = (int)idx_mag;
                        if (is_neg) {
                            /* Negative index: count from end.
                             * Find the highest numeric index in the array. */
                            int max_idx = -1;
                            jash_env_array_iter_t iter;
                            jash_env_array_iter_init(&iter, s, base);
                            for (int i; (i = jash_env_array_iter_next(&iter)) >= 0; ) {
                                const char *k = 0, *v2 = 0;
                                if (!jash_env_at(s, (uint32_t)i, &k, &v2)) continue;
                                size_t kl = strlen(k);
                                if (kl > bl + 2 && k[kl - 1] == ']') {
                                    const char* is2 = k + bl + 1;
                                    int num = 0, overflow = 0;
                                    while (is2 < k + kl - 1 && *is2 >= '0' && *is2 <= '9') {
                                        int digit = *is2++ - '0';
                                        if (num > (INT_MAX - digit) / 10) overflow = 1;
                                        else if (!overflow) num = num * 10 + digit;
                                    }
                                    if (!overflow && is2 == k + kl - 1 && num > max_idx)
                                        max_idx = num;
                                }
                            }
                            actual_idx = max_idx + 1 - (int)idx_mag;
                            if (actual_idx < 0) { out[0] = 0; return 1; }
                        }

                        /* Look up env key: base[actual_idx] */
                        char lookup_key[128];
                        snprintf(lookup_key, sizeof(lookup_key), "%s[%d]", base, actual_idx);
                        const char* v = jash_env_get(s, lookup_key);
                        if (v) {
                            strncpy(out, v, cap - 1);
                            out[cap - 1] = 0;
                        } else {
                            out[0] = 0;
                        }
                        return 1;
                    }
                } else {
                    /* Non-numeric subscript. Handles both associative-array
                     * string keys (${map[key]}) and arithmetic expressions
                     * (${arr[$i]}, ${arr[i+1]}, ${arr[$((expr))]}).
                     *
                     * Strategy: if the subscript is a plain identifier
                     * (all word-chars, no $) AND that literal key exists in
                     * the env, treat it as a string lookup (associative array).
                     * Otherwise evaluate as arithmetic. */
                    char idx_expr[128];
                    size_t idx_len = (size_t)(idxend - idx);
                    if (idx_len > 0 && idx_len < sizeof(idx_expr)) {
                        memcpy(idx_expr, idx, idx_len);
                        idx_expr[idx_len] = 0;

                        char assoc_base[64];
                        size_t abl2 = (size_t)(lb - full);
                        if (abl2 >= sizeof assoc_base) abl2 = sizeof assoc_base - 1;
                        memcpy(assoc_base, full, abl2); assoc_base[abl2] = 0;

                        /* Check if subscript is a plain word (identifier or
                         * quoted string without $ / operators) */
                        int is_plain_key = 1;
                        for (size_t qi = 0; qi < idx_len; qi++) {
                            unsigned char qc = (unsigned char)idx_expr[qi];
                            if (qc != '_' && !isalnum(qc)) { is_plain_key = 0; break; }
                        }

                        if (is_plain_key) {
                            /* Try literal string key first (associative array) */
                            char str_lk[sizeof assoc_base + sizeof idx_expr + 2];
                            snprintf(str_lk, sizeof str_lk, "%s[%s]", assoc_base, idx_expr);
                            const char* sv = jash_env_get(s, str_lk);
                            if (sv) {
                                strncpy(out, sv, cap - 1);
                                out[cap - 1] = 0;
                                return 1;
                            }
                            /* Not found as string key — eval as arithmetic
                             * (e.g. bare variable name used as index) */
                        } else if (idx_expr[0] == '$' && idx_len > 1) {
                            /* ${map[$k]} — expand the variable and use its value
                             * as a string key for the associative array */
                            char varname[64];
                            size_t vn_len = idx_len - 1;
                            if (vn_len >= sizeof varname) vn_len = sizeof varname - 1;
                            memcpy(varname, idx_expr + 1, vn_len);
                            varname[vn_len] = 0;
                            const char* kv = s ? jash_env_get(s, varname) : NULL;
                            if (kv && *kv) {
                                /* Try expanded value as string key */
                                char xstr_lk[192];
                                snprintf(xstr_lk, sizeof xstr_lk, "%s[%s]", assoc_base, kv);
                                const char* xsv = jash_env_get(s, xstr_lk);
                                if (xsv) {
                                    strncpy(out, xsv, cap - 1);
                                    out[cap - 1] = 0;
                                    return 1;
                                }
                                /* May also be a numeric index — fall through to arith */
                            }
                        }

                        long arith_idx = jash_eval_arithmetic(idx_expr, s);
                        if (arith_idx >= 0) {
                            char lk[128];
                            snprintf(lk, sizeof lk, "%s[%ld]", assoc_base, arith_idx);
                            const char* av = jash_env_get(s, lk);
                            strncpy(out, av ? av : "", cap - 1);
                            out[cap - 1] = 0;
                            return 1;
                        }
                    }
                }
            }
        }
    }
    after_arr_subscript:;

    /* ${arr[@]:start:count} / ${arr[*]:start:count} — element slice */
    {
        const char* br = strstr(full, "[@]:");
        if (!br) br = strstr(full, "[*]:");
        if (br) {
            char base[64]; size_t bl = (size_t)(br - full);
            if (bl >= sizeof base) bl = sizeof base - 1;
            memcpy(base, full, bl); base[bl] = 0;

            const char* sp = br + 4; /* after "[@]:" */
            int start = 0, count = -1;
            while (*sp >= '0' && *sp <= '9') { start = start * 10 + (*sp - '0'); sp++; }
            if (*sp == ':') { sp++; count = 0; while (*sp >= '0' && *sp <= '9') { count = count * 10 + (*sp - '0'); sp++; } }

            /* BUG-117b: collect with the \x1E element boundary (at=1,in_dq=1)
             * so multi-word elements survive the internal split — the old
             * space-joined collect re-split "a b" into two slice items. */
            char allbuf[1024];
            int is_at = (strstr(full, "[@]:") != NULL);
            jash_array_collect(s, base, allbuf, sizeof allbuf, 0, 1, 1);

            /* determine output separator */
            char out_sep;
            if (is_at && in_dq) {
                out_sep = '\x1E';
            } else if (!is_at && in_dq) {
                const char* ifs_v = s ? jash_env_get(s, "IFS") : NULL;
                out_sep = ifs_v ? *ifs_v : ' '   /* BUG-117c: set-but-empty IFS -> 0 = no separator */;
            } else {
                out_sep = ' ';
            }

            /* split allbuf by \x1E, emit elements [start, start+count) */
            size_t op = 0; int widx = 0, emitted = 0;
            const char* q = allbuf;
            while (*q) {
                const char* ws = q;
                while (*q && *q != '\x1E') q++;
                if (widx >= start && (count < 0 || emitted < count)) {
                    if (emitted > 0 && out_sep && op + 1 < cap) out[op++] = out_sep;
                    for (const char* t = ws; t < q && op + 1 < cap; t++) {
                        char _se = *t;
                        /* BUG-096: shield literal quotes in element values */
                        if (_se == '"')       _se = (char)0x03;
                        else if (_se == '\'') _se = (char)0x04;
                        else if (_se == '\\') _se = (char)0x05;  /* BUG-097 */
                        _se = jash_meta_shield(_se);   /* JINV-096 */
                        out[op++] = _se;
                    }
                    emitted++;
                }
                widx++;
                if (*q == '\x1E') q++;
            }
            out[op] = 0;
            return 1;
        }
    }

    return 0;
}

/* Parse expansion and apply to variable value */
static void jash_apply_expansion(jash_state_t* s, const char* var_name,
                                  char* result, size_t result_size,
                                  const char* var_value, int var_is_set,
                                  const char* op_str);

/* ---- special positional parameters ($#, $*, $@) ----
 * Positionals are stored as env keys "1".."9". Count the leading run of
 * SET positionals (bash sets exactly $1..$N, so the first unset/empty key
 * ends the list). Returns the count; if out!=NULL, joins values by sep. */
static int jash_positional_join(jash_state_t* s, char sep, char* out, size_t cap) {
    int count = 0;
    size_t op = 0;
    if (out && cap) out[0] = 0;

    /* Custom IFS: first char is the separator. BUG-117c: IFS SET-BUT-EMPTY
     * joins with NO separator (POSIX) — only an UNSET IFS falls back to the
     * caller's default. */
    const char* ifs = s ? jash_env_get(s, "IFS") : NULL;
    int no_sep = (ifs && !*ifs);
    char effective_sep = (ifs && *ifs) ? *ifs : sep;

    for (int i = 1; i <= JASH_POS_MAX; i++) {   /* BUG-095: was 9 */
        char key[8]; snprintf(key, sizeof(key), "%d", i);
        const char* v = s ? jash_env_get(s, key) : NULL;
        if (!v) break;
        if (out) {
            if (count > 0 && !no_sep) {
                if (op + 1 >= cap) { positional_refuse_output(); out[0] = 0; return count; }
                out[op++] = effective_sep;
            }
            for (const char* p = v; *p; p++) {
                if (op + 1 >= cap) { positional_refuse_output(); out[0] = 0; return count; }
                out[op++] = *p;
            }
        }
        count++;
    }
    if (out && op < cap) out[op] = 0;
    return count;
}

static size_t jash_positional_join_required(jash_state_t* s, char sep) {
    size_t need = 0;
    int count = 0;
    const char* ifs = s ? jash_env_get(s, "IFS") : NULL;
    int no_sep = (ifs && !*ifs);
    (void)sep;
    for (int i = 1; i <= JASH_POS_MAX; i++) {
        char key[8]; snprintf(key, sizeof key, "%d", i);
        const char* v = s ? jash_env_get(s, key) : NULL;
        if (!v) break;
        if (count > 0 && !no_sep) need++;
        need += strlen(v);
        count++;
    }
    return need;
}

static int jash_append_quoted_positionals(jash_state_t* s, char* out,
                                          size_t* out_pos, size_t cap) {
    size_t saved = *out_pos;
    int emitted = 0;
    for (int i = 1; i <= JASH_POS_MAX; i++) {   /* BUG-095: was 9 */
        char key[8]; snprintf(key, sizeof(key), "%d", i);
        const char* v = s ? jash_env_get(s, key) : NULL;
        if (!v) break;
        if (emitted) {
            if (*out_pos + 1 >= cap) goto overflow;
            out[(*out_pos)++] = ' ';
        }
        if (*out_pos + 1 >= cap) goto overflow;
        out[(*out_pos)++] = '"';
        for (const char* p = v; *p; p++) {
            if ((*p == '"' || *p == '\\') && *out_pos + 1 >= cap) goto overflow;
            if (*p == '"' || *p == '\\') out[(*out_pos)++] = '\\';
            if (*out_pos + 1 >= cap) goto overflow;
            out[(*out_pos)++] = *p;
        }
        if (*out_pos + 1 >= cap) goto overflow;
        out[(*out_pos)++] = '"';
        emitted++;
    }
    return 0;
overflow:
    *out_pos = saved;
    positional_refuse_output();
    return -1;
}

/* Parse expansion and apply to variable value */
static void jash_apply_expansion(jash_state_t* s, const char* var_name,
                                  char* result, size_t result_size,
                                  const char* var_value, int var_is_set,
                                  const char* op_str) {
    if (!var_value) var_value = "";
    if (!op_str) op_str = "";

    /* BUG-093: POSIX 2.6.2 expands the word/pattern of ${var#word},
     * ${var%word}, ${var/pat/repl}, ${var:-word}… — jash used the raw text,
     * so the ubiquitous `${path#$prefix/}` was a silent no-op. Substitute
     * $name / ${name} here with a LIGHTWEIGHT scan (re-entering
     * jash_param_expand would clobber its static result buffer). Unset
     * names substitute empty, like bash. */
    char op_expanded[512];
    if (s && strchr(op_str, '$')) {
        size_t oi = 0;
        const char* ip = op_str;
        int changed = 0;
        while (*ip && oi + 1 < sizeof(op_expanded)) {
            if (ip[0] == '\\' && ip[1] == '$') {
                op_expanded[oi++] = '$'; ip += 2; changed = 1; continue;
            }
            if (*ip == '$' &&
                (isalpha((unsigned char)ip[1]) || ip[1] == '_' || ip[1] == '{')) {
                const char* q = ip + 1;
                int braced = (*q == '{');
                if (braced) q++;
                char nm[64]; unsigned ni = 0;
                while ((isalnum((unsigned char)*q) || *q == '_') &&
                       ni + 1 < sizeof(nm)) nm[ni++] = *q++;
                nm[ni] = 0;
                if (braced) {
                    /* Only simple ${name} — a nested operator form stays
                     * literal rather than being half-expanded. */
                    if (*q != '}' || ni == 0) { op_expanded[oi++] = *ip++; continue; }
                    q++;
                }
                if (ni == 0) { op_expanded[oi++] = *ip++; continue; }
                const char* v = jash_env_get(s, nm);
                if (!v) {  /* indexed array: bare name = element 0 */
                    char e0[80];
                    snprintf(e0, sizeof(e0), "%s[0]", nm);
                    v = jash_env_get(s, e0);
                }
                if (v) for (const char* vp = v; *vp && oi + 1 < sizeof(op_expanded); vp++)
                    op_expanded[oi++] = *vp;
                ip = q; changed = 1; continue;
            }
            op_expanded[oi++] = *ip++;
        }
        op_expanded[oi] = 0;
        if (changed) {
            /* A pattern that expands to NOTHING leaves a bare "#"/"%"/"/" —
             * that is strip/replace-with-EMPTY-pattern (a no-op returning the
             * value), NOT the ${#var} length form the dispatch below would
             * see. */
            if ((op_expanded[0] == '#' || op_expanded[0] == '%') &&
                op_expanded[1] == 0) {
                strncpy(result, var_value, result_size - 1);
                result[result_size - 1] = 0;
                return;
            }
            op_str = op_expanded;
        }
    }

    /* BUG-115: the word of ${var#word} / ${var%word} undergoes QUOTE REMOVAL
     * (POSIX) — ${w#"'"} and ${w#\'} matched the raw quoted text and stripped
     * nothing. Unquote the pattern part here; the / branch unquotes its two
     * parts separately AFTER splitting on the delimiter. */
    char op_unq[512];
    if ((op_str[0] == '#' || op_str[0] == '%') && op_str[1]) {
        int dbl = (op_str[1] == op_str[0]);
        const char* pat = op_str + (dbl ? 2 : 1);
        if (*pat) {
            /* BUG-115/134: quote removal on the pattern, keeping escaped or
             * quoted metacharacters literal (prep_pattern), so ${w#"'"},
             * ${w#\'}, and ${w#\*} all match the intended literal. */
            char pu[500];
            prep_pattern(pat, pu, sizeof(pu));
            int n2 = 0;
            op_unq[n2++] = op_str[0];
            if (dbl) op_unq[n2++] = op_str[0];
            for (const char* q = pu; *q && n2 + 1 < (int)sizeof(op_unq); q++)
                op_unq[n2++] = *q;
            op_unq[n2] = 0;
            op_str = op_unq;
        }
    }

    if (*op_str == 0) {
        /* ${var} - just return the variable value */
        strncpy(result, var_value, result_size - 1);
        result[result_size - 1] = 0;
    }
    else if (op_str[0] == '#' && op_str[1] == 0) {
        /* ${#var} - length */
        snprintf(result, result_size, "%zu", strlen(var_value));
    }
    else if (op_str[0] == '/' && op_str[1] != 0) {
        /* Check if it's // (replace all), /# (replace prefix), /% (replace suffix), or single / */
        int replace_all = 0;
        int replace_prefix = 0;
        int replace_suffix = 0;
        const char* pattern_start = op_str + 1;
        
        if (op_str[1] == '/') {
            replace_all = 1;
            pattern_start = op_str + 2;
        } else if (op_str[1] == '#') {
            replace_prefix = 1;
            pattern_start = op_str + 2;
        } else if (op_str[1] == '%') {
            replace_suffix = 1;
            pattern_start = op_str + 2;
        }
        
        /* Find the separator between old and new */
        const char* slash = strchr(pattern_start, '/');
        if (slash) {
            int old_len = (int)(slash - pattern_start);
            char old_str[128];
            /* SECURITY: old_len is bounded only by the surrounding op_str
             * (~256), so `${x/<long>/y}` overflowed old_str[128] with
             * user-controlled bytes (and old_str[old_len]=0 wrote past the
             * end). Clamp to the buffer. */
            if (old_len > (int)sizeof(old_str) - 1)
                old_len = (int)sizeof(old_str) - 1;
            strncpy(old_str, pattern_start, old_len);
            old_str[old_len] = 0;

            const char* new_str = slash + 1;

            /* BUG-115: pattern and replacement undergo quote removal —
             * ${w//\'/_} and ${x//"pat"/y} used the raw escaped text and
             * never matched. jash_unquote also restores shielded quote
             * sentinels, so literal quotes from expanded values stay data. */
            char new_unq[256];
            {
                extern int jash_unquote(const char* in, char* out, size_t out_size);
                extern int jash_has_quotes(const char* str);
                extern void jash_quote_unshield(char* s);
                /* BUG-115/134: the PATTERN keeps escaped/quoted metachars
                 * literal (prep_pattern); the REPLACEMENT is plain text and
                 * gets ordinary quote removal. */
                char oprep[256];
                prep_pattern(old_str, oprep, sizeof(oprep));
                strlcpy(old_str, oprep, sizeof(old_str));
                old_str[sizeof(old_str) - 1] = 0;
                if (jash_has_quotes(new_str) || strchr(new_str, '\\')) {
                    if (jash_unquote(new_str, new_unq, sizeof(new_unq)) >= 0)
                        new_str = new_unq;
                }
                jash_quote_unshield((char*)new_str);
            }
            
            if (replace_prefix) {
                /* ${var/#pat/new} — replace longest prefix matching pat */
                size_t var_len = strlen(var_value);
                int matched_pfx = 0;
                size_t pfx_len = 0;
                /* try longest prefix first (greedy) */
                for (size_t tl = var_len + 1; tl-- > 0; ) {
                    char sub[1024];
                    if (tl >= sizeof sub) continue;
                    memcpy(sub, var_value, tl); sub[tl] = '\0';
                    if (subst_glob_match(old_str, sub)) {
                        matched_pfx = 1; pfx_len = tl; break;
                    }
                }
                if (matched_pfx) {
                    size_t rp = 0;
                    for (const char* q = new_str; *q && rp < result_size - 1; q++) result[rp++] = *q;
                    for (size_t vi = pfx_len; vi < var_len && rp < result_size - 1; vi++) result[rp++] = var_value[vi];
                    result[rp] = 0;
                } else {
                    strncpy(result, var_value, result_size - 1);
                    result[result_size - 1] = 0;
                }
            } else if (replace_suffix) {
                /* ${var/%pat/new} — replace longest suffix matching pat */
                size_t var_len = strlen(var_value);
                int matched_sfx = 0;
                size_t sfx_start = 0;
                /* try earliest start (longest suffix) first */
                for (size_t ts = 0; ts <= var_len; ts++) {
                    char sub[1024];
                    size_t tl = var_len - ts;
                    if (tl >= sizeof sub) continue;
                    memcpy(sub, var_value + ts, tl); sub[tl] = '\0';
                    if (subst_glob_match(old_str, sub)) {
                        matched_sfx = 1; sfx_start = ts; break;
                    }
                }
                if (matched_sfx) {
                    size_t rp = 0;
                    for (size_t vi = 0; vi < sfx_start && rp < result_size - 1; vi++) result[rp++] = var_value[vi];
                    for (const char* q = new_str; *q && rp < result_size - 1; q++) result[rp++] = *q;
                    result[rp] = 0;
                } else {
                    strncpy(result, var_value, result_size - 1);
                    result[result_size - 1] = 0;
                }
            } else {
                jash_substitute_inplace(result, result_size, var_value, old_str, new_str, replace_all);
            }
        } else {
            /* BUG-135: no `/replacement` → the replacement is empty, i.e.
             * DELETE the match. `${s/X}` and `${s//X}` remove it. (The rare
             * `/#`/`/%` delete forms stay a documented partial.) */
            char del_pat[128];
            unsigned dl = 0;
            for (const char* q = pattern_start; *q && dl + 1 < sizeof(del_pat); q++)
                del_pat[dl++] = *q;
            del_pat[dl] = 0;
            {
                /* BUG-134: keep escaped/quoted metachars literal in the
                 * delete pattern too. */
                char dprep[256];
                prep_pattern(del_pat, dprep, sizeof(dprep));
                strlcpy(del_pat, dprep, sizeof(del_pat));
                del_pat[sizeof(del_pat) - 1] = 0;
            }
            if (replace_prefix || replace_suffix) {
                strncpy(result, var_value, result_size - 1);
                result[result_size - 1] = 0;
            } else {
                jash_substitute_inplace(result, result_size, var_value,
                                        del_pat, "", replace_all);
            }
        }
    }
    /* ${var:-default}, ${var:+alt}, ${var:=default}, ${var:?error} - check BEFORE slice */
    else if (op_str[0] == ':' && op_str[1] == '-') {
        /* ${var:-default} - use default if var is empty or unset (default may be empty) */
        if (*var_value) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            copy_expanded_text(s, op_str + 2, result, result_size);
        }
    }
    else if (op_str[0] == ':' && op_str[1] == '+') {
        /* ${var:+alternate} - use alternate if var is set and non-empty (alt may be empty) */
        if (*var_value) {
            copy_expanded_text(s, op_str + 2, result, result_size);
        } else {
            result[0] = 0;
        }
    }
    else if (op_str[0] == '-' && op_str[1] != 0) {
        /* ${var-default} - use default only if var is unset. */
        if (var_is_set) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            copy_expanded_text(s, op_str + 1, result, result_size);
            result[result_size - 1] = 0;
        }
    }
    else if (op_str[0] == '+' && op_str[1] != 0) {
        /* ${var+alternate} - use alternate if var is set. */
        if (var_is_set) {
            strncpy(result, op_str + 1, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            result[0] = 0;
        }
    }
    else if (op_str[0] == '?' && op_str[1] != 0) {
        /* ${var?error} - return value if set, otherwise expand to empty.
         * Full bash prints the diagnostic and fails the command; jash does
         * not yet propagate parameter-expansion errors through eval. */
        if (var_is_set) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            snprintf(g_param_error_msg, sizeof(g_param_error_msg), "%s: %s", var_name ? var_name : "parameter", op_str + 1);
            g_param_error = 1;
            result[0] = 0;
        }
    }
    else if (op_str[0] == ':' && op_str[1] == '=') {
        /* ${var:=default} - assign default if var is empty/unset AND return it (default may be empty) */
        if (*var_value) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            copy_expanded_text(s, op_str + 2, result, result_size);
            if (s && var_name && var_name[0]) jash_env_set(s, var_name, result);
        }
    }
    else if (op_str[0] == '=' && op_str[1] != 0) {
        /* ${var=default} - assign default only if var is unset. */
        if (var_is_set) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            copy_expanded_text(s, op_str + 1, result, result_size);
            if (s && var_name && var_name[0]) jash_env_set(s, var_name, result);
        }
    }
    else if (op_str[0] == ':' && op_str[1] == '?' && op_str[2] != 0) {
        /* ${var:?error} - use error message if var is empty/unset */
        if (*var_value) {
            strncpy(result, var_value, result_size - 1);
            result[result_size - 1] = 0;
        } else {
            snprintf(g_param_error_msg, sizeof(g_param_error_msg), "%s: %s", var_name ? var_name : "parameter", op_str + 2);
            g_param_error = 1;
            result[0] = 0;
        }
    }
    else if (op_str[0] == ':') {
        /* ${var:offset:length} — slice. BUG-131: offset AND length are
         * ARITHMETIC expressions (bash), so `${s:i}`, `${s:i+1}`,
         * `${s:${#s}-2}` all work — the old code only accepted literal
         * digits and, for a bare-name offset, left `result` holding an
         * internal sentinel (`__nameref__s` leaked). Split on the first
         * top-level `:` (a `:` inside parens stays in the offset expr) and
         * arithmetic-evaluate each part. */
        const char* p = op_str + 1;
        char offbuf[160], lenbuf[160];
        unsigned oi = 0; int has_len = 0, depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') { if (depth) depth--; }
            else if (*p == ':' && depth == 0) break;
            if (oi + 1 < sizeof(offbuf)) offbuf[oi++] = *p;
            p++;
        }
        offbuf[oi] = 0;
        if (*p == ':') {
            has_len = 1; p++;
            unsigned li = 0;
            while (*p) { if (li + 1 < sizeof(lenbuf)) lenbuf[li++] = *p; p++; }
            lenbuf[li] = 0;
        }
        /* empty offset (`${s:}`) = 0 */
        int start = 0;
        {
            const char* t = offbuf; while (*t == ' ' || *t == '\t') t++;
            if (*t) start = (int)jash_eval_arithmetic(offbuf, s);
        }
        int length = -1;
        if (has_len) {
            const char* t = lenbuf; while (*t == ' ' || *t == '\t') t++;
            if (*t) length = (int)jash_eval_arithmetic(lenbuf, s);
            /* empty length keeps -1 = rest of string */
        }
        jash_slice_inplace(result, result_size, var_value, start, length);
    }
    else if (op_str[0] == '^' && op_str[1] == '^') {
        /* ${var^^[pat]} — uppercase chars matching pat (all chars if no pat) */
        const char* pat = op_str[2] ? op_str + 2 : NULL;
        size_t op = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
            char ch2[2] = {*cp, 0};
            result[op++] = (!pat || jash_glob_match(pat, ch2))
                           ? (char)toupper((unsigned char)*cp) : *cp;
        }
        result[op] = 0;
    }
    else if (op_str[0] == '^') {
        /* ${var^[pat]} — uppercase first char matching pat (first char if no pat) */
        const char* pat = op_str[1] ? op_str + 1 : NULL;
        size_t op = 0;
        int done = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
            if (!done) {
                char ch2[2] = {*cp, 0};
                if (!pat || jash_glob_match(pat, ch2)) {
                    result[op++] = (char)toupper((unsigned char)*cp);
                    done = 1;
                    continue;
                }
            }
            result[op++] = *cp;
        }
        result[op] = 0;
    }
    else if (op_str[0] == ',' && op_str[1] == ',') {
        /* ${var,,[pat]} — lowercase chars matching pat (all chars if no pat) */
        const char* pat = op_str[2] ? op_str + 2 : NULL;
        size_t op = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
            char ch2[2] = {*cp, 0};
            result[op++] = (!pat || jash_glob_match(pat, ch2))
                           ? (char)tolower((unsigned char)*cp) : *cp;
        }
        result[op] = 0;
    }
    else if (op_str[0] == ',') {
        /* ${var,[pat]} — lowercase first char matching pat (first char if no pat) */
        const char* pat = op_str[1] ? op_str + 1 : NULL;
        size_t op = 0;
        int done = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
            if (!done) {
                char ch2[2] = {*cp, 0};
                if (!pat || jash_glob_match(pat, ch2)) {
                    result[op++] = (char)tolower((unsigned char)*cp);
                    done = 1;
                    continue;
                }
            }
            result[op++] = *cp;
        }
        result[op] = 0;
    }
    else if (op_str[0] == '#' && op_str[1] == '#' && op_str[2] != 0) {
        /* ${var##pattern} - remove longest prefix (must be before # check!) */
        const char* stripped = jash_strip_prefix(var_value, op_str + 2, 1);
        strncpy(result, stripped, result_size - 1);
        result[result_size - 1] = 0;
    }
    else if (op_str[0] == '%' && op_str[1] == '%' && op_str[2] != 0) {
        /* ${var%%pattern} - remove longest suffix (must be before % check!) */
        const char* stripped = jash_strip_suffix(var_value, op_str + 2, 1);
        strncpy(result, stripped, result_size - 1);
        result[result_size - 1] = 0;
    }
    else if (op_str[0] == '#' && op_str[1] != 0) {
        /* ${var#pattern} - remove shortest prefix */
        const char* stripped = jash_strip_prefix(var_value, op_str + 1, 0);
        strncpy(result, stripped, result_size - 1);
        result[result_size - 1] = 0;
    }
    else if (op_str[0] == '%' && op_str[1] != 0) {
        /* ${var%pattern} - remove shortest suffix */
        const char* stripped = jash_strip_suffix(var_value, op_str + 1, 0);
        strncpy(result, stripped, result_size - 1);
        result[result_size - 1] = 0;
    }
    /* ${var@A} — assignment representation: declare -- name=<quoted-value>.
     * Use @Q (single-quote) form for the value to avoid jash stripping
     * embedded " characters from the expansion result. */
    else if (op_str[0] == '@' && op_str[1] == 'A' && op_str[2] == 0) {
        char qval[512];
        jash_apply_expansion(s, var_name, qval, sizeof(qval), var_value, var_is_set, "@Q");
        snprintf(result, result_size, "declare -- %s=%s", var_name, qval);
    }
    /* ${var@Q} — shell-quote: emit value as single-quoted string safe for
     * re-use as shell input.  Empty string → ''. Embedded ' → '\''. */
    else if (op_str[0] == '@' && op_str[1] == 'Q' && op_str[2] == 0) {
        size_t op = 0;
        if (!*var_value) {
            if (op + 2 < result_size - 1) { result[op++] = '\''; result[op++] = '\''; }
        } else {
            if (op + 1 < result_size - 1) result[op++] = '\'';
            for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
                if (*cp == '\'') {
                    /* close quote, escaped tick, open quote */
                    if (op + 4 < result_size - 1) {
                        result[op++] = '\'';
                        result[op++] = '\\';
                        result[op++] = '\'';
                        result[op++] = '\'';
                    }
                } else {
                    result[op++] = *cp;
                }
            }
            if (op + 1 < result_size - 1) result[op++] = '\'';
        }
        result[op] = 0;
    }
    /* ${var@U} — uppercase all chars (alias for ^^) */
    else if (op_str[0] == '@' && op_str[1] == 'U' && op_str[2] == 0) {
        size_t op = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++)
            result[op++] = (char)toupper((unsigned char)*cp);
        result[op] = 0;
    }
    /* ${var@L} — lowercase all chars (alias for ,,) */
    else if (op_str[0] == '@' && op_str[1] == 'L' && op_str[2] == 0) {
        size_t op = 0;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++)
            result[op++] = (char)tolower((unsigned char)*cp);
        result[op] = 0;
    }
    /* ${var@u} — uppercase first char (alias for ^) */
    else if (op_str[0] == '@' && op_str[1] == 'u' && op_str[2] == 0) {
        size_t op = 0;
        int first = 1;
        for (const char* cp = var_value; *cp && op < result_size - 1; cp++) {
            result[op++] = first ? (char)toupper((unsigned char)*cp) : *cp;
            first = 0;
        }
        result[op] = 0;
    }
    else {
        /* Unknown operation, return the original value */
        strncpy(result, var_value, result_size - 1);
        result[result_size - 1] = 0;
    }
}

/* BUG-010: IFS word-splitting of UNQUOTED expansion results. When a command
 * argument expands `$X` (X="a:b:c") with IFS=":", the colons become word
 * boundaries — `set -- $X` yields 3 positional params, not one. This applies
 * ONLY to the substituted value of an unquoted expansion, NEVER to literal
 * text (so `set -- a:b:c` stays one word) and NEVER inside quotes or to an
 * assignment RHS (`Y=$X` keeps the colons). We mark split points by emitting a
 * space in place of each non-whitespace IFS char; the downstream tokenizer then
 * splits there. Activated only via jash_param_expand_split() (command-arg
 * context); plain jash_param_expand() leaves g_pe_split=0, so assignments and
 * other contexts are unaffected. Default IFS (unset / whitespace-only) leaves
 * the map empty → zero behaviour change. F E R M I ∞ H A R T */
/* JINV-087: g_pe_split virou TRI-ESTADO. 0 = desligado; 1 = argv (BUG-010,
 * marca com espaço — inalterado); 2 = CAMPOS (marca com JASH_FIELD_SEP e
 * protege o branco que não está no IFS). O modo 2 é o do `for`: ali o
 * tokenizador entende delimitador exato, então campo vazio (`a::b`) e espaço
 * que é dado (`IFS=:` com valor `a b:c`) sobrevivem — coisas que a marca por
 * espaço do modo 1 não consegue exprimir. */
static int g_pe_split = 0;
static int g_pe_ifs_map[256];

/* ═══ JINV-087 — a porta única de separação de campos ═════════════════════
 *
 * O DEFEITO que isto mata: a lista de palavras do `for` aplicava a divisão por
 * IFS à cadeia INTEIRA, depois de expandir — texto literal e resultado de
 * expansão CITADA incluídos. `IFS=:; for d in /bin:/usr/bin` dava dois itens;
 * o bash dá um. É o idioma `as_dir` de varredura de PATH do autoconf, presente
 * nos 19 `configure`.
 *
 * A informação que faltava não estava perdida: ela existe AQUI, no ponto da
 * expansão, onde `dq` diz se o `$VAR` estava entre aspas. O conserto é emitir
 * a marca aqui, e não adivinhar depois — a mesma lição do JINV-016 (argv[0]
 * injetado), JINV-020 (separador em aspas aninhadas), JINV-028 (`>` perdido),
 * JINV-032 (`;;` em aspas) e JINV-083 (palavra-chave em aspas): a citação foi
 * descartada antes do ponto que precisava dela.
 *
 * Estado de corrida (`g_ifs_pend`) implementa a absorção do POSIX: branco de
 * IFS colado num delimitador não-branco é ENGOLIDO por ele (`a :: b` com
 * IFS=": " são três campos, não quatro). */
static int g_ifs_nonws[256];   /* IFS não-branco → delimitador exato          */
static int g_ifs_ws[256];      /* branco que ESTÁ no IFS → separador colapsável */
static int g_ifs_protect_ws;   /* 1 = branco fora do IFS vira sentinela (dado)  */
static int g_ifs_pend;         /* 0 nada · 1 branco de IFS pendente · 2 saiu delim */
static int g_ifs_has_nonws;    /* $IFS tem algum não-branco? senão, nada a marcar */
static int g_ifs_word_mark_on; /* o `for` pediu a marca de PALAVRA (JASH_EXP_MARK) */

static int pe_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n'; }

void jash_ifs_field_begin(void* state, int protect_ws) {
    jash_state_t* s = (jash_state_t*)state;
    for (int i = 0; i < 256; i++) { g_ifs_nonws[i] = 0; g_ifs_ws[i] = 0; }
    g_ifs_has_nonws = 0;
    const char* ifs = s ? jash_env_get(s, "IFS") : NULL;
    if (!ifs) {
        /* IFS AUSENTE = padrão " \t\n". IFS PRESENTE E VAZIO = nenhuma divisão
         * (POSIX). A diferença entre "não existe" e "existe vazio" é o corpo
         * inteiro do caso `IFS=; for i in $v` — sem ela, o vazio herdaria o
         * padrão e dividiria assim mesmo. */
        g_ifs_ws[(unsigned char)' '] = 1;
        g_ifs_ws[(unsigned char)'\t'] = 1;
        g_ifs_ws[(unsigned char)'\n'] = 1;
    } else {
        for (const char* ip = ifs; *ip; ip++) {
            unsigned char ic = (unsigned char)*ip;
            if (pe_is_ws((char)ic)) g_ifs_ws[ic] = 1;
            else                  { g_ifs_nonws[ic] = 1; g_ifs_has_nonws = 1; }
        }
    }
    g_ifs_protect_ws = protect_ws;
    g_ifs_pend = 0;
}

int jash_ifs_field_active(void) { return g_pe_split == 2; }

/* ═══ JINV-087 — a marca de PALAVRA ═══════════════════════════════════════
 *
 * O oráculo (bash) divide por palavra, não por byte: se a PALAVRA passou por
 * uma expansão não citada, os caracteres de IFS LITERAIS dela também cortam.
 *
 *   IFS=:; for d in a:b        → [a:b]   (nenhuma expansão: nada corta)
 *   IFS=:; for d in $P:x       → [0] [x] (o `:` literal corta — houve expansão)
 *   IFS=:; for d in "$P":x     → [0:x]   (a expansão era citada: nada corta)
 *   IFS=:; for d in $P'a:b'    → [0a:b]  (o `:` está em aspas: não corta)
 *
 * Marcar por byte só resolveria o primeiro e o terceiro; marcar a palavra
 * inteira sem olhar aspas quebraria o quarto. Por isso a marca diz apenas
 * "houve expansão AQUI" e quem varre a palavra (for_loop) decide olhando as
 * aspas — as duas informações, cada uma no lugar onde existe.
 *
 * `wanted` é 0 quando `$IFS` só tem branco: nesse caso a marca não mudaria
 * nada e a saída de toda expansão continua byte a byte igual à de antes. */
void jash_ifs_word_mark_enable(int on) { g_ifs_word_mark_on = on; }
/* ═══ The word mark is OFF, and that is the fix, not a regression ══════════
 *
 * JASH_EXP_MARK encoded the rule "a word that underwent unquoted expansion
 * also splits on the IFS separators written LITERALLY in that word". Measured
 * against bash 5.2, that rule is wrong in every shape:
 *
 *   P=0    IFS=:  for d in $P:x      bash → [0:x]     one field
 *   P=a:b  IFS=:  for d in $P:x      bash → [a] [b:x] only the ':' from the VALUE cut
 *   E=     IFS=:  for d in $E:x      bash → [:x]      empty value still does not cut
 *          IFS=:  for d in $((1+1)):x  bash → [2:x]
 *          IFS=:  for d in $(echo 0):x bash → [0:x]
 *
 * POSIX field splitting applies to the RESULT of an expansion, never to the
 * literal text around it. jash already implements that correctly, byte by
 * byte, in PE_EMIT_VAL → jash_ifs_field_put. The word mark was a second,
 * contradictory rule layered on top, and it is the one that produced the wrong
 * answer. Returning 0 here is the single gate all three emitters (this file,
 * cmdsub.c ×2, arithmetic.c) already consult, so the mark is simply never
 * written and jash_ifs_word_split short-circuits on its absence.
 *
 * The machinery is left in place rather than ripped out: it is the record of
 * what was tried, and the comment blocks at each emitter still document the
 * quoting/backslash reasoning, which stays correct. F E R M I ∞ H A R T */
int  jash_ifs_word_mark_wanted(void)   { return 0; }
int  jash_ifs_word_mark_state(void)    { return g_ifs_word_mark_on; }
int  jash_ifs_is_hard_sep(char c)      { return g_ifs_nonws[(unsigned char)c]; }

/* JINV-093 — `c` é um metacaractere de OPERADOR do shell?
 *
 * Estes são os bytes que, NUS, decidem a ESTRUTURA do enunciado (pipeline,
 * lista, redirecionamento, subshell). Quando vêm precedidos de barra
 * invertida eles são DADO, e a barra tem que sobreviver a esta passada — a
 * decisão estrutural (`jash_line_has_pipe`, c/parser/pipeline.c) só acontece
 * DEPOIS, sobre o texto já expandido, e sem a barra não há como distinguir.
 * Contrato completo no bloco de uso, dentro de jash_param_expand_impl.
 * Gate: tests/test_jinv093_escaped_operator.sh */
static int jash_pe_is_operator_meta(char c) {
    return c == '|' || c == '&' || c == ';' ||
           c == '<' || c == '>' || c == '(' || c == ')';
}

void jash_ifs_field_flush(char* out, size_t cap, size_t* o) {
    if (g_ifs_pend == 1 && *o < cap - 1) out[(*o)++] = ' ';
    g_ifs_pend = 0;
}

void jash_ifs_field_put(char orig, char shielded, char* out, size_t cap, size_t* o) {
    unsigned char uc = (unsigned char)orig;
    if (*o >= cap - 1) return;
    if (g_ifs_nonws[uc]) {                    /* delimitador EXATO */
        out[(*o)++] = JASH_FIELD_SEP;
        g_ifs_pend = 2;                       /* branco à direita é absorvido */
        return;
    }
    if (pe_is_ws(orig)) {
        if (g_ifs_ws[uc]) {                   /* branco separador */
            if (g_ifs_pend != 2) g_ifs_pend = 1;   /* corrida colapsa num só */
            return;
        }
        /* branco que NÃO está no IFS é DADO — o tokenizador a jusante corta em
         * isspace(), então ele precisa ir blindado. */
        if (g_ifs_pend == 1 && *o < cap - 1) out[(*o)++] = ' ';
        g_ifs_pend = 0;
        if (*o >= cap - 1) return;
        if (!g_ifs_protect_ws)      out[(*o)++] = shielded;
        else if (orig == ' ')       out[(*o)++] = JASH_PROT_SPACE;  /* JINV-009 */
        else if (orig == '\t')      out[(*o)++] = JASH_PROT_TAB;
        else                        out[(*o)++] = JASH_PROT_NL;
        return;
    }
    if (g_ifs_pend == 1 && *o < cap - 1) out[(*o)++] = ' ';
    g_ifs_pend = 0;
    if (*o < cap - 1) out[(*o)++] = shielded;
}

void jash_ifs_field_end(void) { g_ifs_pend = 0; }

/* Emit a substituted value, converting non-ws IFS chars to spaces when splitting
 * is active and we are not inside double quotes.
 * JINV-087: no modo 2 o byte sai pela porta de campos acima — ela é a que sabe
 * a diferença entre separador e dado. */
#define PE_EMIT_VAL(dst, op, src, cap, dq_flag)                                \
    do {                                                                       \
        int _fld = (g_pe_split == 2) && !(dq_flag);                            \
        /* JINV-087: a marca de palavra NÃO sai aqui — sai no `$`, uma vez só  \
         * (veja o laço principal). Inclusive para valor VAZIO, que é o caso   \
         * de `E=; IFS=:; for d in $E:x`, que corta no `:` literal.  */        \
        for (const char* _v = (src); *_v && (op) < (cap) - 1; _v++) {          \
            unsigned char _c = (unsigned char)*_v;                             \
            char _e = *_v;                                                     \
            /* BUG-096: literal quotes in VALUES are data, not syntax —       \
             * shield them so downstream tokenizers don't re-parse them.      \
             * Unshielded by jash_quote_unshield in every token consumer. */  \
            if (_e == '"')       _e = (char)0x03;                             \
            else if (_e == '\'') _e = (char)0x04;                             \
            else if (_e == '\\') _e = (char)0x05;  /* BUG-097 */              \
            /* JINV-096: e o mesmo, byte a byte, para quem decide ESTRUTURA.  \
             * Um `|` dentro de um VALOR nunca foi digitado por ninguem; a    \
             * pergunta "esta linha tem cano?" so e feita DEPOIS desta        \
             * reescrita, quando as duas origens ja sao o mesmo byte.         \
             * Vem DEPOIS da divisao por IFS de propOsito: o corte olha o     \
             * byte ORIGINAL (`*_v`), nunca o blindado — se `$IFS` contiver   \
             * um metacaractere, quem manda e o IFS. */                       \
            if (_fld) { jash_ifs_field_put(*_v, jash_meta_shield(_e),         \
                                           (dst), (cap), &(op)); continue; }  \
            if (g_pe_split == 1 && !(dq_flag) && g_pe_ifs_map[_c] &&          \
                _e == *_v) _e = ' ';                                          \
            else _e = jash_meta_shield(_e);   /* JINV-096 */                  \
            (dst)[(op)++] = _e;                                                \
        }                                                                      \
        if (_fld) jash_ifs_field_flush((dst), (cap), &(op));                   \
    } while (0)

static const char* param_expand_impl(const char* str, void* state, int known_present) {
    if (g_stack_top == 0) {
        jash_param_expand_clear_error();
    }
    if (!str || (!known_present && !jash_has_param_expansion(str))) return str;

    jash_state_t* s = (jash_state_t*)state;

    /* ADR-0036 W5: cache lookup, gated on the generation counter (so a stale
     * entry from before the last env mutation never matches), the
     * cacheability scan above (so $?/$$/etc. never get served stale), and a
     * length guard (var_cache's key[] is a fixed 64 bytes — a longer string
     * would silently truncate and risk two different inputs colliding on
     * the same truncated key). */
    /* JINV-087: o cache é chaveado SÓ pelo texto de entrada — não pelo modo.
     * Com a divisão de campos ativa, `$X` produz uma cadeia DIFERENTE (com
     * JASH_FIELD_SEP e branco blindado) da que o modo normal produz para a
     * mesma chave. Servir uma pela outra vazaria sentinela para dentro de um
     * valor, ou devolveria um valor sem cortar. Enquanto há modo de divisão
     * ligado, o cache fica fora do caminho — nos dois sentidos. */
    int cacheable = s && !g_pe_split && strlen(str) < 63 && pe_is_cacheable(str);
    if (cacheable) {
        const char* cached = jash_var_cache_lookup(s, str);
        if (cached) return cached;
    }

    /* ADR-0016 §5: lazily-allocated result stack (was 1 MB of .bss).
     * Same pattern as cmdsub out_ring. Slots allocated on first nested use. */
    if (g_stack_top >= JASH_PARAM_EXPAND_DEPTH) {
        g_param_error = 1;
        snprintf(g_param_error_msg, sizeof g_param_error_msg,
                 "jash: parameter expansion nesting limit exceeded — refusing");
        return str;
    }
    if (!g_result_stack[g_stack_top]) {
        g_result_stack[g_stack_top] = malloc(JASH_PARAM_EXPAND_CAP);
        if (!g_result_stack[g_stack_top]) {
            positional_refuse_memory();
            return str;
        }
    }
    char* result = g_result_stack[g_stack_top++];
    size_t out_pos = 0;
    size_t in_pos = 0;
    size_t str_len = strlen(str);
    int sq = 0, dq = 0, skip_closing_dq = 0;

    while (in_pos < str_len && out_pos < JASH_PARAM_EXPAND_CAP - 1) {
        if (str[in_pos] == '\'' && !dq) {
            sq = !sq;
            result[out_pos++] = str[in_pos++];
            continue;
        }
        if (str[in_pos] == '"' && !sq) {
            if (skip_closing_dq) {
                skip_closing_dq = 0;
                dq = 0;
                in_pos++;
                continue;
            }
            dq = !dq;
            result[out_pos++] = str[in_pos++];
            continue;
        }

        /* BUGFIX (edge-case audit): Inside single quotes, ALL characters are
         * literal — no $ expansion, no backtick, nothing. Copy verbatim. */
        if (sq) {
            result[out_pos++] = str[in_pos++];
            continue;
        }

        /* Backslash escapes that SUPPRESS variable expansion.
         *   - Inside double quotes: only \$ \` \" \\ \<newline> are special.
         *     We must convert "\$" to a literal "$" so it never reaches the
         *     $VAR / ${VAR} branches below (which would expand it).
         *   - Outside any quotes: \$ also suppresses expansion (bash POSIX).
         * The backslash is consumed; the escaped char is emitted as-is.
         * Other backslash sequences inside DQ are kept verbatim so
         * downstream stages (jash_unquote) see them unchanged. */
        if (str[in_pos] == '\\' && in_pos + 1 < str_len) {
            char next = str[in_pos + 1];
            if (dq) {
                if (next == '$' || next == '`' || next == '\\') {
                    /* Expansion/escape-suppressing: drop backslash, emit char. */
                    result[out_pos++] = next;
                    in_pos += 2;
                    continue;
                }
                if (next == '"') {
                    /* PRESERVE \" — it is a QUOTING concern, not an expansion one.
                     * De-escaping it here turns an escaped quote into a real
                     * delimiter, so the downstream word-splitter mis-splits at
                     * the next space (e.g. `python3 -c "print(\"a b\")"` became
                     * two args). Keep \" intact; the splitter/unquote stage
                     * removes it with the right quote context. (agent bug) */
                    result[out_pos++] = '\\';
                    result[out_pos++] = '"';
                    in_pos += 2;
                    continue;
                }
                /* Other escapes: keep backslash literal inside DQ (bash). */
            } else {
                /* BUG-059: PRESERVE \' and \" outside quotes — same rationale
                 * as the \" case above. These are QUOTING concerns, not
                 * expansion ones: de-escaping \' here turned it into a REAL
                 * quote delimiter for every downstream quote-aware stage, so
                 * the two-pass idiom `eval 'cmd '\''arg'\''` (what every agent
                 * harness emits) arrived at eval with its inner quote layer
                 * destroyed. jash_unquote / core/argv.c remove the backslash
                 * later, WITH the right quote context. */
                if (next == '\'' || next == '"') {
                    result[out_pos++] = '\\';
                    result[out_pos++] = next;
                    in_pos += 2;
                    continue;
                }
                /* JINV-087: mesmíssima razão, para o separador de campo. Com a
                 * divisão ligada, `IFS=:; for d in ${P}a\:b` é UM campo no
                 * bash — a barra invertida é citação, e citação PROÍBE o
                 * corte. Se ela morresse aqui, a varredura de palavras veria
                 * um `:` nu numa palavra que teve expansão e cortaria. A barra
                 * é removida depois, pelo tokenizador, que tem o contexto. */
                if (g_pe_split == 2 && jash_ifs_is_hard_sep(next)) {
                    result[out_pos++] = '\\';
                    result[out_pos++] = next;
                    in_pos += 2;
                    continue;
                }
                /* ═══ JINV-093 — a barra que segura um OPERADOR ══════════════
                 *
                 * Exatamente a razão do `\'` e do `\"` logo acima, para os
                 * metacaracteres de OPERADOR. `\|` não é um cano: é o byte
                 * `|` como DADO. Mas a decisão estrutural "esta linha é um
                 * pipeline?" (`jash_line_has_pipe`, parser/pipeline.c) roda
                 * DEPOIS desta passada, sobre o texto JÁ EXPANDIDO. Se a
                 * barra morresse aqui, o que sobrava era um `|` nu, e a
                 * varredura — que respeita `\\` corretamente — não tinha mais
                 * como saber que ele fora citado.
                 *
                 * O sintoma que pagou por isto: o idioma `X$0` do autoconf,
                 *
                 *     $as_expr X"$0" : 'X\(.*\)' \| \
                 *              X"$0" : 'X\(.\)'
                 *
                 * virava DOIS comandos e o shell tentava executar
                 * `X./configure` — 10 das 38 invocações de `configure` do
                 * corpus morriam assim. Repare que só quebrava QUANDO havia
                 * expansão na linha: sem `$`, esta passada nem roda, o `\|`
                 * chega inteiro ao tokenizador e tudo funciona. Dado virando
                 * ESTRUTURA por texto reescrito — a família do JINV-016/020/
                 * 032/083/087.
                 *
                 * A barra é removida DEPOIS, pelo tokenizador (jash_unquote /
                 * core/argv.c), que tem o contexto — é o mesmo contrato do
                 * `\'`, do `\"` e do separador de campo do JINV-087. */
                if (jash_pe_is_operator_meta(next)) {
                    result[out_pos++] = '\\';
                    result[out_pos++] = next;
                    in_pos += 2;
                    continue;
                }
                /* Outside quotes: backslash escapes anything else. */
                result[out_pos++] = next;
                in_pos += 2;
                continue;
            }
        }

        /* ═══ JINV-087 — a marca de PALAVRA, emitida UMA vez ═══════════════
         *
         * No `$` que abre uma expansão NÃO CITADA, antes de qualquer ramo:
         * assim `$VAR`, `${…}`, `$1`, `$@`, `$*`, `$?`, `$$`, `$!`, `$-` e
         * `$_` contam a MESMA história, e nenhum deles pode ser esquecido
         * quando um ramo novo nascer. (Marcar dentro de cada ramo era o
         * começo da nona cópia da mesma decisão.) */
        if (g_pe_split == 2 && !sq && !dq && str[in_pos] == '$' &&
            in_pos + 1 < str_len && jash_ifs_word_mark_wanted() &&
            out_pos < JASH_PARAM_EXPAND_CAP - 1) {
            char nx = str[in_pos + 1];
            if (isalnum((unsigned char)nx) || nx == '_' || nx == '{' ||
                nx == '(' || nx == '?' || nx == '$' || nx == '#' ||
                nx == '!' || nx == '-' || nx == '*' || nx == '@')
                result[out_pos++] = JASH_EXP_MARK;
        }

        /* ${VAR...} — braced form with optional operators */
        if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '{') {
            in_pos += 2;  /* Skip ${ */

            /* Extract variable name */
            char var_name[64];
            size_t var_pos = 0;
            
            while (in_pos < str_len && var_pos < sizeof(var_name) - 1 && 
                   (isalnum((unsigned char)str[in_pos]) || str[in_pos] == '_')) {
                var_name[var_pos++] = str[in_pos++];
            }
            if (in_pos < str_len && str[in_pos] == '[' && var_pos < sizeof(var_name) - 1) {
                var_name[var_pos++] = str[in_pos++];
                while (in_pos < str_len && var_pos < sizeof(var_name) - 1) {
                    char c = str[in_pos++];
                    var_name[var_pos++] = c;
                    if (c == ']') break;
                }
            }
            var_name[var_pos] = 0;

            /* Extract operation/parameters until } */
            char op_str[256];
            size_t op_pos = 0;
            int depth = 1;
            
            while (in_pos < str_len && depth > 0 && op_pos < sizeof(op_str) - 1) {
                if (str[in_pos] == '{') {
                    depth++;
                } else if (str[in_pos] == '}') {
                    depth--;
                    if (depth == 0) {
                        in_pos++;
                        break;
                    }
                }
                op_str[op_pos++] = str[in_pos++];
            }
            op_str[op_pos] = 0;

            /* Array/count/indirect forms (${arr[@]}, ${#arr[@]}, ${!arr[@]},
             * ${#scalar}, ${!name}) need to see the whole inner body. */
            {
                char full[320];
                size_t fp = 0;
                for (size_t i = 0; var_name[i] && fp + 1 < sizeof full; i++) full[fp++] = var_name[i];
                for (size_t i = 0; op_str[i] && fp + 1 < sizeof full; i++) full[fp++] = op_str[i];
                full[fp] = 0;
                /* ═══ o quarto gemeo do buffer mudo de 512 ════════════════
                 *
                 * JINV-080 (corpo do `for`), JINV-082 (`while`/`case`) e
                 * JINV-086 (corpo de funcao) foram o MESMO defeito: um buffer
                 * fixo que truncava em silencio e seguia rodando. `${a[@]}`
                 * era o quarto, e ninguem tinha medido.
                 *
                 * Medido em 2026-08-05: um array de 60 nomes (1260 bytes)
                 * expandia para EXATAMENTE 512, e
                 *   for x in "${A[@]}"; do n=$((n+1)); done
                 * contava 25 de 60. Sem erro, sem aviso, rc 0. Um laco que
                 * processa 40% da lista e reporta sucesso e pior que um que
                 * falha: o script parece ter funcionado. Foi assim que a
                 * triagem dos 53 gates orfaos deste dia mediu 26.
                 *
                 * Cresce por tentativa em vez de exigir uma API de medida
                 * nova: jash_expand_array_form escreve com `cap` e trunca,
                 * entao encher cap-1 e o sinal. Dobra ate caber ou ate o teto
                 * de linha do projeto, e falta de memoria e recusa ALTA —
                 * nunca uma expansao pela metade. F E R M I ∞ H A R T */
                char arr_stack[512];
                char*  arr_out = arr_stack;
                char*  arr_heap = NULL;
                size_t arr_cap = sizeof arr_stack;
                /* Positional lists can legitimately contain 384 values and
                 * already have a measured project ceiling. Give those forms
                 * the final capacity immediately; the generic array forms
                 * retain the grow-by-demand path below. */
                if (full[0] == '@' || full[0] == '*') {
                    arr_heap = (char*)malloc(JASH_LINE_CAP);
                    if (!arr_heap) {
                        positional_refuse_memory();
                        g_stack_top--;
                        return NULL;
                    }
                    arr_out = arr_heap;
                    arr_cap = JASH_LINE_CAP;
                }
                int    arr_ok  = jash_expand_array_form(s, full, arr_out, arr_cap, dq);
                /* >= cap-2, nao == cap-1: os lacos de copia da funcao param
                 * em `op + 1 < cap`, entao a saida cheia mede cap-2 e um
                 * criterio de igualdade exata nunca dispara. Errei isso na
                 * primeira tentativa e o buffer seguiu capado em 512 com o
                 * codigo de crescimento no lugar — um off-by-one que se
                 * disfarca de "a correcao nao funcionou". */
                while (arr_ok && strlen(arr_out) >= arr_cap - 2 &&
                       arr_cap < (size_t)JASH_LINE_CAP) {
                    size_t bigger = arr_cap * 4;
                    if (bigger > (size_t)JASH_LINE_CAP) bigger = (size_t)JASH_LINE_CAP;
                    char* grown = (char*)realloc(arr_heap, bigger);
                    if (!grown) {
                        free(arr_heap);
                        jash_writef(jash_stderr_writer, 0,
                            "jash: ${...[@]}: sem memoria para %zu bytes — "
                            "expansao recusada, nao truncada\n", bigger);
                        g_param_error = 1;
                        snprintf(g_param_error_msg, sizeof g_param_error_msg,
                                 "jash: array expansion out of memory — refusing");
                        g_stack_top--;
                        return NULL;
                    }
                    arr_heap = grown; arr_out = arr_heap; arr_cap = bigger;
                    arr_ok = jash_expand_array_form(s, full, arr_out, arr_cap, dq);
                }
                if (arr_ok) {
                    /* BUG-077: a standalone "${a[@]}" over an EMPTY array
                     * expands to ZERO words (like "$@") — drop the wrapping
                     * quotes we already emitted so no empty word survives
                     * (`a=(); set -- "${a[@]}"` → $# == 0, not 1). */
                    if (arr_out[0] == '\0' && dq && jash_str_ends(full, "[@]") &&
                        out_pos > 0 && result[out_pos - 1] == '"') {
                        out_pos--;             /* remove the opening "     */
                        skip_closing_dq = 1;   /* and skip the closing "   */
                        free(arr_heap);
                        continue;
                    }
                    /* JINV-087: `${a[@]}` SEM aspas também é expansão não
                     * citada — `IFS=:; a=(x:y z); for i in ${a[@]}` são três
                     * campos. Com aspas, nenhum: `"${a[@]}"` preserva o `:`
                     * dentro do elemento (era o que a divisão cega quebrava).
                     * protect_ws=0 aqui de propósito: o espaço que separa
                     * elementos de array é JUNÇÃO estrutural, não dado —
                     * protegê-lo colaria os elementos num campo só. */
                    int _afld = jash_ifs_field_active() && !dq;
                    size_t arr_len = strlen(arr_out);
                    size_t arr_need = arr_len;
                    if (_afld && jash_ifs_word_mark_wanted())
                        for (size_t ai = 0; ai < arr_len; ai++)
                            if (arr_out[ai] == ' ') arr_need++;
                    if (arr_need >= JASH_PARAM_EXPAND_CAP - out_pos) {
                        positional_refuse_output();
                        free(arr_heap);
                        continue;
                    }
                    if (_afld) jash_ifs_field_begin(s, 0);
                    size_t arr_i = 0;
                    for (; arr_out[arr_i] && out_pos < JASH_PARAM_EXPAND_CAP - 1; arr_i++) {
                        char _ae = arr_out[arr_i];
                        /* BUG-096: shield literal quotes in element values */
                        if (_ae == '"')       _ae = (char)0x03;
                        else if (_ae == '\'') _ae = (char)0x04;
                        else if (_ae == '\\') _ae = (char)0x05;  /* BUG-097 */
                        _ae = jash_meta_shield(_ae);   /* JINV-096 */
                        if (_afld) {
                            jash_ifs_field_put(arr_out[arr_i], _ae, result,
                                               JASH_PARAM_EXPAND_CAP, &out_pos);
                            /* JINV-087: `${a[@]}` sem aspas produz VÁRIAS
                             * palavras (a junção é um espaço). A marca vale
                             * para todas — `IFS=:; a=(1 2); for d in
                             * ${a[@]}:x` corta o `:x` colado na ÚLTIMA. Sem
                             * remarcar, só a primeira palavra ficaria marcada
                             * e o `:` literal da última sobreviveria. */
                            if (arr_out[arr_i] == ' ' && jash_ifs_word_mark_wanted() &&
                                out_pos < JASH_PARAM_EXPAND_CAP - 1)
                                result[out_pos++] = JASH_EXP_MARK;
                            continue;
                        }
                        result[out_pos++] = _ae;
                    }
                    if (arr_out[arr_i]) positional_refuse_output();
                    if (_afld) {
                        jash_ifs_field_flush(result, JASH_PARAM_EXPAND_CAP, &out_pos);
                        jash_ifs_field_begin(s, 1);   /* volta ao modo de valor */
                    }
                    free(arr_heap);
                    continue;
                }
                free(arr_heap);   /* nao era forma de array: cai adiante */
            }

            /* Get variable value from env (FIX #2) */
            char special[128];
            const char* var_value = jash_special_var(s, var_name, special, sizeof special);
            int var_is_set = var_value ? 1 : 0;
            if (!var_value && s) {
                /* Nameref: follow declare -n indirection if present */
                char nrkey[128];
                snprintf(nrkey, sizeof(nrkey), "__nameref__%s", var_name);
                const char* nrtarget = jash_env_get(s, nrkey);
                if (nrtarget && *nrtarget) {
                    var_value = jash_env_get(s, nrtarget);
                    var_is_set = var_value ? 1 : 0;
                }
            }
            if (!var_value) {
                var_value = s ? jash_env_get(s, var_name) : NULL;
                var_is_set = var_value ? 1 : 0;
            }
            if (!var_value && s) {
                /* BUG-077: a bare reference to an indexed array ($a or ${a})
                 * yields element 0 in bash. Fall back to name[0] when the
                 * plain scalar name is unset. */
                int ident = var_name[0] != '\0';
                for (const char* q = var_name; *q; q++)
                    if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||
                          (*q>='0'&&*q<='9')||*q=='_')) { ident = 0; break; }
                if (ident) {
                    char e0[160];
                    snprintf(e0, sizeof(e0), "%s[0]", var_name);
                    const char* v0 = jash_env_get(s, e0);
                    if (v0) { var_value = v0; var_is_set = 1; }
                }
            }
            if (!var_value) {
                /* set -u (nounset): plain ${var} with no operator triggers an
                 * error when the variable is unset.  Operators that supply a
                 * default (:-  -  :=  =) are handled by jash_apply_expansion
                 * and are therefore exempt from nounset — op_str[0] is set for
                 * those.  :? and ? have their own error path so also exempt. */
                if (!op_str[0] && s && s->shell_opts.nounset) {
                    snprintf(g_param_error_msg, sizeof(g_param_error_msg),
                             "jash: %s: unbound variable", var_name);
                    g_param_error = 1;
                }
                var_value = "";
            }

            /* Apply the expansion operation.
             * Fast path for plain ${var} (no operator): copy directly from
             * var_value into result[], bypassing the intermediate buffer and
             * removing the old 512-char cap that truncated long strings.
             * Slow path (operators present): use an 8 KB scratch buffer —
             * matching JASH_ENV_VAL_LEN so operator results are never clipped
             * shorter than what the env layer can store. */
            if (!op_str[0]) {
                PE_EMIT_VAL(result, out_pos, var_value, JASH_PARAM_EXPAND_CAP, dq);
            } else {
                char expanded[8192];
                jash_apply_expansion(s, var_name, expanded, sizeof(expanded),
                                     var_value, var_is_set, op_str);
                PE_EMIT_VAL(result, out_pos, expanded, JASH_PARAM_EXPAND_CAP, dq);
            }
        }
        /* $1..$9 — positional parameters (single digit). */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len &&
                 str[in_pos + 1] >= '0' && str[in_pos + 1] <= '9') {
            char key[2] = { str[in_pos + 1], 0 };
            in_pos += 2;
            const char* pv = s ? jash_env_get(s, key) : NULL;
            if (!pv) pv = "";
            PE_EMIT_VAL(result, out_pos, pv, JASH_PARAM_EXPAND_CAP, dq);
        }
        /* $VAR — bare form (no braces, no operators) */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len &&
                 (isalpha((unsigned char)str[in_pos + 1]) || str[in_pos + 1] == '_')) {
            in_pos++;  /* skip $ */
            char var_name[64];
            size_t var_pos = 0;
            while (in_pos < str_len && var_pos < sizeof(var_name) - 1 &&
                   (isalnum((unsigned char)str[in_pos]) || str[in_pos] == '_')) {
                var_name[var_pos++] = str[in_pos++];
            }
            var_name[var_pos] = 0;
            char special[128];
            const char* var_value = jash_special_var(s, var_name, special, sizeof special);
            if (!var_value && s) {
                /* Nameref: follow declare -n indirection if present */
                char nrkey[128];
                snprintf(nrkey, sizeof(nrkey), "__nameref__%s", var_name);
                const char* nrtarget = jash_env_get(s, nrkey);
                if (nrtarget && *nrtarget) var_value = jash_env_get(s, nrtarget);
            }
            if (!var_value) var_value = s ? jash_env_get(s, var_name) : NULL;
            if (!var_value && s) {
                /* BUG-077: bare $a on an indexed array is element 0 (var_name
                 * is already a clean identifier here). */
                char e0[80];
                snprintf(e0, sizeof(e0), "%s[0]", var_name);
                var_value = jash_env_get(s, e0);
            }
            if (!var_value) {
                /* set -u (nounset): bare $VAR for an unset variable is an error. */
                if (s && s->shell_opts.nounset) {
                    snprintf(g_param_error_msg, sizeof(g_param_error_msg),
                             "jash: %s: unbound variable", var_name);
                    g_param_error = 1;
                }
                var_value = "";
            }
            PE_EMIT_VAL(result, out_pos, var_value, JASH_PARAM_EXPAND_CAP, dq);
        }
        /* $#, $*, $@ — positional parameter count / list */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len &&
                 (str[in_pos + 1] == '#' || str[in_pos + 1] == '*' || str[in_pos + 1] == '@')) {
            char which = str[in_pos + 1];
            in_pos += 2;
            char small[32];
            char* sbuf = small;
            char* sbuf_heap = NULL;
            small[0] = 0;
            if (which == '#') {
                int c = jash_positional_join(s, ' ', NULL, 0);
                snprintf(small, sizeof small, "%d", c);
            } else if (which == '@' && dq && out_pos > 0 && result[out_pos - 1] == '"' &&
                       in_pos < str_len && str[in_pos] == '"') {
                /* Standalone "$@" (nothing else inside the quotes): remove
                 * the wrapping quote we already copied, emit one quoted word
                 * per positional, and skip the closing quote. This preserves
                 * argv splitting downstream. BUG-094: with adjacent text
                 * ("$@ tail") this must NOT fire — the \x1E case below
                * handles adjacency-join. */
                out_pos--;
                (void)jash_append_quoted_positionals(s, result, &out_pos,
                                                     JASH_PARAM_EXPAND_CAP);
                skip_closing_dq = 1;
            } else if (which == '@' && dq) {
                /* BUG-094: NON-standalone "pre $@ post" — each positional is
                 * its own word with the adjacent literal glued to the
                 * first/last (POSIX adjacency-join). Emit \x1E boundaries;
                 * the \x1E-aware tokenizers split there while the quote
                 * state persists across elements. (Not positional_join —
                 * that substitutes IFS's first char for the separator.) */
                size_t need = 1;
                int pc = jash_positional_join(s, ' ', NULL, 0);
                for (int pi = 1; pi <= pc; pi++) {
                    char pk[sizeof(int) * CHAR_BIT + 2];
                    snprintf(pk, sizeof pk, "%d", pi);
                    const char* pv = jash_env_get(s, pk);
                    need += strlen(pv ? pv : "") + (pi > 1 ? 1u : 0u);
                }
                sbuf_heap = (char*)malloc(need);
                if (!sbuf_heap) {
                    positional_refuse_memory();
                } else {
                    sbuf = sbuf_heap;
                }
                size_t sp3 = 0;
                for (int pi = 1; pi <= JASH_POS_MAX; pi++) {
                    char pk[8]; snprintf(pk, sizeof(pk), "%d", pi);
                    const char* pv = s ? jash_env_get(s, pk) : NULL;
                    if (!pv) break;
                    if (!sbuf_heap) break;
                    if (pi > 1) sbuf[sp3++] = '\x1E';
                    for (const char* q = pv; *q; q++) {
                        char _pe = *q;
                        /* BUG-096: shield literal quotes in values */
                        if (_pe == '"')       _pe = (char)0x03;
                        else if (_pe == '\'') _pe = (char)0x04;
                        else if (_pe == '\\') _pe = (char)0x05;  /* BUG-097 */
                        _pe = jash_meta_shield(_pe);   /* JINV-096 */
                        sbuf[sp3++] = _pe;
                    }
                }
                sbuf[sp3] = 0;
            } else {
                /* $* and $@ both join by space outside quotes. */
                size_t need = jash_positional_join_required(s, ' ') + 1;
                sbuf_heap = (char*)malloc(need);
                if (!sbuf_heap) {
                    positional_refuse_memory();
                } else {
                    sbuf = sbuf_heap;
                    jash_positional_join(s, ' ', sbuf, need);
                }
            }
            /* JINV-087: `$*` / `$@` SEM aspas são expansão não citada — o
             * texto já unido pelo primeiro caractere do IFS ainda tem que ser
             * dividido nele (`set -- a b; IFS=:; for d in $*` → dois campos).
             * COM aspas, `"$*"` é UMA palavra e o `:` da junção é dado. O ramo
             * `"$@"` acima já emite \x1E por elemento e passa intacto (dq). */
            {
                int _pfld = jash_ifs_field_active() && !dq;
                size_t i = 0;
                for (; sbuf[i] && out_pos < JASH_PARAM_EXPAND_CAP - 1; i++) {
                    /* JINV-096: `$1` / `$@` / `$*` sao a porta mais exposta que
                     * existe — o argumento de linha de comando vem de FORA.
                     * `./script.sh 'a | tee X'` com `echo $1` executava o tee. */
                    char _oe = jash_meta_shield(sbuf[i]);   /* JINV-096 */
                    if (_pfld) {
                        jash_ifs_field_put(sbuf[i], _oe, result,
                                           JASH_PARAM_EXPAND_CAP, &out_pos);
                        continue;
                    }
                    result[out_pos++] = _oe;
                }
                if (sbuf[i]) positional_refuse_output();
                if (_pfld) jash_ifs_field_flush(result, JASH_PARAM_EXPAND_CAP, &out_pos);
            }
            free(sbuf_heap);
        }
        /* $_ — last argument of the previous command (best-effort) */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '_') {
            in_pos += 2;
            const char* v = s ? jash_env_get(s, "_") : NULL;
            if (!v) v = "";
            /* JINV-096: `$_` guarda o ULTIMO ARGUMENTO do comando anterior —
             * dado puro, e um dos poucos que o usuario nunca escreveu. */
            while (*v && out_pos < JASH_PARAM_EXPAND_CAP - 1)
                result[out_pos++] = jash_meta_shield(*v++);   /* JINV-096 */
        }
        /* $! — PID/PGID of last background job */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '!') {
            in_pos += 2;
            const char* v = s ? jash_env_get(s, "!") : NULL;
            if (!v) v = "";
            while (*v && out_pos < JASH_PARAM_EXPAND_CAP - 1)
                result[out_pos++] = jash_meta_shield(*v++);   /* JINV-096 */
        }
        /* $- — current shell option flags */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '-') {
            in_pos += 2;
            char flags[16];
            int fp = 0;
            if (s) {
                if (s->shell_opts.errexit) flags[fp++] = 'e';
                if (s->shell_opts.noglob) flags[fp++] = 'f';   /* BUG-141: set -f */
                if (s->shell_opts.nounset) flags[fp++] = 'u';
                if (s->shell_opts.xtrace) flags[fp++] = 'x';
                if (s->shell_opts.verbose) flags[fp++] = 'v';
                if (s->interactive) flags[fp++] = 'i';
                if (s->job_control_active) flags[fp++] = 'm';
            }
            flags[fp] = 0;
            for (int i = 0; flags[i] && out_pos < JASH_PARAM_EXPAND_CAP - 1; i++) result[out_pos++] = flags[i];
        }
        /* $? — last exit status */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '?') {
            char buf[16];
            int code = s ? s->last_status.code : 0;
            int n = 0;
            /* itoa */
            if (code < 0) { buf[n++] = '-'; code = -code; }
            char tmp[16]; int t = 0;
            if (code == 0) tmp[t++] = '0';
            while (code > 0) { tmp[t++] = '0' + (code % 10); code /= 10; }
            while (t > 0) buf[n++] = tmp[--t];
            buf[n] = 0;
            for (int i = 0; buf[i] && out_pos < JASH_PARAM_EXPAND_CAP - 1; i++) {
                result[out_pos++] = buf[i];
            }
            in_pos += 2;
        }
        /* $$ — current shell PID */
        else if (str[in_pos] == '$' && in_pos + 1 < str_len && str[in_pos + 1] == '$') {
            char buf[32];
            long pid = s ? s->pid : 0;
            int n = 0;
            if (pid < 0) { buf[n++] = '-'; pid = -pid; }
            char tmp[32]; int t = 0;
            if (pid == 0) tmp[t++] = '0';
            while (pid > 0 && t < (int)sizeof(tmp)) { tmp[t++] = '0' + (pid % 10); pid /= 10; }
            while (t > 0 && n < (int)sizeof(buf) - 1) buf[n++] = tmp[--t];
            buf[n] = 0;
            for (int i = 0; buf[i] && out_pos < JASH_PARAM_EXPAND_CAP - 1; i++) {
                result[out_pos++] = buf[i];
            }
            in_pos += 2;
        }
        else {
            result[out_pos++] = str[in_pos++];
        }
    }

    if (in_pos < str_len) {
        positional_refuse_output();
        out_pos = 0;
    }
    result[out_pos] = 0;
    g_stack_top--;

    /* ADR-0036 W5: store only if nothing during expansion raised a param
     * error (set -u unbound var, etc. — g_param_error is global/transient,
     * not worth caching) and the expanded result fits var_cache's fixed
     * value[512] — a longer result would silently truncate on store and
     * come back truncated on every subsequent hit. */
    if (cacheable && !g_param_error && out_pos < 511)
        jash_var_cache_store(s, str, result);

    return result;
}

const char* jash_param_expand(const char* str, void* state) {
    return param_expand_impl(str, state, 0);
}

/* Command-argument variant: same as jash_param_expand but performs IFS
 * word-splitting on UNQUOTED expansion results (BUG-010). Used by the command
 * dispatcher only — assignment RHS and other contexts call the plain version so
 * their values are never split. F E R M I ∞ H A R T */
const char* jash_param_expand_split(const char* str, void* state) {
    if (!str || !jash_has_param_expansion(str)) return str;
    return jash_param_expand_split_known(str, state);
}

const char* jash_param_expand_split_known(const char* str, void* state) {
    if (!str) return str;
    int saved = g_pe_split;
    g_pe_split = 1;
    for (int i = 0; i < 256; i++) g_pe_ifs_map[i] = 0;
    jash_state_t* s = (jash_state_t*)state;
    const char* ifs = s ? jash_env_get(s, "IFS") : NULL;
    /* IFS unset → default whitespace (space/tab/newline); IFS="" → no splitting.
     * Either way only non-whitespace chars become hard field separators. */
    if (ifs) {
        for (const char* ip = ifs; *ip; ip++) {
            unsigned char ic = (unsigned char)*ip;
            if (ic != ' ' && ic != '\t' && ic != '\n') g_pe_ifs_map[ic] = 1;
        }
    }
    const char* r = param_expand_impl(str, state, 1);
    g_pe_split = saved;
    return r;
}

/* ═══ JINV-087 — a variante de CAMPOS (lista de palavras do `for`) ═════════
 *
 * Mesma regra do POSIX da irmã acima, com a marca que o tokenizador do `for`
 * entende: JASH_FIELD_SEP em vez de espaço. A diferença não é cosmética —
 * é o que permite exprimir as duas coisas que o espaço não exprime:
 *
 *   campo VAZIO      `IFS=:; v=a::b`  → [a] [] [b]  (o espaço colapsaria)
 *   espaço que é DADO `IFS=:; v='a b:c'` → [a b] [c] (o espaço cortaria)
 *
 * E, principalmente: quem NÃO passa por aqui não é dividido. Texto literal e
 * resultado de expansão citada chegam ao tokenizador sem marca nenhuma, que é
 * exatamente o que o bash faz. */
const char* jash_param_expand_split_fields(const char* str, void* state) {
    if (!str || !jash_has_param_expansion(str)) return str;
    int saved = g_pe_split;
    g_pe_split = 2;
    jash_ifs_field_begin(state, 1);
    const char* r = param_expand_impl(str, state, 1);
    jash_ifs_field_end();
    g_pe_split = saved;
    return r;
}
