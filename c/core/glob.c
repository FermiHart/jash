// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/glob — wildcard matcher (impl)
//
// Ported from user/jash-legacy/main.c glob_match. Adds `**` recursive
// support per manifesto §2 (no shopt opt-in required).
//
// Implementation: tail-recursive backtracking. Worst case is O(n*m)
// for `*` and O(n*m) for `**` where the inner `**` traverses every
// position once. For shell-line-length inputs this is irrelevant.

#include "glob.h"
#include <string.h>
#include <stdlib.h>

int jash_glob_has_meta(const char* pat) {
    if (!pat) return 0;
    for (const char* p = pat; *p; p++) {
        if (*p == '*' || *p == '?' || *p == '[') return 1;
    }
    return 0;
}

/* Match one char against a [...] class starting at pat (which points
 * AT the '['). On match, returns 1 and advances *pat_out past the ']'.
 * On no-match returns 0 (pat_out still advanced past ']'). If the class
 * is malformed (no closing ']'), returns -1. Supports ranges a-z and
 * negation [!...] / [^...]. */
static int match_class(const char* pat, char c, const char** pat_out) {
    const char* p = pat + 1; /* skip '[' */
    int negate = 0;
    if (*p == '!' || *p == '^') { negate = 1; p++; }
    int matched = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        if (p[0] && p[1] == '-' && p[2] && p[2] != ']') {
            /* range a-z */
            char lo = p[0], hi = p[2];
            if (c >= lo && c <= hi) matched = 1;
            p += 3;
        } else {
            if (c == *p) matched = 1;
            p++;
        }
    }
    if (*p != ']') return -1; /* malformed */
    *pat_out = p + 1;
    return negate ? !matched : matched;
}

/* SECURITY (audit rank 8, glob ReDoS / stack-overflow): the matcher recurses
 * with backtracking, so a pattern like `a*a*a*...*b` against a long
 * non-matching name explodes exponentially in BOTH time (CPU DoS / hang) and
 * recursion depth (stack overflow). A global step budget bounds the total work
 * of a single top-level match; exceeding it returns "no match" (the safe
 * default) rather than hanging or smashing the stack. The cap is far above any
 * realistic pattern. F E R M I ∞ H A R T */
#define JASH_GLOB_MAX_STEPS 4000000L

/* PERF (audit F6): the budget is threaded by pointer, not kept in a global,
 * so a caller doing a whole line expansion can share ONE aggregate counter
 * across the per-dirent calls (see jash_glob_match_ctx). `call` bounds a
 * single top-level match (the old per-call cap); `line`, when non-NULL,
 * bounds the whole expansion. */
typedef struct {
    long  call;
    long* line;
} glob_budget_t;

