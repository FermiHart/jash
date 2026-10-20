// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/grep — disruptive grep builtin with smart cache,
// fuzzy matching, and context awareness.
//
// Features:
//   grep "pattern" file     — search for pattern (cached, 2nd call: 1ms vs 500ms)
//   grep --fuzzy "usrnm"   — fuzzy match: finds "username" ✓
//   grep --context "TODO"  — shows 3 lines before/after
//   grep --smart "fix bug" — understands "fix bug", "fixed bugs", "fixes bug"
//   grep -i "pattern"      — case-insensitive
//   grep -r "pattern" dir  — recursive search
//
// Smart Cache: keyed on file mtime+size (ADR-0044) — a hit is served only
// while the file is provably unchanged, including negative (no-match) results.
// Fuzzy Matching: Levenshtein distance for typo tolerance
// Smart Matching: understands word variations (fix/fixed/fixes)
//
// ADR-0044 gold: --json emits {file,line,col,match} records; --since SEQ
// scopes a recursive search to files touched at/after chain sequence SEQ.

#include "builtin.h"
#include <core/env.h>
#include <core/cache.h>
#include <core/chain_read.h>
#include <core/json_writer.h>
#include <core/string_compat.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <osif/osif_stat.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <ctype.h>
#include <fnmatch.h>
#include "grep_query.h"
/* REQ-2: POSIX ERE/BRE via libc regex. Guarded so a freestanding target
 * (Josh OS ring-3 / BeyondBSD) without <regex.h> can fall back to the
 * literal/BRE path by building with -DJASH_NO_REGEX. */
#ifndef JASH_NO_REGEX
#include <regex.h>
#define JASH_HAVE_REGEX 1
#endif

/* ── Minimal BRE matcher ──────────────────────────────────────────────────────
 * Handles the most common bash-grep idioms without dragging in <regex.h>:
 *   .       any single character
 *   ^pat    pattern anchored at start
 *   pat$    pattern anchored at end
 *   literal text otherwise
 * No backtracking, no character classes — just enough for `grep -c .` /
 * `grep '^foo'` / `grep 'bar$'` to behave like real grep. Falls back to
 * strstr() for everything else (legacy behavior preserved).
 * F E R M I ∞ H A R T */
static int bre_match_here(const char* line, const char* pat) {
    while (*pat) {
        if (*pat == '$' && pat[1] == '\0') return *line == '\0' || *line == '\n';
        if (*line == '\0' || *line == '\n') return 0;
        if (*pat != '.' && *pat != *line) return 0;
        line++; pat++;
    }
    return 1;
}
static int bre_search(const char* line, const char* pat) __attribute__((unused));
static int bre_search(const char* line, const char* pat) {
    if (*pat == '^') return bre_match_here(line, pat + 1);
    do {
        if (bre_match_here(line, pat)) return 1;
    } while (*line++ && *line != '\n');
    return 0;
}
/* Returns 1 if pattern contains a BRE meta-character we actually handle. */
static int has_bre_meta(const char* pat) __attribute__((unused));
static int has_bre_meta(const char* pat) {
    for (const char* p = pat; *p; p++) {
        if (*p == '.' || *p == '^' || (*p == '$' && p[1] == '\0')) return 1;
    }
    return 0;
}

/* ── REQ-2: unified regex matcher (regcomp/regexec) ───────────────────────────
 * Compile the pattern once per grep invocation. -E → ERE, else BRE; -i → ICASE.
 * Plain-literal patterns (no metachars) keep the allocation-free strstr path.
 * Flag bits: 0x01 -i, 0x200 -E, 0x400 -F (fixed/literal). */
static int has_regex_meta(const char* pat) {
    for (const char* p = pat; *p; p++)
        if (strchr(".^$[]*+?(){}|\\", *p)) return 1;
    return 0;
}

#ifdef JASH_HAVE_REGEX
static regex_t g_grep_re;
static int     g_grep_use_regex = 0;   /* 1 when g_grep_re holds a live program */

/* Word-forming atom lookahead for \b translation below. */
static int grep_is_word_atom(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '[';
}

/* Bounded append of a NUL-terminated string; returns 0 if it didn't fit. */
static int xlat_append(char* buf, size_t* o, size_t cap, const char* s) {
    size_t n = strlen(s);
    if (*o + n >= cap) return 0;
    memcpy(buf + *o, s, n);
    *o += n;
    return 1;
}

/* JINV-105: characters that are plain literals in a POSIX BRE but
 * metacharacters in an ERE. When a BRE is handed to regcomp() with
 * REG_EXTENDED, every bare occurrence of one of these silently changes
 * meaning: BRE `a+b` matches the three characters "a+b", ERE a+b matches
 * "aab". They must be re-escaped on the way across. */
static int grep_bre_meta_in_ere(char c) {
    return c == '+' || c == '?' || c == '|' ||
           c == '(' || c == ')' || c == '{' || c == '}';
}

/* End of the bracket expression starting at p (p points at '['), or NULL if it
 * is unterminated (in which case the '[' is just a literal). Inside a bracket
 * expression every character above is literal in BOTH grammars, so nothing in
 * here may be rewritten. POSIX: a ']' first in the list -- after an optional
 * '^' -- is a literal, and [: :] [. .] [= =] nest. */
static const char* grep_brack_end(const char* p) {
    const char* q = p + 1;
    if (*q == '^') q++;
    if (*q == ']') q++;
    for (; *q; q++) {
        if (*q == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
            char kind = q[1];
            const char* r = q + 2;
            while (*r && !(*r == kind && r[1] == ']')) r++;
            if (!*r) return NULL;              /* unterminated [: :] */
            q = r + 1;                         /* lands on the class's ']' */
            continue;
        }
        if (*q == ']') return q;
    }
    return NULL;                               /* unterminated '[' */
}

/* The GNU BRE extensions that POSIX BRE genuinely cannot express, so the only
 * way to honour them is to compile the pattern as an ERE. Note what is NOT
 * here: \( \) \{ \} are *native* POSIX BRE grouping and interval syntax.
 * Promoting those too (as this code originally did) forced an ERE compile for
 * patterns that never needed one, and an ERE has no backreferences — so
 * `grep '\(a\)\1'` quietly matched nothing instead of matching doubled a's. */
static int grep_bre_gnu_ext(char c) {
    return c == '|' || c == '+' || c == '?';
}

/* Will this BRE be promoted to an ERE? True iff it uses a GNU extension escape
 * outside a bracket expression. This has to be decided up front: the escaping
 * of bare metas depends on the fate of the whole pattern, and a `\|` at the
 * end retroactively changes how a '+' at the start must be emitted. */
static int grep_bre_promotes(const char* pat) {
    for (const char* q = pat; *q; q++) {
        if (*q == '[') {
            const char* e = grep_brack_end(q);
            if (e) q = e;
            continue;
        }
        if (*q == '\\' && q[1]) {
            if (grep_bre_gnu_ext(q[1])) return 1;
            q++;
        }
    }
    return 0;
}

/* Does this pattern have a top-level alternation (unescaped '|' outside a
 * bracket expression)? Only then does anchoring need a wrapping group. */
static int grep_has_top_alt(const char* pat) {
    for (const char* q = pat; *q; q++) {
        if (*q == '[') {
            const char* e = grep_brack_end(q);
            if (e) q = e;
            continue;
        }
        if (*q == '\\' && q[1]) { q++; continue; }
        if (*q == '|') return 1;
    }
    return 0;
}

/* Drop a leading '^' / trailing unescaped '$' in place. Used only under -x,
 * where both ends are already pinned: re-anchoring `^a+b$` would produce
 * `^^a+b$$`, which demands a literal '^' and can never match. */
static void grep_strip_anchors(char* p) {
    size_t len = strlen(p);
    if (len && p[len - 1] == '$') {
        size_t nb = 0, i = len - 1;
        while (i > 0 && p[i - 1] == '\\') { nb++; i--; }
        if (nb % 2 == 0) p[--len] = '\0';      /* unescaped '$' */
    }
    if (p[0] == '^') memmove(p, p + 1, len);   /* len includes the NUL slot */
}

/* BUG-042: translate GNU-only regex escapes (\b \s \S \w \W) that POSIX
 * regcomp() silently fails to match (it compiles them without error but
 * never matches — a false-negative footgun for anyone grepping code).
 * Confirmed via direct regcomp()/regexec() probe: \b/\s/\w compile clean but
 * never match on this platform's libc. The word-boundary replacement is
 * libc-specific — [[:<:]]/[[:>:]] on the BSDs, \<\> on glibc — see the 'b'
 * case below; both match in BOTH BRE and ERE without REG_EXTENDED.
 * Also drops the backslash from GNU BRE extensions \| \+ \? \( \) \{ \}
 * (BUG-038/grep) which POSIX BRE treats as a literal escaped char, so
 * `grep 'a\|b'` matched nothing without this. Returns 1 if the pattern uses
 * an escape we cannot faithfully translate (\d \D \B \A \Z) — caller must
 * fail loudly rather than silently returning zero matches. */
