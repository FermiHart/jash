// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: let — evaluate arithmetic expressions
//
// Delegates to the REAL $((...)) evaluator (parser/arithmetic.c) instead of
// the old digit-only toy parser, so `let x=x+5` resolves bare variable names,
// parentheses, **, bitwise ops — exactly like bash. Multiple expressions are
// evaluated left to right; exit status is 1 if the LAST expression evaluates
// to 0, else 0 (bash semantics).
// F E R M I ∞ H A R T

#include "builtin.h"
#include <core/env.h>
#include <parser/arithmetic.h>
#include <string.h>

int jash_b_let(jash_state_t *s, jash_writer_t w, void *ctx,
               const char *args) {
    (void)w; (void)ctx;
    if (!args || !*args) return 1;

    char buf[512];
    int bi = 0;
    while (*args && bi < 510) buf[bi++] = *args++;
    buf[bi] = 0;

    long last_result = 0;
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* One expression token: up to unquoted whitespace. Quotes let the
         * user write let "x = x + 5" as a single expression. */
        char expr[256];
        unsigned ei = 0;
        char quote = 0;
        while (*p && ei + 1 < sizeof(expr)) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; continue; }
                expr[ei++] = *p++;
                continue;
            }
            if (*p == '"' || *p == '\'') { quote = *p++; continue; }
            if (*p == ' ' || *p == '\t') break;
            expr[ei++] = *p++;
        }
        expr[ei] = 0;
        if (!ei) break;

        /* let VAR=expr  → assign; bare `let expr` → just evaluate.
         * Find the first '=' that is an assignment (not ==, <=, >=, !=). */
        char *eq = NULL;
        for (char *q = expr; *q; q++) {
            if (*q == '=' && q > expr && q[1] != '=' &&
                q[-1] != '=' && q[-1] != '<' && q[-1] != '>' && q[-1] != '!') {
                /* += -= *= /= %= are compound assignments */
                eq = q;
                break;
            }
        }

        if (eq) {
            char compound = 0;
            char *name_end = eq;
            if (name_end > expr) {
                char prev = name_end[-1];
                if (prev == '+' || prev == '-' || prev == '*' ||
                    prev == '/' || prev == '%') {
                    compound = prev;
                    name_end--;
                }
            }
            /* BUG-127: `let "x = 5 + 3"` — trim whitespace around the name
             * (the quoted form has spaces around the '='). */
            char varname[64];
            const char* ns = expr;
            while (ns < name_end && (*ns == ' ' || *ns == '\t')) ns++;
            const char* ne = name_end;
            while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            int ni = (int)(ne - ns);
            if (ni > 62) ni = 62;
            for (int i = 0; i < ni; i++) varname[i] = ns[i];
            varname[ni] = 0;

            long rhs = jash_eval_arithmetic(eq + 1, s);
            long result = rhs;
            if (compound) {
                const char *cur = jash_env_get(s, varname);
                long curv = 0;
                if (cur) {
                    const char *c = cur;
                    int neg = 0;
                    if (*c == '-') { neg = 1; c++; }
                    while (*c >= '0' && *c <= '9') { curv = curv * 10 + (*c - '0'); c++; }
                    if (neg) curv = -curv;
                }
                switch (compound) {
                case '+': result = curv + rhs; break;
                case '-': result = curv - rhs; break;
                case '*': result = curv * rhs; break;
                case '/': result = rhs != 0 ? curv / rhs : 0; break;
                case '%': result = rhs != 0 ? curv % rhs : 0; break;
                }
            }

            char val[24];
            int vi = 0;
            if (result == 0) { val[vi++] = '0'; }
            else {
                long tmp = result < 0 ? -result : result;
                char rev[24]; int ri = 0;
                while (tmp > 0) { rev[ri++] = (char)('0' + (tmp % 10)); tmp /= 10; }
                if (result < 0) val[vi++] = '-';
                while (ri > 0) val[vi++] = rev[--ri];
            }
            val[vi] = 0;
            jash_env_set(s, varname, val);
            last_result = result;
        } else {
            last_result = jash_eval_arithmetic(expr, s);
        }
    }
    return last_result == 0 ? 1 : 0;
}