static int glob_match_rec(const char* pat, const char* name, glob_budget_t* b) {
    if (!pat || !name) return 0;
    if (--b->call < 0) return 0;                    /* per-call DoS budget exhausted */
    if (b->line && --(*b->line) < 0) return 0;      /* shared expansion budget exhausted */

    while (*pat) {
        /* `**` — recursive: matches anything including '/'. */
        if (pat[0] == '*' && pat[1] == '*') {
            /* Skip the entire `**` (and any literal-glob noise after). */
            pat += 2;
            while (*pat == '*') pat++;          /* `***` collapses to `**` */
            if (!*pat) return 1;                /* trailing ** matches all */

            /* Special case "ds-slash-rest": ds-slash-x should match
             * both "a/b/x" AND "x" (no leading directory). We try
             * the skip-the-separator variant first.
             * ("ds" = double-star, written that way to dodge the
             *  C comment terminator we'd hit if we wrote it out.) */
            if (*pat == '/' && glob_match_rec(pat + 1, name, b)) return 1;

            /* Try every suffix of name as the continuation point. */
            while (*name) {
                if (glob_match_rec(pat, name, b)) return 1;
                name++;
            }
            return glob_match_rec(pat, name, b);  /* try empty tail */
        }

        /* Single `*` — matches anything within one name segment
         * (does NOT cross '/'). Skip if followed by '(' — that's extglob *(pat). */
        if (*pat == '*' && pat[1] != '(') {
            pat++;
            if (!*pat) {
                /* Trailing `*`: match rest, but stop at '/'. */
                while (*name && *name != '/') name++;
                return *name == '\0';
            }
            while (*name && *name != '/') {
                if (glob_match_rec(pat, name, b)) return 1;
                name++;
            }
            return glob_match_rec(pat, name, b);
        }

        /* `?` — exactly one char, but never '/'.
         * Skip if followed by '(' — that's extglob ?(pat). */
        if (*pat == '?' && pat[1] != '(') {
            if (!*name || *name == '/') return 0;
            pat++; name++;
            continue;
        }

        /* `[...]` — character class. */
        if (*pat == '[') {
            if (!*name || *name == '/') return 0;
            const char* after = 0;
            int r = match_class(pat, *name, &after);
            if (r < 0) {
                /* malformed class — treat '[' as a literal */
                if (*pat != *name) return 0;
                pat++; name++;
                continue;
            }
            if (!r) return 0;
            pat = after;
            name++;
            continue;
        }


        /* EXTGLOB: @(a|b|c) — match exactly one alternative */
        if (pat[0] == '@' && pat[1] == '(') {
            const char* alt_start = pat + 2;
            const char* p = alt_start;
            int depth = 1;
            /* Find closing ) */
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            if (depth == 0) {
                const char* alt_end = p;  /* points to closing ) */
                const char* rest = p + 1;
                /* Try each alternative */
                const char* a = alt_start;
                while (a < alt_end) {
                    const char* ae = a;
                    int d = 0;
                    while (ae < alt_end && (d > 0 || *ae != '|')) {
                        if (*ae == '(') d++;
                        else if (*ae == ')') d--;
                        if (d > 0 || *ae != '|') ae++;
                    }
                    /* Try alternative [a, ae) */
                    char alt[1024];
                    unsigned al = 0;
                    for (const char* k = a; k < ae && al + 1 < sizeof(alt); k++)
                        alt[al++] = *k;
                    alt[al] = 0;
                    /* Try matching alt + rest against name */
                    char combined[2048];
                    unsigned ci = 0;
                    for (unsigned i = 0; i < al && ci + 1 < sizeof(combined); i++)
                        combined[ci++] = alt[i];
                    for (const char* k = rest; *k && ci + 1 < sizeof(combined); k++)
                        combined[ci++] = *k;
                    combined[ci] = 0;
                    if (glob_match_rec(combined, name, b)) return 1;
                    /* Advance to next alternative */
                    a = ae + 1;
                }
                return 0;
            }
        }

        /* EXTGLOB: *(pat) — match zero or more occurrences */
        if (pat[0] == '*' && pat[1] == '(') {
            const char* p = pat + 2;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            if (depth == 0) {
                char inner[1024];
                unsigned il = 0;
                for (const char* k = pat + 2; k < p && il + 1 < sizeof(inner); k++)
                    inner[il++] = *k;
                inner[il] = 0;
                const char* rest = p + 1;
                /* Try zero occurrences */
                if (glob_match_rec(rest, name, b)) return 1;
                /* rest is used above, silence warning */
                (void)rest;
                /* Try one or more: match inner once then recurse with *(inner)rest */
                const char* try_name = name;
                while (*try_name) {
                    /* Try matching inner at current position */
                    if (glob_match_rec(inner, try_name, b)) {
                        /* Matched one occurrence; try more */
                        if (glob_match_rec(pat, try_name + strlen(inner), b)) return 1;
                    }
                    try_name++;
                }
                return 0;
            }
        }

        /* EXTGLOB: +(pat) — match one or more occurrences */
        if (pat[0] == '+' && pat[1] == '(') {
            const char* p = pat + 2;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            if (depth == 0) {
                char inner[1024];
                unsigned il = 0;
                for (const char* k = pat + 2; k < p && il + 1 < sizeof(inner); k++)
                    inner[il++] = *k;
                inner[il] = 0;
                const char* rest = p + 1;
                /* Must match at least once */
                (void)rest;  /* used conceptually */
                /* Convert +(pat)rest to *(pat)rest for recursion after first match */
                char star_pat[2048];
                star_pat[0] = '*';
                unsigned si = 1;
                for (const char* k = pat + 1; *k && si + 1 < sizeof(star_pat); k++)
                    star_pat[si++] = *k;
                star_pat[si] = 0;
                /* Try: match inner once, then *(pat)rest */
                const char* try_name = name;
                while (*try_name) {
                    if (glob_match_rec(inner, try_name, b)) {
                        if (glob_match_rec(star_pat, try_name + strlen(inner), b)) return 1;
                    }
                    try_name++;
                }
                return 0;
            }
        }

        /* EXTGLOB: ?(pat) — match zero or one occurrence */
        if (pat[0] == '?' && pat[1] == '(') {
            const char* p = pat + 2;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            if (depth == 0) {
                char inner[1024];
                unsigned il = 0;
                for (const char* k = pat + 2; k < p && il + 1 < sizeof(inner); k++)
                    inner[il++] = *k;
                inner[il] = 0;
                const char* rest = p + 1;
                /* Zero occurrences */
                if (glob_match_rec(rest, name, b)) return 1;
                /* One occurrence */
                char combined[2048];
                unsigned ci = 0;
                for (unsigned i = 0; i < il && ci + 1 < sizeof(combined); i++)
                    combined[ci++] = inner[i];
                for (const char* k = rest; *k && ci + 1 < sizeof(combined); k++)
                    combined[ci++] = *k;
                combined[ci] = 0;
                return glob_match_rec(combined, name, b);
            }
        }

        /* EXTGLOB: !(pat) — match anything EXCEPT pat */
        if (pat[0] == '!' && pat[1] == '(') {
            const char* p = pat + 2;
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth > 0) p++;
            }
            if (depth == 0) {
                char inner[1024];
                unsigned il = 0;
                for (const char* k = pat + 2; k < p && il + 1 < sizeof(inner); k++)
                    inner[il++] = *k;
                inner[il] = 0;
                const char* rest = p + 1;
                /* Try every prefix of name against inner; if none match, continue with rest.
                 * PERF (audit F6): the prefix is produced by NUL-punching ONE private
                 * copy of name (restored right after each probe) instead of rebuilding
                 * it byte-by-byte per length — the old loop was O(n^2) per attempt. */
                size_t nlen = strlen(name);
                char stackbuf[1024];
                char* nbuf = stackbuf;
                if (nlen + 1 > sizeof(stackbuf)) {
                    nbuf = (char*)malloc(nlen + 1);
                    if (!nbuf) return 0;   /* safe default: no match */
                }
                memcpy(nbuf, name, nlen + 1);
                int ret = 0;
                for (size_t len = 0; len < nlen; len++) {
                    char saved = nbuf[len];
                    nbuf[len] = 0;                          /* prefix [0, len) */
                    int pm = glob_match_rec(inner, nbuf, b);
                    nbuf[len] = saved;
                    if (pm) continue;    /* prefix matches inner - skip */
                    /* This prefix doesn't match inner; try rest against remaining name */
                    if (glob_match_rec(rest, name + len, b)) { ret = 1; break; }
                }
                /* Try entire name against inner */
                if (!ret && !glob_match_rec(inner, nbuf, b))
                    ret = glob_match_rec(rest, "", b);
                if (nbuf != stackbuf) free(nbuf);
                return ret;
            }
        }

        /* Literal byte. */
        if (*pat != *name) return 0;
        pat++; name++;
    }

    return *name == '\0';
}

/* Public entries: run the bounded matcher with a fresh per-call budget;
 * the _ctx variant additionally shares one aggregate budget across a whole
 * line expansion (audit F6). */
int jash_glob_match_ctx(const char* pat, const char* name, long* line_steps) {
    glob_budget_t b = { JASH_GLOB_MAX_STEPS, line_steps };
    return glob_match_rec(pat, name, &b);
}

int jash_glob_match(const char* pat, const char* name) {
    return jash_glob_match_ctx(pat, name, NULL);
}