static int grep_translate(const char* pat, int ere_in, int pcre, char* out, size_t out_cap,
                          int* out_ere) {
    size_t o = 0;
    int ere = ere_in;
    /* JINV-105: decided before the first byte is emitted — see grep_bre_promotes. */
    int promote = !ere_in && grep_bre_promotes(pat);
    const char* brack_stop = NULL;             /* last byte of the open [...] */
    for (const char* q = pat; *q; q++) {
        int in_brack = (brack_stop && q <= brack_stop);
        if (*q != '\\' || !q[1]) {
            if (!in_brack && *q == '[') {
                const char* e = grep_brack_end(q);
                if (e) brack_stop = e;
            } else if (promote && !in_brack && grep_bre_meta_in_ere(*q)) {
                /* Literal in the BRE the user wrote; would be a meta in the
                 * ERE we are about to compile. Carry it across as a literal. */
                if (o + 1 >= out_cap) return 1;
                out[o++] = '\\';
            }
            if (o + 1 >= out_cap) return 1;
            out[o++] = *q;
            continue;
        }
        char x = q[1];
        /* GNU BRE extensions \| \+ \? \( \) \{ \} — POSIX BRE treats them as a
         * literal escaped char, not a meta. Only promote to bare ERE meta when
         * the pattern is NOT already -E (else `-E 'a\|b'` — which should mean
         * literal escaped pipe in ERE — would wrongly become alternation). */
        if (!ere_in && grep_bre_gnu_ext(x)) {
            if (o + 1 >= out_cap) return 1;
            out[o++] = x; q++; ere = 1;
            continue;
        }
        /* \( \) \{ \} are native BRE syntax and stay escaped in a BRE; they
         * only need un-escaping because something *else* forced this pattern
         * onto the ERE side. */
        if (promote && (x == '(' || x == ')' || x == '{' || x == '}')) {
            if (o + 1 >= out_cap) return 1;
            out[o++] = x; q++;
            continue;
        }
        /* A backreference cannot survive the crossing — POSIX ERE has none.
         * Fail loudly rather than compile something that quietly matches the
         * wrong lines. (Without a GNU extension in play the pattern stays a
         * BRE and \1 passes through to regcomp intact, below.) */
        if (promote && x >= '1' && x <= '9') return 1;
        /* Same story for an ERE the user asked for directly: POSIX ERE has no
         * backreferences and GNU grep rejects `-E '\(a\)\1'` outright, so
         * accepting it here would mean matching a literal '1' and calling it a
         * backreference. -P keeps them: PCRE really does have backrefs. */
        if (ere_in && !pcre && x >= '1' && x <= '9') return 1;
        switch (x) {
            case 'b': {
                char next = q[2];
                /* Word-boundary spelling is libc-specific, and this is the
                 * one translation that is NOT portable:
                 *   BSD/macOS  — [[:<:]] / [[:>:]]  (glibc rejects: bad class)
                 *   glibc      — \< / \>            (BSD rejects)
                 * Getting it wrong is loud (regcomp errors out with "invalid
                 * pattern"), not silent, but it means `grep -E '\bfoo\b'`
                 * simply does not run on the other platform. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
                const char* rep = grep_is_word_atom(next) ? "[[:<:]]" : "[[:>:]]";
#else
                const char* rep = grep_is_word_atom(next) ? "\\<" : "\\>";
#endif
                if (!xlat_append(out, &o, out_cap, rep)) return 1;
                q++;
                break;
            }
            case 's': if (!xlat_append(out, &o, out_cap, "[[:space:]]")) return 1; q++; break;
            case 'S': if (!xlat_append(out, &o, out_cap, "[^[:space:]]")) return 1; q++; break;
            case 'w': if (!xlat_append(out, &o, out_cap, "[[:alnum:]_]")) return 1; q++; break;
            case 'W': if (!xlat_append(out, &o, out_cap, "[^[:alnum:]_]")) return 1; q++; break;
            /* ADR-0044 grep parity: Perl-class escapes. Under -P (pcre) the
             * common ones agents type daily translate to their POSIX class —
             * `grep -oP '\d+'` is one of the most-reported "unknown option"
             * failures. Without -P these remain unsupported (loud error, as
             * before — never a silent wrong answer). */
            case 'd':
                if (!pcre) return 1;
                if (!xlat_append(out, &o, out_cap, "[[:digit:]]")) return 1;
                q++; break;
            case 'D':
                if (!pcre) return 1;
                if (!xlat_append(out, &o, out_cap, "[^[:digit:]]")) return 1;
                q++; break;
            case 'A':  /* \A start-of-input ≈ ^ ; \Z end ≈ $ (line-mode grep) */
                if (!pcre) return 1;
                if (o + 1 >= out_cap) { return 1; }
                out[o++] = '^';
                q++; break;
            case 'Z':
                if (!pcre) return 1;
                if (o + 1 >= out_cap) { return 1; }
                out[o++] = '$';
                q++; break;
            case 'B':  /* \B non-word-boundary: no POSIX equivalent — best-effort
                        * drop under -P; unsupported (loud) without -P. */
                if (!pcre) return 1;
                q++; break;
            default:
                /* Standard POSIX escape (\. \* \[ \] \^ \$ \\ \/ etc.) — pass through. */
                if (o + 2 >= out_cap) return 1;
                out[o++] = '\\'; out[o++] = x; q++;
                break;
        }
    }
    if (o >= out_cap) return 1;
    out[o] = '\0';
    *out_ere = ere;
    return 0;
}

/* Compile (or skip) the matcher for this invocation. Frees any previous
 * program first, so there is never more than one outstanding regex_t. */
static int grep_compile_program(const char* pat, int flags, regex_t* program, int* using_regex) {
    *using_regex = 0;
    if (flags & 0x400) return 0;                       /* -F: literal only (grep_match handles -x itself) */
    int pcre = (flags & 0x200000) != 0;                /* -P: PCRE-lite */
    int ere = (flags & 0x200) != 0 || pcre;            /* -P implies ERE (quantifiers +?{}) */
    char xlat[4096];
    if (grep_translate(pat, ere, pcre, xlat, sizeof(xlat), &ere) != 0) return 1;
    const char* use = xlat;
    int need = ere || has_regex_meta(use);
    int want_x = (flags & 0x40000) != 0;   /* -x: whole-line match */
    char anchored[4096 + 4];
    if (want_x) {
        /* Wrap in a group before anchoring — `^a|b$` would only anchor the
         * first/last alternative, not the whole expression. But only an ERE
         * can have a top-level alternation in the first place: a BRE that
         * wanted one would have used \| and already been promoted above.
         * JINV-105: forcing ERE here to get the group re-read the user's BRE
         * under the wrong grammar, so `grep -x 'a+b'` stopped matching "a+b"
         * and started matching "aab". Anchor a BRE as a BRE. */
        char body[4096];
        if (strlen(use) >= sizeof(body)) return 1;
        snprintf(body, sizeof(body), "%s", use);
        /* Only an ERE can alternate: in a BRE a bare '|' is an ordinary
         * character, so `grep -x 'a|b'` must match the literal line "a|b"
         * rather than be grouped and re-read as "a or b". */
        int alt = ere && grep_has_top_alt(body);
        if (!alt) grep_strip_anchors(body);    /* ^ and $ are redundant under -x */
        int n = alt ? snprintf(anchored, sizeof(anchored), "^(%s)$", body)
                    : snprintf(anchored, sizeof(anchored), "^%s$", body);
        if (n < 0 || (size_t)n >= sizeof(anchored)) return 1;
        if (alt) ere = 1;                      /* a group needs ERE; a plain anchor does not */
        use = anchored; need = 1;
    }
    if (!need) return 0;                               /* literal fast-path */
    /* -o / -w / -x need the match span or a specific span shape. */
    int want_span = (flags & 0x2000) || (flags & 0x4000) || want_x || g_grep_query;
    int cf = (want_span ? 0 : REG_NOSUB)
           | (ere ? REG_EXTENDED : 0)
           | ((flags & 0x01)  ? REG_ICASE    : 0);
    if (regcomp(program, use, cf) != 0) return 1;
    *using_regex = 1;
    return 0;
}
static int grep_compile(const char* pat, int flags) {
    if (g_grep_use_regex) { regfree(&g_grep_re); g_grep_use_regex = 0; }
    return grep_compile_program(pat, flags, &g_grep_re, &g_grep_use_regex);
}
static int grep_is_word(char c);
#include "grep_query_match.h"

static int grep_match(const char* line, const char* pattern, int flags) {
    if (g_grep_query && g_grep_query->matchers) {
        int start, end;
        return grep_query_span_multi(line, flags, 0, &start, &end);
    }
    if (g_grep_use_regex) return regexec(&g_grep_re, line, 0, NULL, 0) == 0;
    /* -x -F: whole-line match on a literal pattern (no regex compiled at all
     * for -F, so grep_compile never sees this case — handle it here). */
    if (flags & 0x40000) {
        return (flags & 0x01) ? strcasecmp(line, pattern) == 0
                              : strcmp(line, pattern) == 0;
    }
    if (flags & 0x01) return jash_strcasestr_portable(line, pattern) != NULL;
    return strstr(line, pattern) != NULL;
}

/* Find the FIRST match span in `line` starting at offset `from` → [*ms,*me).
 * Returns 1 on a match. Powers -o (only-matching) and -w (word boundary). */
static int grep_span(const char* line, const char* pattern, int flags,
                     int from, int* ms, int* me) {
    if (g_grep_query && g_grep_query->matchers)
        return grep_query_span_multi(line, flags, from, ms, me);
    const char* base = line + from;
#ifdef JASH_HAVE_REGEX
    if (g_grep_use_regex) {
        regmatch_t pm[1];
        if (regexec(&g_grep_re, base, 1, pm, from > 0 ? REG_NOTBOL : 0) != 0) return 0;
        *ms = from + (int)pm[0].rm_so; *me = from + (int)pm[0].rm_eo;
        return 1;
    }
#endif
    const char* hit = (flags & 0x01)
        ? jash_strcasestr_portable(base, pattern) : strstr(base, pattern);
    if (!hit) return 0;
    *ms = (int)(hit - line); *me = *ms + (int)strlen(pattern);
    return 1;
}

static int grep_is_word(char c) {
    return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* -w: does `line` contain the pattern as a WHOLE word? */
static int grep_word_match(const char* line, const char* pattern, int flags) {
    int ms = 0, me = 0, from = 0;
    while (grep_span(line, pattern, flags, from, &ms, &me)) {
        int lok = (ms == 0) || !grep_is_word(line[ms - 1]);
        int rok = (line[me] == '\0') || !grep_is_word(line[me]);
        if (lok && rok) return 1;
        from = (me > ms) ? me : ms + 1;          /* advance past this span */
        if (line[from] == '\0') break;
    }
    return 0;
}
#else
/* Freestanding fallback: no <regex.h>. Use the hand-rolled BRE for meta
 * patterns, literal strstr/strcasestr otherwise. (-E behaves like BRE.) */
static int grep_compile(const char* pat, int flags) { (void)pat; (void)flags; return 0; }
static int grep_match(const char* line, const char* pattern, int flags) {
    if (flags & 0x400) {  /* -F */
        return (flags & 0x01) ? jash_strcasestr_portable(line, pattern) != NULL
                              : strstr(line, pattern) != NULL;
    }
    if (flags & 0x01) {
        char ll[4096], lp[1024];
        size_t i = 0; for (; line[i] && i < sizeof(ll)-1; i++) ll[i] = (char)tolower((unsigned char)line[i]); ll[i] = 0;
        size_t j = 0; for (; pattern[j] && j < sizeof(lp)-1; j++) lp[j] = (char)tolower((unsigned char)pattern[j]); lp[j] = 0;
        return has_bre_meta(lp) ? bre_search(ll, lp) : (strstr(ll, lp) != NULL);
    }
    return has_bre_meta(pattern) ? bre_search(line, pattern) : (strstr(line, pattern) != NULL);
}
#endif

/* ── Growable output buffer ───────────────────────────────────────────────────
 * Replaces the old fixed 65536-byte accumulator (BUG: silently dropped any
 * append past capacity, no error, no marker). Never silently drops: grows by
 * doubling, only fails (returns 0) on true OOM. One of these is allocated
 * FRESH per file operand / per recursive-directory call (not shared across
 * the whole invocation) — the old shared-buffer design leaked one operand's
 * matches into the NEXT operand's cache entry. */
typedef struct { char* data; size_t len; size_t cap; } grep_outbuf_t;

static void outbuf_init(grep_outbuf_t* b) { b->data = NULL; b->len = 0; b->cap = 0; }

static int outbuf_append(grep_outbuf_t* b, const char* s, size_t n) {
    if (n == 0) return 1;
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 4096;
        while (newcap < b->len + n + 1) newcap *= 2;
        char* p = realloc(b->data, newcap);
        if (!p) return 0;   /* true OOM — leave existing content, drop this append */
        b->data = p; b->cap = newcap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static void outbuf_free(grep_outbuf_t* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* ── Cache infrastructure ─────────────────────────────────────────────────── */

#define GREP_CACHE_MAX 16

typedef struct {
    char key[1024];  /* pattern + file path */
    char* output;
    time_t timestamp; /* eviction tie-break only, not a TTL */
    time_t mtime;
    off_t  size;
    int    matched;   /* mirrors grep_file's result==0 */
} grep_cache_entry_t;

static grep_cache_entry_t g_grep_cache[GREP_CACHE_MAX];
static int g_grep_cache_count = 0;

/* ADR-0044: keyed on file mtime+size instead of a wall-clock TTL — a hit is
 * served ONLY while the file is provably unchanged (fixes the stale-after-
 * edit bug), which also makes this a correct NEGATIVE cache (see
 * grep_cache_update below — a no-match result is cached too). */
static grep_cache_entry_t* grep_cache_find(const char* pattern, const char* path, int flags) {
    char key[1024];
    /* flags MUST be in the key: `grep` and `grep -n` produce different output
     * for the same pattern+path; without it the second call served the first's. */
    snprintf(key, sizeof(key), "%d:%s:%s", flags, pattern, path);

    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    for (int i = 0; i < g_grep_cache_count; i++) {
        if (strcmp(g_grep_cache[i].key, key) == 0) {
            if (g_grep_cache[i].mtime == st.st_mtime && g_grep_cache[i].size == st.st_size)
                return &g_grep_cache[i];
            return NULL;  /* file changed since caching — stale, do not trust */
        }
    }
    return NULL;
}

static void grep_cache_update(const char* pattern, const char* path, int flags,
                              int matched, const char* output) {
    struct stat st;
    if (stat(path, &st) != 0) return;   /* can't validate later — don't cache */

    char key[1024];
    snprintf(key, sizeof(key), "%d:%s:%s", flags, pattern, path);

    /* Find existing or empty slot */
    grep_cache_entry_t* entry = NULL;
    for (int i = 0; i < g_grep_cache_count; i++) {
        if (strcmp(g_grep_cache[i].key, key) == 0) {
            entry = &g_grep_cache[i];
            break;
        }
    }

    if (!entry && g_grep_cache_count < GREP_CACHE_MAX) {
        entry = &g_grep_cache[g_grep_cache_count++];
    } else if (!entry) {
        /* Evict oldest */
        entry = &g_grep_cache[0];
        for (int i = 1; i < g_grep_cache_count; i++) {
            if (g_grep_cache[i].timestamp < entry->timestamp) {
                entry = &g_grep_cache[i];
            }
        }
    }

    strlcpy(entry->key, key, sizeof(entry->key));
    free(entry->output);
    entry->output = strdup(output ? output : "");
    entry->timestamp = time(NULL);
    entry->mtime = st.st_mtime;
    entry->size = st.st_size;
    entry->matched = matched;
}

/* ── Fuzzy matching (Levenshtein distance) ────────────────────────────────── */

static int levenshtein(const char* s1, const char* s2) {
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    
    if (len1 > 64 || len2 > 64) return 999;  /* Too long */
    
    int d[65][65];
    
    for (int i = 0; i <= len1; i++) d[i][0] = i;
    for (int j = 0; j <= len2; j++) d[0][j] = j;
    
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            int cost = (tolower((unsigned char)s1[i-1]) == tolower((unsigned char)s2[j-1])) ? 0 : 1;
            d[i][j] = d[i-1][j] + 1;
            if (d[i][j-1] + 1 < d[i][j]) d[i][j] = d[i][j-1] + 1;
            if (d[i-1][j-1] + cost < d[i][j]) d[i][j] = d[i-1][j-1] + cost;
        }
    }
    
    return d[len1][len2];
}

static int fuzzy_match_word(const char* word, const char* pattern, int threshold) {
    /* Check if any substring of word matches pattern within threshold */
    int wlen = strlen(word);
    int plen = strlen(pattern);
    
    if (plen == 0) return 0;
    /* levenshtein() is capped at 64-byte operands; a longer pattern must be a
     * safe non-match, never an index past substr[65]. */
    if (plen > 64) return 0;
    
    /* Slide window of size plen across word */
    for (int i = 0; i <= wlen - plen; i++) {
        char substr[65];
        strncpy(substr, word + i, plen);
        substr[plen] = '\0';
        
        int dist = levenshtein(substr, pattern);
        if (dist <= threshold) return 1;
    }
    
    return 0;
}

/* ── Smart matching (word variations) ─────────────────────────────────────── */

static int smart_match(const char* line, const char* pattern) {
    /* Simple heuristic: match pattern words with common variations */
    /* For "fix bug", match "fix", "fixed", "fixes", "fixing", "bug", "bugs" */
    
    char pattern_copy[256];
    strlcpy(pattern_copy, pattern, sizeof(pattern_copy));
    
    char* word = strtok(pattern_copy, " \t");
    while (word) {
        /* Check base word */
        if (jash_strcasestr_portable(line, word)) return 1;
        
        /* Check common suffixes */
        char variation[256];
        
        /* -ed */
        snprintf(variation, sizeof(variation), "%sed", word);
        if (jash_strcasestr_portable(line, variation)) return 1;
        
        /* -s */
        snprintf(variation, sizeof(variation), "%ss", word);
        if (jash_strcasestr_portable(line, variation)) return 1;
        
        /* -ing */
        snprintf(variation, sizeof(variation), "%sing", word);
        if (jash_strcasestr_portable(line, variation)) return 1;
        
        word = strtok(NULL, " \t");
    }
    
    return 0;
}

/* ── Core grep logic ──────────────────────────────────────────────────────── */

/* Context line counts: -A (after), -B (before), -C (both). Set by arg parser. */
static int g_ctx_after = 0;
static int g_ctx_before = 0;
static int g_grep_json_errors = 0;

#define GREP_JSON_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define GREP_JSON_MAX_LINE_BYTES 65535u

/* ADR-0044 grep pass 2: -m NUM (stop after N matches per file), and
 * --exclude-dir / --exclude globs for recursive search (skip node_modules,
 * .git, build, vendored trees that otherwise flood the agent's window). */
static int  g_grep_max = 0;                 /* -m: 0 = unlimited */
static char g_grep_excl_dir[16][128];       /* --exclude-dir globs */
static int  g_grep_excl_dir_n = 0;
static char g_grep_excl[16][128];           /* --exclude globs */
static int  g_grep_excl_n = 0;
/* JINV-104: --include is a UNION, exactly like --exclude above. Holding it in a
 * single scalar made the last glob win, so `--include=*.h --include=*.c`
 * silently never visited the .h files — a false negative that reads as "not in
 * the codebase". Same storage shape for both flags, same OR semantics. */
static char g_grep_incl[16][128];           /* --include globs */
static int  g_grep_incl_n = 0;

/* ADR-0044 gold: --since SEQ — resolved once (chain seq → timestamp) in
 * jash_b_grep, then consulted by grep_recursive's tree walk to skip files
 * that predate the cutoff. */
static time_t g_grep_since_cutoff = 0;
static int    g_grep_since_active = 0;

static int grep_name_excluded(char (*globs)[128], int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (fnmatch(globs[i], name, 0) == 0) return 1;
    return 0;
}

/* JINV-104: the same "matches any glob" predicate, read positively. --include
 * is a whitelist: with no --include at all every file passes, otherwise the
 * name must match at least ONE of the globs (union, GNU semantics). */
static int grep_name_included(char (*globs)[128], int n, const char* name) {
    if (n == 0) return 1;
    return grep_name_excluded(globs, n, name);
}

/* Dynamic context line buffers */
static char** g_ctx_buf = NULL;
static int g_ctx_buf_size = 0;
static int g_ctx_buf_pos = 0;

static void ctx_buf_free(void);
static void ctx_buf_init(int n) {
    ctx_buf_free();
    if (n <= 0) return;
    g_ctx_buf = (char**)calloc((size_t)n, sizeof(char*));
    g_ctx_buf_size = n;
    g_ctx_buf_pos = 0;
}

static void ctx_buf_add(const char* line) {
    if (!g_ctx_buf || g_ctx_buf_size <= 0) return;
    free(g_ctx_buf[g_ctx_buf_pos]);
    g_ctx_buf[g_ctx_buf_pos] = strdup(line);
    g_ctx_buf_pos = (g_ctx_buf_pos + 1) % g_ctx_buf_size;
}

static void ctx_buf_flush(int show_filename, const char* path, int base_line,
                          grep_outbuf_t* out) {
    if (!g_ctx_buf || g_ctx_buf_size <= 0) return;
    for (int i = 0; i < g_ctx_buf_size; i++) {
        int idx = (g_ctx_buf_pos + i) % g_ctx_buf_size;
        if (g_ctx_buf[idx] && g_ctx_buf[idx][0]) {
            char ctx_line[65536];
            int ctx_ln = base_line - g_ctx_buf_size + i;
            int k;
            if (show_filename)
                k = snprintf(ctx_line, sizeof(ctx_line), "%s-%d-%s\n", path, ctx_ln, g_ctx_buf[idx]);
            else
                k = snprintf(ctx_line, sizeof(ctx_line), "%d-%s\n", ctx_ln, g_ctx_buf[idx]);
            if (k > 0) outbuf_append(out, ctx_line, (size_t)k);
        }
    }
}

static void ctx_buf_free(void) {
    if (g_ctx_buf) {
        for (int i = 0; i < g_ctx_buf_size; i++) free(g_ctx_buf[i]);
        free(g_ctx_buf);
        g_ctx_buf = NULL;
        g_ctx_buf_size = 0;
        g_ctx_buf_pos = 0;
    }
}

/* ── ADR-0044 gold: JSON text/byte encoding and explicit input limits ─────── */

static int grep_json_span_utf8(const unsigned char* bytes, size_t length) {
    if (memchr(bytes, 0, length)) return 0;
    for (size_t offset = 0; offset < length;) {
        size_t width = jash_json_utf8_span(bytes + offset);
        if (!width || width > length - offset) return 0;
        offset += width;
    }
    return 1;
}

static int grep_json_write(jash_writer_t writer, void* context,
                           const char* bytes, size_t length) {
    return writer(context, bytes, length) == (long)length ? 0 : -1;
}

/* Preserve the legacy string value for valid UTF-8; arbitrary path/match bytes
 * use the shared tagged-base64 codec rather than emitting invalid JSON. */
static int grep_json_str_n(jash_writer_t writer, void* context,
                           const char* text, size_t length) {
    if (!grep_json_span_utf8((const unsigned char*)text, length)) {
        jash_json_writer_t json;
        jash_jsonw_init(&json, writer, context);
        jash_jsonw_bytes(&json, text, length);
        return jash_jsonw_finish(&json);
    }
    if (grep_json_write(writer, context, "\"", 1) < 0) return -1;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (byte == '"' || byte == '\\') {
            char escaped[2] = {'\\', (char)byte};
            if (grep_json_write(writer, context, escaped, sizeof escaped) < 0) return -1;
        } else if (byte == '\n') {
            if (grep_json_write(writer, context, "\\n", 2) < 0) return -1;
        } else if (byte == '\t') {
            if (grep_json_write(writer, context, "\\t", 2) < 0) return -1;
        } else if (byte < 0x20) {
            char escaped[8];
            int count = snprintf(escaped, sizeof escaped, "\\u%04x", byte);
            if (count < 0 || grep_json_write(writer, context, escaped, (size_t)count) < 0) return -1;
        } else if (grep_json_write(writer, context, (const char*)&text[i], 1) < 0) {
            return -1;
        }
    }
    return grep_json_write(writer, context, "\"", 1);
}

static int grep_json_str(jash_writer_t writer, void* context, const char* text) {
    return grep_json_str_n(writer, context, text, strlen(text));
}

static int grep_json_error_row(jash_writer_t writer, void* context,
                               int* json_first, const char* path,
                               const char* reason) {
    if (g_grep_query && g_grep_query->native)
        return grep_query_add(path, 0, 0, 0, NULL, 0, reason);
    if (g_grep_query && *path) {
        size_t length = strlen(path) + strlen(reason);
        if (g_grep_query->json_records >= QUERY_DATA_MAX_ITEMS ||
            length > QUERY_DATA_MAX_BYTES / 2u - g_grep_query->json_bytes) {
            g_grep_query->capped = 1; return 0;
        }
        g_grep_query->json_records++; g_grep_query->json_bytes += length;
    }
    if (!*json_first && grep_json_write(writer, context, ",", 1) < 0) return -1;
    *json_first = 0;
    jash_json_writer_t json;
    jash_jsonw_init(&json, writer, context);
    jash_jsonw_raw(&json, "{\"file\":");
    jash_jsonw_string_or_bytes(&json, path);
    jash_jsonw_raw(&json, ",\"error\":"); jash_jsonw_string(&json, reason);
    jash_jsonw_raw(&json, ",\"complete\":false}");
    return jash_jsonw_finish(&json);
}

/* Both projections consume the same byte span. The legacy JSON payload cap
 * remains explicit; the native record retains every observed match byte. */
static int grep_match_row(jash_writer_t writer, void* context, int* first,
                           const char* path, uint64_t line, uint64_t start,
                           uint64_t end, const char* bytes, size_t length) {
    if (g_grep_query && g_grep_query->native)
        return grep_query_add(path, line, start, end, bytes, length, NULL);
    size_t selected = length > 4000u ? 4000u : length;
    if (g_grep_query) {
        size_t size = strlen(path) + selected;
        if (g_grep_query->json_records >= QUERY_DATA_MAX_ITEMS ||
            size > QUERY_DATA_MAX_BYTES / 2u - g_grep_query->json_bytes) {
            g_grep_query->capped = 1; return 0;
        }
        g_grep_query->json_records++; g_grep_query->json_bytes += size;
    }
    if (!*first && grep_json_write(writer, context, ",", 1) < 0) return -1;
    *first = 0;
    if (grep_json_write(writer, context, "{\"file\":", 8) < 0 ||
        grep_json_str(writer, context, path) < 0) return -1;
    char numbers[192];
    int count = snprintf(numbers, sizeof(numbers), ",\"line\":%llu,\"col\":%llu,\"match\":",
        (unsigned long long)line, (unsigned long long)(start + 1u));
    if (count < 0 || (size_t)count >= sizeof(numbers) ||
        grep_json_write(writer, context, numbers, (size_t)count) < 0 ||
        grep_json_str_n(writer, context, bytes, selected) < 0) return -1;
    count = snprintf(numbers, sizeof(numbers),
        ",\"span\":{\"start\":%llu,\"end\":%llu},\"match_truncated\":%s}",
        (unsigned long long)start, (unsigned long long)end,
        length > selected ? "true" : "false");
    return count < 0 || (size_t)count >= sizeof(numbers) ? -1 :
        grep_json_write(writer, context, numbers, (size_t)count);
}

/* --json is a text-search contract, not a binary grep mode. Preflight each
 * regular file before emitting its records so NUL data and overlong lines are
 * reported as an explicit error row rather than a partial match stream. */
static int grep_json_preflight(FILE* file, struct stat* before,
                              const char** reason) {
    if (fstat(fileno(file), before) < 0) { *reason = "stat_failed"; return -1; }
    if (!S_ISREG(before->st_mode)) { *reason = "non_regular_input"; return -1; }
    if (before->st_size < 0 || (uint64_t)before->st_size > GREP_JSON_MAX_FILE_BYTES) {
        *reason = "file_exceeds_json_limit"; return -1;
    }
    unsigned char buffer[8192];
    size_t line_bytes = 0;
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, file)) > 0) {
        if (grep_query_stop()) { *reason = "request_cancelled"; return -1; }
        if (g_grep_query) {
            if (count > 256u * 1024u * 1024u - g_grep_query->scanned_bytes) {
                g_grep_query->capped = 1; *reason = "query_collection_limit"; return -1;
            }
            g_grep_query->scanned_bytes += count;
        }
        if (memchr(buffer, 0, count)) { *reason = "binary_input"; return -1; }
        for (size_t i = 0; i < count; i++) {
            if (buffer[i] == '\n') line_bytes = 0;
            else if (++line_bytes > GREP_JSON_MAX_LINE_BYTES) {
                *reason = "line_exceeds_json_limit";
                return -1;
            }
        }
    }
    if (ferror(file)) { *reason = "read_failed"; return -1; }
    clearerr(file);
    if (fseeko(file, 0, SEEK_SET) != 0) { *reason = "seek_failed"; return -1; }
    return 0;
}

static int grep_stat_unchanged(const struct stat* before, const struct stat* after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           BEAR_ST_MTIM(*before).tv_sec == BEAR_ST_MTIM(*after).tv_sec &&
           BEAR_ST_MTIM(*before).tv_nsec == BEAR_ST_MTIM(*after).tv_nsec &&
           BEAR_ST_CTIM(*before).tv_sec == BEAR_ST_CTIM(*after).tv_sec &&
           BEAR_ST_CTIM(*before).tv_nsec == BEAR_ST_CTIM(*after).tv_nsec;
}

/* json_mode: 0 = normal (append text into `out`); 1 = emit one JSON object
 * per match straight through `w`/`ctx` (bypasses `out` and the cache, since
 * the array wrapper spans multiple files — see jash_b_grep). `json_first`
 * tracks comma placement across the whole invocation (shared across files). */
static int grep_file(const char* path, const char* pattern, int flags,
                     jash_writer_t w, void* ctx, grep_outbuf_t* out,
                     int json_mode, int* json_first) {
    if (json_mode && grep_query_stop()) return 1;
    size_t first_observation = g_grep_query ? g_grep_query->count : 0;
    FILE* fp;
    if (json_mode) {
        int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        fp = descriptor < 0 ? NULL : fdopen(descriptor, "rb");
        if (descriptor >= 0 && !fp) { int saved = errno; close(descriptor); errno = saved; }
    } else fp = fopen(path, "rb");
    if (!fp) {
        if (json_mode) {
            g_grep_json_errors++;
            if (grep_json_error_row(w, ctx, json_first, path, "open_failed") < 0) return 125;
        } else jash_writef(jash_stderr_writer, 0, "grep: cannot open '%s'\n", path);
        return json_mode ? 2 : 1;
    }

    struct stat json_before = {0};
    int json_input_checked = 0;
    if (json_mode) {
        const char* reason = NULL;
        if (grep_json_preflight(fp, &json_before, &reason) < 0) {
            if (g_grep_query && (g_grep_query->cancelled || g_grep_query->capped)) {
                fclose(fp); return 1;
            }
            g_grep_json_errors++;
            int emitted = grep_json_error_row(w, ctx, json_first, path,
                                               reason ? reason : "read_failed");
            fclose(fp);
            return emitted < 0 ? 125 : 2;
        }
        json_input_checked = 1;
    }

    int fuzzy_mode = flags & 0x02;
    int smart_mode = flags & 0x04;
    int show_filename = flags & 0x10;
    int invert = (flags & 0x20) != 0;

    /* 64KB/line cap — matches wc/awk/diff's read-buffer convention; a line
     * longer than this is truncated at the boundary (rare: minified/generated
     * files). Not unbounded. */
    char line[65537];
    int line_num = 0;
    int matches = 0;

    /* After-context counter: lines remaining to print after a match */
    int after_pending = 0;
    /* Track if we are inside a match group (suppresses before-context buffering) */
    int in_group = 0;

    /* Initialize before-context circular buffer */
    int before_n = g_ctx_before > 0 ? g_ctx_before : 0;
    ctx_buf_init(before_n);

    while (fgets(line, sizeof(line), fp)) {
        if (json_mode && grep_query_stop()) break;
        line_num++;

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        int match = 0;

        if (fuzzy_mode) {
            char line_copy[65536];
            strlcpy(line_copy, line, sizeof(line_copy));
            char* word = strtok(line_copy, " \t");
            while (word) {
                if (fuzzy_match_word(word, pattern, 2)) { match = 1; break; }
                word = strtok(NULL, " \t");
            }
        } else if (smart_mode) {
            match = smart_match(line, pattern);
        } else if (flags & 0x4000) {
            match = grep_word_match(line, pattern, flags);
        } else {
            match = grep_match(line, pattern, flags);
        }

        int hit = invert ? !match : match;
        if (hit) {
            matches++;
            /* JINV-127: -q dominates every other output mode. This branch did
             * not exist: grep_file() is the FILE path, and the only quiet
             * handling lived in the separate stdin loop below (`if (flags &
             * 0x100) return 0`), so `grep -q pat file` printed every match and
             * only the exit code looked right. -q outranks -c/-l/-L (verified
             * against GNU: -qc/-ql/-qL all emit nothing and exit purely on
             * "did any line match"), so it must be tested FIRST. */
            if (flags & 0x100) { (void)fclose(fp); ctx_buf_free(); return 0; }
            /* -L (files-without-match): a single hit disqualifies the file;
             * stop early and emit nothing. */
            if (flags & 0x400000) { (void)fclose(fp); ctx_buf_free(); return 1; }
            if (flags & 0x80) {
                if (g_grep_max > 0 && matches >= g_grep_max) break;  /* -m with -c */
                continue;
            }
            if (flags & 0x1000) {
                char _fl[4096]; int k = snprintf(_fl, sizeof(_fl), "%s\n", path);
                if (k > 0) outbuf_append(out, _fl, (size_t)k);
                (void)fclose(fp); return 0;
            }
            /* Flush before-context buffer */
            if (before_n > 0 && !in_group) {
                if (out->len > 0 && out->data[out->len - 1] != '\n') outbuf_append(out, "\n", 1);
                ctx_buf_flush(show_filename, path, line_num, out);
            }
            in_group = 1;

            if (json_mode) {
                int start = 0, end = 0, from = 0;
                while (from <= (int)len && grep_span(line, pattern, flags, from, &start, &end)) {
                    if (grep_query_stop()) break;
                    int accepted = !(flags & 0x4000) ||
                        ((start == 0 || !grep_is_word(line[start - 1])) &&
                         (line[end] == '\0' || !grep_is_word(line[end])));
                    if (accepted && grep_match_row(w, ctx, json_first, path,
                            (uint64_t)line_num, (uint64_t)start, (uint64_t)end,
                            line + start, (size_t)(end - start)) < 0) {
                        fclose(fp); ctx_buf_free(); return 125;
                    }
                    if (accepted && !(flags & 0x2000)) break;
                    from = end > start ? end : end + 1;
                }
            } else if (flags & 0x2000) {
                /* -o: print only matched spans */
                int _ms = 0, _me = 0, _fr = 0;
                while (grep_span(line, pattern, flags, _fr, &_ms, &_me) && _me > _ms) {
                    _fr = _me;
                    /* BUG-070: with -w, only emit a span whose edges are word
                     * boundaries — otherwise `grep -wo the` on "the theater"
                     * wrongly matched the "the" inside "theater". */
                    if (flags & 0x4000) {
                        int lok = (_ms == 0) || !grep_is_word(line[_ms - 1]);
                        int rok = (line[_me] == '\0') || !grep_is_word(line[_me]);
                        if (!(lok && rok)) continue;
                    }
                    char _ol[65536]; int _seg = _me - _ms; if (_seg > 4000) _seg = 4000;
                    int k;
                    if (show_filename) k = snprintf(_ol, sizeof(_ol), "%s:%.*s\n", path, _seg, line + _ms);
                    else               k = snprintf(_ol, sizeof(_ol), "%.*s\n", _seg, line + _ms);
                    if (k > 0) outbuf_append(out, _ol, (size_t)k);
                }
            } else {
                char match_line[65536];
                int show_n = (flags & 0x40) != 0;
                int k;
                if (show_filename && show_n)
                    k = snprintf(match_line, sizeof(match_line), "%s:%d:%s\n", path, line_num, line);
                else if (show_filename)
                    k = snprintf(match_line, sizeof(match_line), "%s:%s\n", path, line);
                else if (show_n)
                    k = snprintf(match_line, sizeof(match_line), "%d:%s\n", line_num, line);
                else
                    k = snprintf(match_line, sizeof(match_line), "%s\n", line);
                if (k > 0) outbuf_append(out, match_line, (size_t)k);
            }

            /* Set after-context pending lines */
            if (g_ctx_after > 0) after_pending = g_ctx_after;
            /* -m NUM: stop after NUM matching lines (per file), once any
             * pending after-context is exhausted. */
            if (g_grep_max > 0 && matches >= g_grep_max && after_pending == 0) {
                if (g_grep_query) g_grep_query->match_limited = 1;
                break;
            }
        } else if (after_pending > 0) {
            /* Print after-context line */
            char ctx_line[65536];
            int show_n = (flags & 0x40) != 0;
            int k;
            if (show_filename && show_n)
                k = snprintf(ctx_line, sizeof(ctx_line), "%s:%d-%s\n", path, line_num, line);
            else if (show_filename)
                k = snprintf(ctx_line, sizeof(ctx_line), "%s-%s\n", path, line);
            else if (show_n)
                k = snprintf(ctx_line, sizeof(ctx_line), "%d-%s\n", line_num, line);
            else
                k = snprintf(ctx_line, sizeof(ctx_line), "%s\n", line);
            if (k > 0) outbuf_append(out, ctx_line, (size_t)k);
            after_pending--;
            in_group = 1;
        } else {
            /* No match, no after-pending: store in before-context buffer if not in a group */
            if (before_n > 0 && !in_group) ctx_buf_add(line);
            if (before_n > 0 && in_group) in_group = 0;
        }
    }

    if (json_mode && json_input_checked) {
        struct stat after, named;
        if (fstat(fileno(fp), &after) < 0 || stat(path, &named) < 0 ||
            !grep_stat_unchanged(&json_before, &after) ||
            !grep_stat_unchanged(&json_before, &named)) {
            if (g_grep_query && g_grep_query->native)
                for (size_t i = first_observation; i < g_grep_query->count; i++)
                    g_grep_query->rows[i].error = "changed_during_read";
            g_grep_json_errors++;
            if (grep_json_error_row(w, ctx, json_first, path, "changed_during_read") < 0) {
                fclose(fp); ctx_buf_free(); return 125;
            }
        }
    }
    if (ferror(fp) && json_mode) {
        g_grep_json_errors++;
        if (grep_json_error_row(w, ctx, json_first, path, "read_failed") < 0) {
            fclose(fp); ctx_buf_free(); return 125;
        }
    }
    if (fclose(fp) != 0 && json_mode) {
        g_grep_json_errors++;
        if (grep_json_error_row(w, ctx, json_first, path, "close_failed") < 0) {
            ctx_buf_free(); return 125;
        }
    }
    /* JINV-127: the tail emitters are the second half of quiet. Reaching here
     * under -q means zero matches (a hit returns early above), but -c would
     * still print "0" and -L would still print the filename — so both must be
     * silenced, and -L must not force rc=0: under -q the exit code is purely
     * "did any line match" (GNU: `grep -qL nomatch f` exits 1, not 0). */
    if (flags & 0x100) { ctx_buf_free(); return matches > 0 ? 0 : 1; }
    if (flags & 0x80) {
        char _cl[80]; int k;
        if (flags & 0x10) k = snprintf(_cl, sizeof(_cl), "%s:%d\n", path, matches);
        else              k = snprintf(_cl, sizeof(_cl), "%d\n", matches);
        if (k > 0) outbuf_append(out, _cl, (size_t)k);
    }
    /* -L (files-without-match): reached EOF with zero matches → emit the name.
     * (A file WITH a match returned early above.) */
    if (flags & 0x400000) {
        char _fl[4096]; int k = snprintf(_fl, sizeof _fl, "%s\n", path);
        if (k > 0) outbuf_append(out, _fl, (size_t)k);
        ctx_buf_free();
        return 0;
    }
    ctx_buf_free();
    return matches > 0 ? 0 : 1;
}

/* ── Recursive grep ───────────────────────────────────────────────────────── */

static int grep_recursive(const char* dir, const char* pattern, int flags,
                          jash_writer_t w, void* ctx, grep_outbuf_t* out,
                          int json_mode, int* json_first, unsigned depth) {
    if (json_mode && grep_query_stop()) return 1;
    if (g_grep_query && depth > 64u) { g_grep_query->capped = 1; return 1; }
    DIR* d = opendir(dir);
    if (!d) {
        if (json_mode) {
            g_grep_json_errors++;
            (void)grep_json_error_row(w, ctx, json_first, dir, "directory_open_failed");
        } else jash_writef(jash_stderr_writer, 0, "grep: cannot open directory '%s'\n", dir);
        return 1;
    }

    int total_matches = 0;
    struct dirent* ent;

    for (;;) {
        if (json_mode && grep_query_stop()) break;
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            if (json_mode && errno) {
                g_grep_json_errors++;
                (void)grep_json_error_row(w, ctx, json_first, dir, "directory_read_failed");
            }
            break;
        }
        if (g_grep_query && ++g_grep_query->visits > 100000u) {
            g_grep_query->capped = 1; break;
        }
        if (json_mode ? (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) : ent->d_name[0] == '.') continue;

        char path[4096];
        int path_len = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            if (json_mode) {
                g_grep_json_errors++;
                (void)grep_json_error_row(w, ctx, json_first, dir, "path_exceeds_json_limit");
            }
            continue;
        }

        struct stat st;
        if ((json_mode ? lstat(path, &st) : stat(path, &st)) != 0) {
            if (json_mode) {
                g_grep_json_errors++;
                (void)grep_json_error_row(w, ctx, json_first, path, "stat_failed");
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* --exclude-dir=GLOB: skip whole trees (node_modules, .git, build…)
             * that otherwise flood the agent's context. */
            if (grep_name_excluded(g_grep_excl_dir, g_grep_excl_dir_n, ent->d_name)) continue;
            if (grep_recursive(path, pattern, flags, w, ctx, out, json_mode, json_first, depth + 1u) == 0)
                total_matches++;
        } else if (S_ISREG(st.st_mode)) {
            /* --include=GLOB: only search files matching ANY given glob. */
            if (!grep_name_included(g_grep_incl, g_grep_incl_n, ent->d_name)) continue;
            /* --exclude=GLOB: skip files matching the glob. */
            if (grep_name_excluded(g_grep_excl, g_grep_excl_n, ent->d_name)) continue;
            /* --since SEQ: skip files that predate the resolved cutoff. */
            if (g_grep_since_active && st.st_mtime < g_grep_since_cutoff) continue;
            int result = grep_file(path, pattern,
                                   flags | ((flags & 0x80000) ? 0 : 0x10),
                                   w, ctx, out, json_mode, json_first);
            if (result == 0) total_matches++;
        }
    }

    if (closedir(d) != 0 && json_mode) {
        g_grep_json_errors++;
        (void)grep_json_error_row(w, ctx, json_first, dir, "directory_close_failed");
    }
    return total_matches > 0 ? 0 : 1;
}

#include "grep_semantic.inc"

/* ── Main grep builtin ────────────────────────────────────────────────────── */

static int grep_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    g_grep_json_errors = 0;

    if (argc == 0) {
        jash_writes(jash_stderr_writer, 0, "Usage: grep [-i] [--fuzzy] [--smart] [--context] <pattern> <file> [file2] ...\n");
        return 1;
    }

    /* ADR-0044 gold: pre-scan for --json before the main parse loop (same
     * discipline as builtin_diff.c/builtin_awk.c) — lets the loud-error gates
     * below fire even if --json is combined with a flag that would otherwise
     * short-circuit parsing before reaching it. */
    int json_present = grep_query_mode(argc, argv) != 0;

    /* Parse arguments */
    int flags = 0;
    const char* pattern = NULL;
    int recursive = 0;
    int json = 0;
    unsigned long long since_seq = 0;
    int since_given = 0;
    g_ctx_after = 0;
    g_ctx_before = 0;
    g_grep_since_active = 0;

    /* ADR-0036 W2: argv arrives already tokenized quote-aware by the
     * dispatcher (core/argv.c) — the tokenizer that used to live here
     * (args_copy + manual quote-state walk) is retired; it did the same job
     * per-builtin instead of once, centrally. */

    /* BUG-045: GNU getopt permutes flags and operands — `grep pat file -c`
     * counts just like `grep -c pat file`. The old loop here `break`d on
     * the FIRST non-flag token, so any flag appearing after the pattern
     * (or after a file) was misread as a file operand instead of a flag.
     * Collect every non-flag token into `operands[]` while continuing to
     * scan the rest of argv for flags, instead of stopping at the first one. */
    const char* operands[512];
    int noperands = 0;
    int file_start = 0;  /* unused after the parse loop; kept so the existing
                          * per-flag `file_start = i + 1;` lines need no edits */
    (void)file_start;

    /* ADR-0044 grep parity: pattern-source flags. e_pattern holds a `-e PAT`
     * value (so a pattern beginning with '-' is not misread as a flag); end_opts
     * is set by a bare `--` after which every token is an operand. */
    const char* e_pattern = NULL;
    int end_opts = 0;

    /* Extract flags */
    g_ctx_after = 0;
    g_ctx_before = 0;
    g_grep_max = 0;                 /* reset static state per invocation */
    g_grep_excl_dir_n = 0;
    g_grep_excl_n = 0;
    g_grep_incl_n = 0;
    /* JINV-127: expand a clustered short-flag token ONCE, here, into the
     * canonical token forms the chain below already understands:
     *     -vE      -> "-v" "-E"
     *     -nealpha -> "-n" "-e" "alpha"
     *     -nA1     -> "-n" "-A" "1"
     * The chain therefore stays the SINGLE source of truth for what a flag
     * MEANS. The old expander re-implemented that mapping inline, and the
     * copy silently drifted: -q/-l/-L/-e/-m behaved differently clustered
     * than separated. Rewriting instead of re-mapping makes drift impossible.
     *
     * A value-taking option's argument is copied verbatim and never expanded,
     * so `grep -e -vn f` still searches for the literal "-vn". */
    char* xargv[512];
    {
        static char SHORTTOK[52][3];
        static int shorttok_init = 0;
        if (!shorttok_init) {
            for (int k = 0; k < 26; k++) {
                SHORTTOK[k][0]      = '-'; SHORTTOK[k][1]      = (char)('A' + k); SHORTTOK[k][2]      = '\0';
                SHORTTOK[26 + k][0] = '-'; SHORTTOK[26 + k][1] = (char)('a' + k); SHORTTOK[26 + k][2] = '\0';
            }
            shorttok_init = 1;
        }
        #define GREP_TOKOF(ch) ((ch) >= 'a' ? SHORTTOK[26 + ((ch) - 'a')] : SHORTTOK[(ch) - 'A'])
        /* These two sets MUST equal the single-letter options of the chain
         * below, partitioned by whether they take a value. Gate:
         * tests/test_jinv127_grep_cluster.sh re-derives them from the source
         * and fails if a new "-X" branch is added without landing here. */
        const char* boolf = "cEFGhHilLnoPqrRvwx";  /* take no value */
        const char* argf  = "ABCem";               /* take a value  */
        const int   xcap  = (int)(sizeof(xargv) / sizeof(xargv[0]));
        int xn = 0, ok = 1, skip_next = 0, endo = 0;
        for (int i = 0; i < argc && ok; i++) {
            char* a = argv[i];
            if (xn + 4 >= xcap) { ok = 0; break; }   /* overflow: use argv as-is */
            if (skip_next) { xargv[xn++] = a; skip_next = 0; continue; }
            if (endo)      { xargv[xn++] = a; continue; }
            if (strcmp(a, "--") == 0) { endo = 1; xargv[xn++] = a; continue; }
            if (a[0] != '-' || a[1] == '\0') { xargv[xn++] = a; continue; }
            if (a[1] == '-') {                        /* long option */
                xargv[xn++] = a;
                if (strcmp(a, "--regexp") == 0 || strcmp(a, "--include") == 0 ||
                    strcmp(a, "--exclude") == 0 || strcmp(a, "--exclude-dir") == 0 ||
                    strcmp(a, "--after-context") == 0 || strcmp(a, "--before-context") == 0 ||
                    ((!g_grep_query || grep_query_takes_value(argc, argv, i)) && strcmp(a, "--context") == 0) ||
                    strcmp(a, "--since") == 0 || strcmp(a, "--max-count") == 0)
                    skip_next = 1;
                continue;
            }
            int k = 1, argat = -1;
            for (; a[k]; k++) {
                if (strchr(boolf, a[k])) continue;
                if (strchr(argf,  a[k])) { argat = k; break; }
                argat = -2; break;                    /* unknown letter (e.g. "-x.y") */
            }
            if (argat == -2) { xargv[xn++] = a; continue; }   /* leave for the chain */
            if (xn + k + 3 >= xcap) { ok = 0; break; }
            for (int b = 1; b < (argat < 0 ? k : argat); b++) xargv[xn++] = GREP_TOKOF(a[b]);
            if (argat > 0) {
                xargv[xn++] = GREP_TOKOF(a[argat]);
                if (a[argat + 1]) xargv[xn++] = a + argat + 1;  /* attached value */
                else if (!g_grep_query || a[argat] != 'C' || grep_query_takes_value(argc, argv, i))
                    skip_next = 1;                             /* value is next token */
            }
        }
        #undef GREP_TOKOF
        if (ok) { argv = xargv; argc = xn; }
    }
    for (int i = 0; i < argc; i++) {
        /* After `--`, everything is a pattern/file operand, never a flag. */
        if (end_opts) {
            if (noperands < (int)(sizeof(operands)/sizeof(operands[0])))
                operands[noperands++] = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--") == 0) { end_opts = 1; continue; }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            jash_writes(jash_stderr_writer, 0, "grep (jash)\n");
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--no-filename") == 0) {
            flags |= 0x80000;   /* BUG-156: suppress filename prefix */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-H") == 0 ||
                   strcmp(argv[i], "--with-filename") == 0) {
            flags |= 0x100000;  /* BUG-156: force filename prefix */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-P") == 0 ||
                   strcmp(argv[i], "--perl-regexp") == 0) {
            /* PCRE-lite: \d \D \w \W \s \S \b \A \Z translate to POSIX classes;
             * full PCRE (lookaround/backrefs) is not an engine here. Fixes the
             * most-reported `grep -oP '\d+'` "unknown option". */
            flags |= 0x200000;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-e") == 0 ||
                   strcmp(argv[i], "--regexp") == 0) {
            if (i + 1 < argc) { e_pattern = argv[++i]; file_start = i + 1; }
            else { jash_writef(jash_stderr_writer, 0,
                       "grep: option '-e' requires an argument\n"); return 1; }
            if (g_grep_query) {
                if (g_grep_query->pattern_count == 64u) return 2;
                g_grep_query->pattern_sources[g_grep_query->pattern_count++] = e_pattern;
            }
        } else if (strncmp(argv[i], "--regexp=", 9) == 0) {
            e_pattern = argv[i] + 9;
            file_start = i + 1;
            if (g_grep_query) {
                if (g_grep_query->pattern_count == 64u) return 2;
                g_grep_query->pattern_sources[g_grep_query->pattern_count++] = e_pattern;
            }
        } else if (strcmp(argv[i], "-i") == 0) {
            flags |= 0x01;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-E") == 0 ||
                   strcmp(argv[i], "--extended-regexp") == 0) {
            /* Extended regex (REQ-2): selects REG_EXTENDED in grep_compile. */
            flags |= 0x200;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-G") == 0 ||
                   strcmp(argv[i], "--basic-regexp") == 0) {
            /* Basic regex (default); accept the explicit flag as a no-op. */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fixed-strings") == 0) {
            /* Fixed strings: force literal matching. */
            flags |= 0x400;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--fuzzy") == 0) {
            flags |= 0x02;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--smart") == 0) {
            flags |= 0x04;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--context") == 0) {
            flags |= 0x08;
            if (g_grep_query) {
                g_ctx_before = g_ctx_after = grep_query_takes_value(argc, argv, i) ? atoi(argv[++i]) : 3;
            }
            file_start = i + 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-R") == 0) {
            recursive = 1;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--invert-match") == 0) {
            flags |= 0x20;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--line-number") == 0) {
            flags |= 0x40;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0) {
            /* Count matching lines only (suppress normal output). */
            flags |= 0x80;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0 ||
                   strcmp(argv[i], "--silent") == 0) {
            /* Quiet: no output; exit 0 if any match found, 1 otherwise. */
            flags |= 0x100;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--files-with-matches") == 0) {
            flags |= 0x1000;                 /* -l: print file names with a match */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--files-without-match") == 0 ||
                   strcmp(argv[i], "--files-without-matches") == 0) {
            flags |= 0x400000;               /* -L: print file names with NO match */
            file_start = i + 1;
        } else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--max-count")) && i + 1 < argc) {
            g_grep_max = atoi(argv[++i]);    /* -m NUM: stop after NUM matches/file */
            if (g_grep_max < 0) g_grep_max = 0;
            file_start = i + 1;
        } else if (strncmp(argv[i], "--max-count=", 12) == 0) {
            g_grep_max = atoi(argv[i] + 12);
            if (g_grep_max < 0) g_grep_max = 0;
            file_start = i + 1;
        } else if (strncmp(argv[i], "--exclude-dir=", 14) == 0) {
            if (g_grep_excl_dir_n < 16) strlcpy(g_grep_excl_dir[g_grep_excl_dir_n++], argv[i] + 14, 128);
            file_start = i + 1;
        } else if (strcmp(argv[i], "--exclude-dir") == 0 && i + 1 < argc) {
            if (g_grep_excl_dir_n < 16) strlcpy(g_grep_excl_dir[g_grep_excl_dir_n++], argv[++i], 128);
            file_start = i + 1;
        } else if (strncmp(argv[i], "--exclude=", 10) == 0) {
            if (g_grep_excl_n < 16) strlcpy(g_grep_excl[g_grep_excl_n++], argv[i] + 10, 128);
            file_start = i + 1;
        } else if (strcmp(argv[i], "--exclude") == 0 && i + 1 < argc) {
            if (g_grep_excl_n < 16) strlcpy(g_grep_excl[g_grep_excl_n++], argv[++i], 128);
            file_start = i + 1;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--only-matching") == 0) {
            flags |= 0x2000;                 /* -o: print only the matched part */
            file_start = i + 1;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--word-regexp") == 0) {
            flags |= 0x4000;
            file_start = i + 1;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--line-regexp") == 0) {
            /* BUG-042 conformance gap: -x (whole-line match) was unsupported,
             * silently treated as an unknown file operand. */
            flags |= 0x40000;
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'A' && argv[i][2] >= '0' && argv[i][2] <= '9') {
            flags |= 0x10000;
            g_ctx_after = atoi(argv[i] + 2);
            if (g_ctx_after < 0) g_ctx_after = 0;
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'B' && argv[i][2] >= '0' && argv[i][2] <= '9') {
            flags |= 0x20000;
            g_ctx_before = atoi(argv[i] + 2);
            if (g_ctx_before < 0) g_ctx_before = 0;
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'C' && argv[i][2] >= '0' && argv[i][2] <= '9') {
            flags |= 0x08;
            int n = atoi(argv[i] + 2);
            if (n < 0) n = 0;
            g_ctx_before = g_ctx_after = n;
            file_start = i + 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'm' && 
                   (argv[i][2] == '\0' || (argv[i][2] >= '0' && argv[i][2] <= '9'))) {
            if (argv[i][2] == '\0') {
                if (i + 1 < argc) { g_grep_max = atoi(argv[++i]); file_start = i + 1; }
                else { jash_writef(jash_stderr_writer, 0, "grep: option '-m' requires an argument\n"); return 1; }
            } else {
                g_grep_max = atoi(argv[i] + 2);
                if (g_grep_max < 0) g_grep_max = 0;
                file_start = i + 1;
            }
        } else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--after-context") == 0) {
            flags |= 0x10000;
            if (i + 1 < argc) {
                g_ctx_after = atoi(argv[++i]);
                if (g_ctx_after < 0) g_ctx_after = 0;
                file_start = i + 1;
            } else {
                jash_writef(jash_stderr_writer, 0, "grep: option '%s' requires an argument\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-B") == 0 || strcmp(argv[i], "--before-context") == 0) {
            flags |= 0x20000;
            if (i + 1 < argc) {
                g_ctx_before = atoi(argv[++i]);
                if (g_ctx_before < 0) g_ctx_before = 0;
                file_start = i + 1;
            } else {
                jash_writef(jash_stderr_writer, 0, "grep: option '%s' requires an argument\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--context") == 0) {
            flags |= 0x08;  /* context_mode */
            if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9') {
                /* -C N or --context=N */
                int n = atoi(argv[++i]);
                if (n < 0) n = 0;
                g_ctx_before = g_ctx_after = n;
                file_start = i + 1;
            } else {
                /* -C without number (or --context without =N): default 3 */
                g_ctx_before = g_ctx_after = 3;
                file_start = i + 1;
            }
        } else if (strncmp(argv[i], "--include=", 10) == 0) {
            /* -r file-name filter (e.g. *.c); repeatable — see grep_name_included */
            if (g_grep_incl_n < 16) strlcpy(g_grep_incl[g_grep_incl_n++], argv[i] + 10, 128);
            file_start = i + 1;
        } else if (strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            /* JINV-104: separated form, same as --exclude-dir accepts both. */
            if (g_grep_incl_n < 16) strlcpy(g_grep_incl[g_grep_incl_n++], argv[++i], 128);
            file_start = i + 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
            file_start = i + 1;
        } else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) {
            since_seq = strtoull(argv[++i], NULL, 10);
            since_given = 1;
            file_start = i + 1;
        } else if (strncmp(argv[i], "--since=", 8) == 0) {
            since_seq = strtoull(argv[i] + 8, NULL, 10);
            since_given = 1;
            file_start = i + 1;
        } else if (g_grep_query && (!strncmp(argv[i], "--after-context=", 16) ||
                   !strncmp(argv[i], "--before-context=", 17) || !strncmp(argv[i], "--context=", 10))) {
            const char* value = strchr(argv[i], '=') + 1;
            if (!strncmp(argv[i], "--after", 7)) { flags |= 0x10000; g_ctx_after = atoi(value); }
            else if (!strncmp(argv[i], "--before", 8)) { flags |= 0x20000; g_ctx_before = atoi(value); }
            else { flags |= 0x08; g_ctx_before = g_ctx_after = atoi(value); }
        } else if (g_grep_query && !strcmp(argv[i], "-")) {
            if (noperands < (int)(sizeof(operands) / sizeof(operands[0]))) operands[noperands++] = argv[i];
        } else if (argv[i][0] == '-') {
            jash_writef(jash_stderr_writer, 0, "grep: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            /* BUG-045: collect and keep scanning — a flag appearing after
             * this operand (pattern or file) must still be recognized. */
            if (noperands < (int)(sizeof(operands) / sizeof(operands[0])))
                operands[noperands++] = argv[i];
        }
    }

    /* Pattern source: -e PAT takes precedence and makes EVERY operand a file
     * (file0=0); otherwise operands[0] is the pattern and files start at 1. */
    int file0 = 1;
    if (e_pattern) {
        pattern = e_pattern;
        file0 = 0;
    } else {
        if (noperands == 0) {
            jash_writes(jash_stderr_writer, 0, "grep: no pattern specified\n");
            return 1;
        }
        pattern = operands[0];
    }

    /* REQ-2: compile the matcher once for this invocation (ERE/BRE/literal). */
    int compiled;
#ifdef JASH_HAVE_REGEX
    compiled = g_grep_query ? grep_query_patterns_compile(pattern, flags) : grep_compile(pattern, flags);
#else
    compiled = grep_compile(pattern, flags);
#endif
    if (compiled != 0) {
        jash_writef(jash_stderr_writer, 0, "grep: invalid pattern '%s'\n", pattern);
        /* JINV-106: a broken pattern is rc 2, not rc 1. rc 1 means "no line
         * matched" — a legitimate answer — so returning it here makes a
         * malformed regex indistinguishable from an honest empty result for
         * anything that branches on the status (`grep ... || handle_absent`).
         * Same rc-2 convention the --json guard below already uses. */
        return 2;
    }

/* ADR-0044 gold: --json is a loud rc-2 error (never a silent fallback or
     * malformed partial array) when combined with a mode that has no clean
     * 1:1 {file,line,col,match} record mapping in v1. */
    if (!g_grep_query && json_present && (flags & (0x80 /*-c*/ | 0x1000 /*-l*/ | 0x400000 /*-L*/ |
                                    0x02 /*--fuzzy*/ | 0x04 /*--smart*/ |
                                    0x20 /*-v*/ | 0x100 /*-q*/))) {
        jash_writef(jash_stderr_writer, 0,
            "grep: --json: unsupported with -c/-l/-L/--fuzzy/--smart/-v/-q\n");
        return 2;
    }
    if (!g_grep_query && json && (flags & (0x10000 /*-A*/ | 0x20000 /*-B*/ | 0x08 /*-C*/))) {
        jash_writef(jash_stderr_writer, 0,
            "grep: --json with -A/-B/-C: use --after, --before, --context separately\n");
        return 2;
    }

    /* ADR-0044 gold: --since SEQ requires -r/-R AND a directory operand —
     * without a directory operand, -r alone falls through to the stdin
     * branch today, where --since would otherwise be silently ignored. */
    if (since_given) {
        if (!recursive || noperands <= file0) {
            jash_writef(jash_stderr_writer, 0, "grep: --since requires -r/-R and a directory operand\n");
            return 2;
        }
        if (g_grep_query) {
            int result = grep_query_since(since_seq);
            if (result) return result;
        } else {
        FILE* cf = jash_chain_open();
        if (!cf) {
            jash_writef(jash_stderr_writer, 0, "grep: --since: no chain log available\n");
            return 2;
        }
        jash_chain_entry_t ce;
        int found = 0;
        while (jash_chain_next(cf, &ce)) { if (ce.seq == since_seq) { found = 1; break; } }
        (void)fclose(cf);
        if (!found) {
            char sb[32]; snprintf(sb, sizeof sb, "%llu", since_seq);
            jash_writef(jash_stderr_writer, 0, "grep: --since: no chain entry with seq %s\n", sb);
            return 2;
        }
        g_grep_since_cutoff = (time_t)ce.ts_sec;
        g_grep_since_active = 1;
        }
    }

    if (g_grep_query) {
        int semantic = g_grep_query->semantic_required || noperands <= file0 ||
            (flags & (0x80 | 0x1000 | 0x400000 | 0x02 | 0x04 | 0x20 | 0x100 | 0x10000 | 0x20000 | 0x08));
        for (int i = file0; i < noperands; i++) if (!strcmp(operands[i], "-")) semantic = 1;
        if (semantic) return grep_semantic_run(flags, recursive, noperands - file0, operands + file0);
    }

    if (noperands <= file0) {
        if (json) {
            jash_writef(jash_stderr_writer, 0,
                "grep: --json: requires a file or directory operand (stdin not supported)\n");
            return 2;
        }
        /* No files specified — check if stdin is piped */
        if (!isatty(STDIN_FILENO)) {
            /* 64KB/line cap — matches grep_file's convention. */
            char line[65536];
            int matches = 0;
            int sline = 0;
            int invert = (flags & 0x20) ? 1 : 0;
            int after_pending = 0;
            int group_active = 0;
            int before_n = g_ctx_before > 0 ? g_ctx_before : 0;
            ctx_buf_init(before_n);

            while (fgets(line, sizeof(line), stdin)) {
                int match = 0;
                sline++;
                size_t len = strlen(line);
                if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

                /* Dispatch parity with grep_file: fuzzy -> smart -> word -> default.
                 * Previously only had fuzzy->default, so -w and --smart silently
                 * degraded to substring match when reading from a pipe. */
                if (flags & 0x02) {
                    char line_copy[65536];
                    strlcpy(line_copy, line, sizeof(line_copy));
                    char *word = strtok(line_copy, " \t");
                    while (word) {
                        if (fuzzy_match_word(word, pattern, 2)) { match = 1; break; }
                        word = strtok(NULL, " \t");
                    }
                } else if (flags & 0x04) {
                    match = smart_match(line, pattern);
                } else if (flags & 0x4000) {
                    match = grep_word_match(line, pattern, flags);
                } else {
                    match = grep_match(line, pattern, flags);
                }

                if (invert) match = !match;

                if (match) {
                    matches++;
                    if (flags & 0x100) return 0;
                    if (flags & 0x80) continue;
                    /* Flush before-context buffer. (The dead
                     * `ctx_buf_flush(0, NULL, sline, NULL, 0, NULL)` call that
                     * used to sit here passed a NULL output_len into a function
                     * that dereferenced it unconditionally — SIGSEGV on any
                     * `-B`/`-C` over piped stdin with a preceding non-match
                     * line. The manual loop below was already the real,
                     * correct implementation; the dead call is simply gone.) */
                    if (before_n > 0 && !group_active) {
                        for (int bi = 0; bi < before_n; bi++) {
                            int idx = (g_ctx_buf_pos + bi) % g_ctx_buf_size;
                            if (g_ctx_buf[idx] && g_ctx_buf[idx][0]) {
                                if (flags & 0x40) {
                                    char nb[24]; snprintf(nb, sizeof nb, "%d-", sline - before_n + bi);
                                    jash_writes(w, ctx, nb);
                                }
                                jash_writes(w, ctx, g_ctx_buf[idx]);
                                jash_writes(w, ctx, "\n");
                            }
                        }
                    }
                    group_active = 1;
                    if (flags & 0x2000) {
                        /* -o: print only matched spans */
                        int _ms = 0, _me = 0, _fr = 0;
                        while (grep_span(line, pattern, flags, _fr, &_ms, &_me) && _me > _ms) {
                            _fr = _me;
                            /* BUG-070: -w word-boundary in the -o file path too. */
                            if (flags & 0x4000) {
                                int lok = (_ms == 0) || !grep_is_word(line[_ms - 1]);
                                int rok = (line[_me] == '\0') || !grep_is_word(line[_me]);
                                if (!(lok && rok)) continue;
                            }
                            int _seg = _me - _ms; if (_seg > 4000) _seg = 4000;
                            char _ol[65536]; snprintf(_ol, sizeof(_ol), "%.*s\n", _seg, line + _ms);
                            jash_writes(w, ctx, _ol);
                        }
                    } else {
                        if (flags & 0x40) {
                            char nb[24]; snprintf(nb, sizeof nb, "%d:", sline);
                            jash_writes(w, ctx, nb);
                        }
                        jash_writes(w, ctx, line);
                        jash_writes(w, ctx, "\n");
                    }
                    if (g_ctx_after > 0) after_pending = g_ctx_after;
                    else after_pending = 0;
                    
                    /* -m NUM: stop after NUM matching lines (per input stream) */
                    if (g_grep_max > 0 && matches >= g_grep_max && after_pending == 0) break;
                } else if (after_pending > 0) {
                    /* BUG: this branch used to be preceded by a duplicate,
                     * empty `} else if (after_pending > 0) {` — the empty one
                     * always won, so after-context on stdin was permanently
                     * dead code (never printed, after_pending never
                     * decremented, before-context buffering never resumed).
                     * That duplicate is gone; this is the one real body. */
                    if (flags & 0x40) {
                        char nb[24]; snprintf(nb, sizeof nb, "%d-", sline);
                        jash_writes(w, ctx, nb);
                    }
                    jash_writes(w, ctx, line);
                    jash_writes(w, ctx, "\n");
                    after_pending--;
                    group_active = 1;
                } else {
                    if (before_n > 0) ctx_buf_add(line);
                    group_active = 0;
                }
            }
            if (flags & 0x80) {
                char numbuf[32];
                snprintf(numbuf, sizeof(numbuf), "%d\n", matches);
                jash_writes(w, ctx, numbuf);
            }
            if (flags & 0x100) return 1;
            ctx_buf_free();
            return matches > 0 ? 0 : 1;
        }
        jash_writes(jash_stderr_writer, 0, "grep: no files specified\n");
        return 1;
    }

    /* Process files */
    if (g_grep_query) g_grep_query->started = 1;
    int total_matches = 0;
    int json_first = 1;
    if (json) w(ctx, "[", 1);
    if (g_grep_query && g_grep_query->max_zero) g_grep_query->match_limited = 1;

    for (int i = file0; i < noperands; i++) {
        if (g_grep_query && g_grep_query->max_zero) break;
        if (json && grep_query_stop()) break;
        const char* path = operands[i];

        if (!json) {
            /* Check cache (mtime+size keyed — see grep_cache_find). JSON mode
             * bypasses the cache: it writes straight through `w` and the
             * array wrapper spans every operand, so there's no single
             * "verbatim replay" string to cache per file. */
            grep_cache_entry_t* cached = grep_cache_find(pattern, path, flags);
            if (cached) {
                if (cached->output && cached->output[0]) jash_writes(w, ctx, cached->output);
                if (cached->matched) total_matches++;
                continue;
            }
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            if (json) {
                g_grep_json_errors++;
                if (grep_json_error_row(w, ctx, &json_first, path, "stat_failed") < 0)
                    g_grep_json_errors++;
            } else jash_writef(jash_stderr_writer, 0, "grep: cannot access '%s'\n", path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                grep_outbuf_t ob; outbuf_init(&ob);
                int result = grep_recursive(path, pattern, flags, w, ctx, &ob, json, &json_first, 0);
                if (ob.len > 0) jash_writes(w, ctx, ob.data);
                if (result == 0) total_matches++;
                outbuf_free(&ob);
            } else {
                if (json) {
                    g_grep_json_errors++;
                    (void)grep_json_error_row(w, ctx, &json_first, path,
                                              "directory_requires_recursive");
                } else jash_writef(jash_stderr_writer, 0,
                                   "grep: '%s' is a directory (use -r for recursive)\n", path);
            }
        } else if (S_ISREG(st.st_mode)) {
            int show_filename = (flags & 0x80000) ? 0
                              : (flags & 0x100000) ? 0x10
                              : (noperands - file0 > 1) ? 0x10 : 0;
            grep_outbuf_t ob; outbuf_init(&ob);
            int result = grep_file(path, pattern, flags | show_filename, w, ctx, &ob, json, &json_first);
            if (ob.len > 0) jash_writes(w, ctx, ob.data);
            if (!json) grep_cache_update(pattern, path, flags, result == 0, ob.data ? ob.data : "");
            if (result == 0) total_matches++;
            else if (json && result == 125) g_grep_json_errors++;
            outbuf_free(&ob);
        } else if (json) {
            g_grep_json_errors++;
            (void)grep_json_error_row(w, ctx, &json_first, path, "unsupported_file_type");
        }
    }

    if (json && g_grep_query) {
        (void)grep_query_stop();
        if (!g_grep_query->native && (g_grep_query->cancelled || g_grep_query->capped || g_grep_query->match_limited))
            (void)grep_json_error_row(w, ctx, &json_first, "",
                g_grep_query->cancelled ? "request_cancelled" :
                g_grep_query->capped ? "query_collection_limit" : "match_limit");
    }
    if (json) w(ctx, "]\n", 2);

    if (g_grep_query && g_grep_query->cancelled) return g_grep_query->cancelled;
    if (g_grep_query && g_grep_query->capped) return 75;

    if (json && g_grep_json_errors) return 2;
    return total_matches > 0 ? 0 : 1;
}

int jash_b_grep(jash_state_t* state, jash_writer_t writer, void* context,
                  int argc, char** argv) {
    int mode = grep_query_mode(argc, argv);
    if (!mode) return grep_run(state, writer, context, argc, argv);
    grep_query_t query = {0};
    query.state = state; query.native = mode == 2; query.limit = UINT64_MAX;
    const char* argument_error = NULL;
    char** arguments = argc > 0 && argc <= 256 ? calloc((size_t)argc + 1u, sizeof(*arguments)) : NULL;
    int count = 0, end_options = 0, explicit_json = 0;
    size_t bytes = 0, expanded = 0;
    unsigned includes = 0, excludes = 0, excluded_directories = 0;
    if (!arguments) argument_error = argc > 256 ? "argument_limit" : "allocation_failed";
    for (int i = 0; i < argc && !argument_error; i++) {
        char* argument = argv[i];
        size_t length = strlen(argument);
        if (length > 65536u - bytes) { argument_error = "argument_limit"; break; }
        bytes += length;
        if (!end_options && !strcmp(argument, "--")) end_options = 1;
        if (!end_options && (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2"))) {
            arguments[count++] = "--json"; expanded++; continue;
        }
        if (!end_options && !strcmp(argument, "--json")) explicit_json = 1;
        const char* numeric = NULL;
        int max_count = 0, context_count = 0;
        if (!end_options && (!strcmp(argument, "--max-count") || !strcmp(argument, "--since"))) {
            numeric = i + 1 < argc ? argv[i + 1] : "";
            max_count = !strcmp(argument, "--max-count");
        } else if (!end_options && !strncmp(argument, "--max-count=", 12)) {
            numeric = argument + 12; max_count = 1;
        } else if (!end_options && !strncmp(argument, "--since=", 8)) numeric = argument + 8;
        else if (!end_options && (!strcmp(argument, "--after-context") || !strcmp(argument, "--before-context") ||
                 (!strcmp(argument, "--context") && grep_query_takes_value(argc, argv, i)))) {
            numeric = i + 1 < argc ? argv[i + 1] : ""; context_count = 1;
        } else if (!end_options && (!strncmp(argument, "--after-context=", 16) ||
                   !strncmp(argument, "--before-context=", 17) || !strncmp(argument, "--context=", 10))) {
            numeric = strchr(argument, '=') + 1; context_count = 1;
        }
        else if (!end_options && argument[0] == '-' && argument[1] && argument[1] != '-') {
            for (size_t letter = 1; argument[letter]; letter++) {
                if (!strchr("ABCem", argument[letter])) continue;
                if (argument[letter] == 'm') {
                    numeric = argument[letter + 1] ? argument + letter + 1 : i + 1 < argc ? argv[i + 1] : "";
                    max_count = 1;
                } else if (strchr("ABC", argument[letter]) &&
                           (argument[letter + 1] || argument[letter] != 'C' || grep_query_takes_value(argc, argv, i))) {
                    numeric = argument[letter + 1] ? argument + letter + 1 : i + 1 < argc ? argv[i + 1] : "";
                    context_count = 1;
                }
                break;
            }
        }
        if (numeric) {
            uint64_t value;
            if (grep_query_number(numeric, &value) || (max_count && value > INT_MAX) ||
                (context_count && value > GS_CONTEXT_MAX)) {
                argument_error = "invalid_numeric_option"; break;
            }
            if (max_count) query.max_zero = !value;
        }
        if (!end_options && (!strcmp(argument, "--offset") || !strcmp(argument, "--limit"))) {
            int limit = !strcmp(argument, "--limit");
            uint64_t value;
            if (i + 1 >= argc || grep_query_number(argv[++i], &value) < 0 || (limit && !value)) {
                argument_error = "invalid_page"; break;
            }
            if (limit) query.limit = value; else query.offset = value;
            if (!query.native) query.semantic_required = 1;
            continue;
        }
        if (!end_options && argument[0] == '-' && argument[1] && argument[1] != '-') expanded += length;
        else expanded++;
        if (expanded > 480u) { argument_error = "argument_limit"; break; }
        const char* filter = NULL;
        unsigned* number = NULL;
        if (!end_options && (!strncmp(argument, "--include=", 10) || !strcmp(argument, "--include"))) {
            filter = argument[9] == '=' ? argument + 10 : i + 1 < argc ? argv[i + 1] : NULL;
            number = &includes;
        } else if (!end_options && (!strncmp(argument, "--exclude=", 10) || !strcmp(argument, "--exclude"))) {
            filter = argument[9] == '=' ? argument + 10 : i + 1 < argc ? argv[i + 1] : NULL;
            number = &excludes;
        } else if (!end_options && (!strncmp(argument, "--exclude-dir=", 14) || !strcmp(argument, "--exclude-dir"))) {
            filter = argument[13] == '=' ? argument + 14 : i + 1 < argc ? argv[i + 1] : NULL;
            number = &excluded_directories;
        }
        if (number && (!filter || strlen(filter) >= 128u || ++*number > 16u)) {
            argument_error = "filter_limit"; break;
        }
        arguments[count++] = argument;
        if (!end_options && grep_query_takes_value(argc, argv, i)) {
            if (i + 1 >= argc) { argument_error = "missing_option_value"; break; }
            length = strlen(argv[i + 1]);
            if (length > 65536u - bytes) { argument_error = "argument_limit"; break; }
            bytes += length; arguments[count++] = argv[++i]; expanded++;
        }
    }
    if (query.native && explicit_json) argument_error = "conflicting_output_modes";
    if (argument_error) {
        free(arguments);
        return query.native ? query_data_error(writer, context, &GREP_QUERY_SCHEMA, argument_error, 2) :
            jash_structured_error(writer, context, "grep", argument_error, 2);
    }
    jash_structured_output_t output;
    if (!query.native && jash_structured_begin(&output, writer, context) < 0) { free(arguments); return 125; }
    g_grep_query = &query;
    int result = grep_run(state, query.native ? grep_query_discard : grep_query_checked,
        query.native ? NULL : &output, count, arguments);
    (void)grep_query_stop();
    if (g_grep_semantic) {
        if (!query.native && jash_structured_finish(&output, 0)) result = 125;
        if (result != 125) result = grep_semantic_emit(g_grep_semantic, writer, context, result);
        grep_semantic_free();
    } else if (query.native) result = grep_query_emit(&query, writer, context, result, (uint64_t)g_grep_json_errors);
    else {
        result = jash_structured_finish(&output, result);
        if (!query.started && result != 125)
            result = jash_structured_error(writer, context, "grep",
                query.cancelled ? "request_cancelled" : query.preflight_error ? query.preflight_error : "invalid_arguments",
                query.cancelled ? query.cancelled : 2);
    }
    g_grep_query = NULL;
#ifdef JASH_HAVE_REGEX
    grep_query_patterns_free(&query);
#endif
    for (size_t i = 0; i < query.count; i++) { free(query.rows[i].path); free(query.rows[i].bytes); }
    free(query.rows); free(arguments);
    return result;
}
