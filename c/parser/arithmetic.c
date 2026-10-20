// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Parser: arithmetic expansion $((expr))
// Supports full operator set with bash-compatible precedence:
//   Power: **
//   Unary: ++, --, !, ~, +, - (prefix)
//   Multiplicative: *, /, %
//   Additive: +, -
//   Bitwise shift: <<, >>
//   Bitwise AND: &
//   Bitwise XOR: ^
//   Bitwise OR: |

#include "arithmetic.h"
#include "cmdsub.h"        /* JINV-087: JASH_EXP_MARK */
#include "param_expand.h"  /* JINV-087: jash_ifs_word_mark_wanted */
#include <core/state.h>
#include <core/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>

/* ── Checked-arithmetic layer (SECURITY: audit class "integer/UB arithmetic
 * with zero-only guards") ────────────────────────────────────────────────────
 * The $((...)) evaluator operated on raw `long` with C signed-overflow,
 * shift-out-of-range, and INT_MIN negation all reachable from a single command
 * line — every one is UndefinedBehavior (UBSan-confirmed) and at least one
 * (LONG_MIN/-1) is a SIGFPE crash. These helpers give every dangerous operation
 * defined, saturating/guarded semantics. Saturation (not trap) keeps the shell
 * alive on hostile input the way bash clamps rather than aborts.
 * F E R M I ∞ H A R T */

/* Multiply with saturation to LONG_MIN/LONG_MAX (no signed-overflow UB). */
static long arith_mul_sat(long a, long b) {
    if (a == 0 || b == 0) return 0;
    /* overflow iff a > MAX/b (sign-adjusted); do it in checks, not products */
    if (a > 0) {
        if (b > 0) { if (a > LONG_MAX / b) return LONG_MAX; }
        else       { if (b < LONG_MIN / a) return LONG_MIN; }
    } else {
        if (b > 0) { if (a < LONG_MIN / b) return LONG_MIN; }
        else       { if (a != 0 && b < LONG_MAX / a) return LONG_MAX; }
    }
    return a * b;
}

/* Add with saturation to LONG_MIN/LONG_MAX (no signed-overflow UB). */
static long arith_add_sat(long a, long b) {
    if (b > 0 && a > LONG_MAX - b) return LONG_MAX;
    if (b < 0 && a < LONG_MIN - b) return LONG_MIN;
    return a + b;
}

/* Subtract with saturation. Guard b == LONG_MIN separately (negation of it is
 * itself UB, so it can never be folded into the add path). */
static long arith_sub_sat(long a, long b) {
    if (b == LONG_MIN) return (a >= 0) ? LONG_MAX : LONG_MIN;
    if (b > 0 && a < LONG_MIN + b) return LONG_MIN;
    if (b < 0 && a > LONG_MAX + b) return LONG_MAX;
    return a - b;
}

/* Accumulate a decimal digit into a literal with saturation (no overflow UB). */
static long arith_acc_digit(long acc, int digit) {
    long t = arith_mul_sat(acc, 10);
    if (t > LONG_MAX - digit) return LONG_MAX;   /* saturate */
    return t + digit;
}

/* BUG-040 class-kill: accumulate a digit in ANY base (hex/octal/base#) with
 * saturation. The literal paths used raw `(num<<4)|d` / `num*base+d` on a signed
 * long — signed-overflow UB. Route them ALL through this, like the decimal path. */
static long arith_acc_base(long acc, int base, int digit) {
    return arith_add_sat(arith_mul_sat(acc, base), digit);
}

/* Division-by-zero error flag (bash parity). Set when a `/0` or `%0` is hit;
 * the caller (eval.c) emits "division by 0" to stderr, sets rc=1, and aborts the
 * command — instead of the old saturating return-0. Cleared by the caller before
 * an expansion pass; never auto-reset here so it survives a whole line's $(())s. */
static int g_arith_div0 = 0;
void jash_arith_clear_error(void) { g_arith_div0 = 0; }
int  jash_arith_error(void)       { return g_arith_div0; }

/* Division/modulo: /0 and %0 flag an error (still return a defined 0 so the
 * evaluator never traps); LONG_MIN/-1 is guarded (SIGFPE on x86, not just UB). */
static long arith_div(long a, long b) {
    if (b == 0) { g_arith_div0 = 1; return 0; }   /* bash: error, rc 1 */
    if (a == LONG_MIN && b == -1) return LONG_MIN;/* avoid SIGFPE/overflow */
    return a / b;
}
static long arith_mod(long a, long b) {
    if (b == 0) { g_arith_div0 = 1; return 0; }
    if (a == LONG_MIN && b == -1) return 0;       /* mathematically 0 */
    return a % b;
}

/* Shifts with the count clamped to [0,63] (a shift >= width or negative is UB). */
static long arith_shl(long a, long n) {
    if (n < 0) return 0;
    if (n >= (long)(sizeof(long) * 8)) return 0;
    return (long)((unsigned long)a << n);         /* shift in unsigned: defined */
}
static long arith_shr(long a, long n) {
    if (n < 0) return 0;
    if (n >= (long)(sizeof(long) * 8)) return a < 0 ? -1 : 0;
    return a >> n;                                /* impl-defined arithmetic shr, ok */
}

/* result ** exp, saturating, with a hard iteration cap and negative-exp = 0
 * (bash: integer power of |exp|; negative exponent of non-1 base is 0). */
#define ARITH_POW_CAP 100000L
static long arith_pow(long base, long exp) {
    if (exp < 0) return (base == 1) ? 1 : (base == -1 ? (exp & 1 ? -1 : 1) : 0);
    if (exp > ARITH_POW_CAP) exp = ARITH_POW_CAP; /* DoS cap: no uninterruptible loop */
    long r = 1;
    for (long i = 0; i < exp; i++) {
        long nr = arith_mul_sat(r, base);
        if (nr == LONG_MAX || nr == LONG_MIN) return nr; /* saturated: stop early */
        r = nr;
    }
    return r;
}

typedef struct {
    const char* expr;
    unsigned pos;
    void* state;
} arith_parser;

/* Forward declarations */
static long arith_parse_expr(arith_parser* p);
static long arith_parse_comma(arith_parser* p);
static long arith_parse_comparison(arith_parser* p);
static long arith_parse_assignment(arith_parser* p);
static long arith_parse_ternary(arith_parser* p);

static void skip_whitespace(arith_parser* p) {
    if (!p || !p->expr) return;
    while (p->expr[p->pos] && isspace(p->expr[p->pos])) {
        p->pos++;
    }
}

/* --- lvalue helpers for ++ / -- (read/write a bare shell variable) --- */
/* BUG-103: normalize "base[expr]" to the canonical stored key "base[N]" by
 * evaluating the subscript (array elements are stored under literal keys like
 * "a[0]"); names without '[' pass through unchanged. Writes through the raw
 * name (`++a[i]`) hit a junk env key otherwise. */
static void arith_norm_key(jash_state_t* s, const char* name,
                           char* key, size_t cap) {
    const char* lb = strchr(name, '[');
    const char* rb = lb ? strrchr(name, ']') : NULL;
    if (s && lb && lb != name && rb && rb > lb + 1) {
        char base[64], subx[128];
        size_t bl = (size_t)(lb - name), sl = (size_t)(rb - lb - 1);
        if (bl < sizeof(base) && sl < sizeof(subx)) {
            memcpy(base, name, bl); base[bl] = 0;
            memcpy(subx, lb + 1, sl); subx[sl] = 0;
            long ix = jash_eval_arithmetic(subx, s);
            if (ix < 0) ix = 0;
            snprintf(key, cap, "%s[%ld]", base, ix);
            return;
        }
    }
    snprintf(key, cap, "%s", name);
}
static long arith_var_read(jash_state_t* s, const char* name) {
    if (!s) return 0;
    char key[128];
    arith_norm_key(s, name, key, sizeof(key));
    const char* v = jash_env_get(s, key);
    return (v && *v) ? strtol(v, NULL, 0) : 0;
}
static void arith_var_write(jash_state_t* s, const char* name, long val) {
    if (!s) return;
    char key[128];
    arith_norm_key(s, name, key, sizeof(key));
    char b[32]; snprintf(b, sizeof(b), "%ld", val);
    jash_env_set(s, key, b);
}
static int arith_read_name(arith_parser* p, char* name, unsigned cap) {
    if (!(isalpha((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_')) return 0;
    unsigned n = 0;
    while (p->expr[p->pos] && n + 1 < cap &&
           (isalnum((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_'))
        name[n++] = p->expr[p->pos++];
    name[n] = 0;
    /* BUG-079: array element a[expr] — consume the subscript so the whole
     * `a[1]` is the name; arith_lookup_var resolves the element. */
    if (p->expr[p->pos] == '[') {
        int depth = 0;
        while (p->expr[p->pos] && n + 1 < cap) {
            char c = p->expr[p->pos++];
            name[n++] = c;
            if (c == '[') depth++;
            else if (c == ']') { depth--; if (depth == 0) break; }
        }
        name[n] = 0;
    }
    return 1;
}

/* Resolve a variable name to a long, handling special computed vars ($?, $#, $$)
 * that are not stored in the env table but computed from shell state. */
static long arith_lookup_var(jash_state_t* s, const char* name) {
    if (!s || !name || !name[0]) return 0;
    /* BUG-079: array element base[subscript] — resolve the subscript (which
     * may itself be arithmetic, e.g. a[i] or a[1+1]) and read the element key
     * base[N] from the env. */
    {
        const char* lb = strchr(name, '[');
        if (lb && lb != name) {
            const char* rb = strrchr(name, ']');
            if (rb && rb > lb + 1) {
                char base[64], subx[128];
                size_t bl = (size_t)(lb - name), sl = (size_t)(rb - lb - 1);
                if (bl < sizeof(base) && sl < sizeof(subx)) {
                    memcpy(base, name, bl); base[bl] = 0;
                    memcpy(subx, lb + 1, sl); subx[sl] = 0;
                    long ix = jash_eval_arithmetic(subx, s);
                    if (ix < 0) ix = 0;
                    char key[96];
                    snprintf(key, sizeof(key), "%s[%ld]", base, ix);
                    const char* av = jash_env_get(s, key);
                    return (av && *av) ? strtol(av, NULL, 0) : 0;
                }
            }
        }
    }
    if (name[0] == '?' && !name[1]) return s->last_status.code;
    if (name[0] == '$' && !name[1]) return s->pid;
    if (name[0] == '#' && !name[1]) {
        int cnt = 0;
        for (int i = 1; i <= JASH_POS_MAX; i++) {
            char key[8]; int k = 0;
            if (i >= 100) key[k++] = '0' + i / 100;
            if (i >= 10)  key[k++] = '0' + (i / 10) % 10;
            key[k++] = '0' + i % 10; key[k] = 0;
            if (!jash_env_get(s, key)) break;
            cnt++;
        }
        return cnt;
    }
    /* $RANDOM and $SECONDS work inside $(( )) too */
    if (strcmp(name, "RANDOM") == 0)
        return (long)(rand() & 0x7fff);
    if (strcmp(name, "SECONDS") == 0) {
        static time_t base = 0;
        time_t now = time(NULL);
        if (!base) base = now;
        return (long)(now - base);
    }
    const char* v = jash_env_get(s, name);
    return (v && *v) ? strtol(v, NULL, 0) : 0;
}

static long arith_parse_number(arith_parser* p) {
    if (!p || !p->expr) return 0;

    skip_whitespace(p);

    /* Parenthesized expression (recursive). Comma-aware: $(((1,2,3)+10))
     * must support the comma operator grouping too. */
    if (p->expr[p->pos] == '(') {
        p->pos++;
        long result = arith_parse_comma(p);
        skip_whitespace(p);
        if (p->expr[p->pos] == ')') p->pos++;
        return result;
    }

    /* $VAR (dollar form inside arithmetic) — look up shell variable.
     * Handles: $name, $1-$9, $?, $#, $$, $!, $-, $@, $* (bash compat).
     * Without this, $1 inside $(( )) was parsed as the literal number 1
     * instead of the first positional parameter — breaking recursion in
     * shell functions (fact $(( $1 - 1 )) always computed $1 as 1). */
    if (p->expr[p->pos] == '$') {
        p->pos++;
        /* Brace form: ${name}, ${1}-${9}, ${?}, etc. */
        if (p->expr[p->pos] == '{') {
            p->pos++; /* consume '{' */
            char name[64];
            unsigned n = 0;
            /* Collect everything up to '}' */
            while (p->expr[p->pos] && p->expr[p->pos] != '}' && n + 1 < sizeof(name))
                name[n++] = p->expr[p->pos++];
            name[n] = 0;
            if (p->expr[p->pos] == '}') p->pos++; /* consume '}' */
            jash_state_t* s = (jash_state_t*)p->state;
            /* BUG-104: ${#name} / ${#name[@]} length form inside $(( )) —
             * arith_lookup_var can't resolve "#b", so every length expansion
             * evaluated to 0. Handle it here. name[1] must be non-empty so
             * ${#} (argc) still reaches the special-var path below. */
            if (s && name[0] == '#' && name[1]) {
                const char* body = name + 1;
                size_t blen = strlen(body);
                if (blen > 3 && (strcmp(body + blen - 3, "[@]") == 0 ||
                                 strcmp(body + blen - 3, "[*]") == 0)) {
                    extern int jash_array_count(jash_state_t* s, const char* base);
                    char base[64];
                    size_t bl = blen - 3;
                    if (bl >= sizeof(base)) bl = sizeof(base) - 1;
                    memcpy(base, body, bl); base[bl] = 0;
                    return (long)jash_array_count(s, base);
                }
                const char* v = jash_env_get(s, body);
                if (!v) {  /* bash: ${#arr} == ${#arr[0]} */
                    char e0[80];
                    snprintf(e0, sizeof(e0), "%s[0]", body);
                    v = jash_env_get(s, e0);
                }
                return v ? (long)strlen(v) : 0;
            }
            return (s && n > 0) ? arith_lookup_var(s, name) : 0;
        }
        /* Special single-char variables: $?, $#, $$, $!, $-, $@, $*, $0-$9 */
        char c = p->expr[p->pos];
        if (c == '?' || c == '#' || c == '$' || c == '!' ||
            c == '-' || c == '@' || c == '*' ||
            (c >= '0' && c <= '9')) {
            char name[4];
            name[0] = c;
            name[1] = '\0';
            p->pos++;
            jash_state_t* s = (jash_state_t*)p->state;
            return s ? arith_lookup_var(s, name) : 0;
        }
        /* Fall through to identifier lookup for $name */
    }

    /* Identifier: variable lookup via env */
    if (isalpha((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_') {
        char name[64];
        unsigned n = 0;
        while (p->expr[p->pos] && n + 1 < sizeof(name) &&
               (isalnum((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_')) {
            name[n++] = p->expr[p->pos++];
        }
        name[n] = 0;
        /* BUG-079: array element name[subscript] — consume the subscript and
         * resolve the element value via arith_lookup_var. */
        if (p->expr[p->pos] == '[') {
            int depth = 0;
            while (p->expr[p->pos] && n + 1 < sizeof(name)) {
                char cc = p->expr[p->pos++];
                name[n++] = cc;
                if (cc == '[') depth++;
                else if (cc == ']') { depth--; if (depth == 0) break; }
            }
            name[n] = 0;
            jash_state_t* sa = (jash_state_t*)p->state;
            long cur = sa ? arith_lookup_var(sa, name) : 0;
            /* BUG-103: postfix a[i]++ / a[i]-- — the early return here left
             * the dangling "++" silently skipped, so the increment vanished.
             * Mirror the scalar postfix write-back (old value returned). */
            if (p->expr[p->pos] == '+' && p->expr[p->pos + 1] == '+') {
                p->pos += 2; arith_var_write(sa, name, cur + 1); return cur;
            }
            if (p->expr[p->pos] == '-' && p->expr[p->pos + 1] == '-') {
                p->pos += 2; arith_var_write(sa, name, cur - 1); return cur;
            }
            return cur;
        }
        jash_state_t* s = (jash_state_t*)p->state;
        const char* val = s ? jash_env_get(s, name) : NULL;
        if (!val || !*val) val = "0";  /* unset var = 0 (still allows ++ / -- writeback) */
        /* Convert val to long — support 0x hex, 0 octal, decimal (bash compat) */
        long num = 0;
        int neg = 0;
        const char* q = val;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '-') { neg = 1; q++; }
        else if (*q == '+') { q++; }
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
            q += 2;
            while (*q) {
                if (*q >= '0' && *q <= '9')      num = arith_acc_base(num, 16, *q - '0');
                else if (*q >= 'a' && *q <= 'f') num = arith_acc_base(num, 16, *q - 'a' + 10);
                else if (*q >= 'A' && *q <= 'F') num = arith_acc_base(num, 16, *q - 'A' + 10);
                else break;
                q++;
            }
        } else if (q[0] == '0' && q[1] >= '0' && q[1] <= '7') {
            q++;
            while (*q >= '0' && *q <= '7') {
                num = arith_acc_base(num, 8, *q - '0');
                q++;
            }
        } else {
            while (*q && isdigit((unsigned char)*q)) {
                num = arith_acc_digit(num, *q - '0');
                q++;
            }
        }
        long cur = neg ? -num : num;
        /* Postfix var++ / var-- : write back the new value, return the OLD value. */
        if (p->expr[p->pos] == '+' && p->expr[p->pos + 1] == '+') {
            p->pos += 2; arith_var_write(s, name, cur + 1); return cur;
        }
        if (p->expr[p->pos] == '-' && p->expr[p->pos + 1] == '-') {
            p->pos += 2; arith_var_write(s, name, cur - 1); return cur;
        }
        return cur;
    }

    /* Number literal: hex (0x/0X), octal (0N), or decimal */
    long num = 0;
    if (p->expr[p->pos] == '0' && (p->expr[p->pos + 1] == 'x' || p->expr[p->pos + 1] == 'X')) {
        /* Hexadecimal: 0x... */
        p->pos += 2;
        while (p->expr[p->pos]) {
            char c = p->expr[p->pos];
            if (c >= '0' && c <= '9')      num = arith_acc_base(num, 16, c - '0');
            else if (c >= 'a' && c <= 'f') num = arith_acc_base(num, 16, c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') num = arith_acc_base(num, 16, c - 'A' + 10);
            else break;
            p->pos++;
        }
    } else if (p->expr[p->pos] == '0' && p->expr[p->pos + 1] >= '0' && p->expr[p->pos + 1] <= '7') {
        /* Octal: 0NNN */
        p->pos++; /* skip leading 0 */
        while (p->expr[p->pos] >= '0' && p->expr[p->pos] <= '7') {
            num = arith_acc_base(num, 8, p->expr[p->pos] - '0');
            p->pos++;
        }
    } else {
        /* Decimal or base#number notation (e.g. 16#FF, 8#17, 2#1010) */
        while (p->expr[p->pos] && isdigit((unsigned char)p->expr[p->pos])) {
            num = arith_acc_digit(num, p->expr[p->pos] - '0');
            p->pos++;
        }
        if (p->expr[p->pos] == '#' && num >= 2 && num <= 64) {
            /* base#digits: num is the base, parse following alphanumeric chars */
            long base = num;
            num = 0;
            p->pos++; /* consume '#' */
            while (p->expr[p->pos]) {
                char c = p->expr[p->pos];
                int d = -1;
                if (c >= '0' && c <= '9')      d = c - '0';
                else if (base <= 36) {
                    /* bases 2..36: letters are case-insensitive (a=A=10);
                     * '_' stays an optional digit-group separator (jash ext). */
                    if (c >= 'a' && c <= 'z')      d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
                    else if (c == '_')             { p->pos++; continue; }
                    else break;
                } else {
                    /* BUG-130: bases 37..64 — a-z=10-35, A-Z=36-61, @=62, _=63
                     * (bash). jash used A-Z=10-35 for all bases, so `37#A` gave
                     * 10 instead of 36. */
                    if (c >= 'a' && c <= 'z')      d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 36;
                    else if (c == '@')             d = 62;
                    else if (c == '_')             d = 63;
                    else break;
                }
                if (d < 0 || d >= (int)base) break;
                num = arith_acc_base(num, (int)base, d);
                p->pos++;
            }
        }
    }

    return num;
}

/* Unary operators: ++, --, !, ~, unary +, unary - (prefix) */
static long arith_parse_unary(arith_parser* p) {
    if (!p || !p->expr) return 0;

    skip_whitespace(p);

    /* Prefix ++var : increment then return NEW value (writes back). */
    if (p->expr[p->pos] == '+' && p->expr[p->pos + 1] == '+') {
        p->pos += 2;
        skip_whitespace(p);
        char nm[64];
        if (arith_read_name(p, nm, sizeof(nm))) {
            long v = arith_var_read((jash_state_t*)p->state, nm) + 1;
            arith_var_write((jash_state_t*)p->state, nm, v);
            return v;
        }
        return arith_parse_unary(p) + 1;
    }

    /* Prefix --var : decrement then return NEW value (writes back). */
    if (p->expr[p->pos] == '-' && p->expr[p->pos + 1] == '-') {
        p->pos += 2;
        skip_whitespace(p);
        char nm[64];
        if (arith_read_name(p, nm, sizeof(nm))) {
            long v = arith_var_read((jash_state_t*)p->state, nm) - 1;
            arith_var_write((jash_state_t*)p->state, nm, v);
            return v;
        }
        return arith_parse_unary(p) - 1;
    }

    /* Logical NOT */
    if (p->expr[p->pos] == '!') {
        p->pos++;
        long val = arith_parse_unary(p);
        return val ? 0 : 1;
    }

    /* Bitwise NOT */
    if (p->expr[p->pos] == '~') {
        p->pos++;
        long val = arith_parse_unary(p);
        return ~val;
    }

    /* Unary + (++ already handled above) */
    if (p->expr[p->pos] == '+') {
        p->pos++;
        return arith_parse_unary(p);
    }

    /* Unary - (-- already handled above). BUG-122: the old `!isdigit(next)`
     * guard made `-3` SKIP unary minus and fall through to a number parse
     * that does NOT consume the `-`, so the operand became 0 and the `-`
     * leaked to the additive level. Standalone `-2` only worked by accident
     * (0 - 2 at the additive level); `5 * -3` gave -3 (5*0, then 0-3) and
     * `6 / -2` gave "division by 0". Always apply unary minus. */
    if (p->expr[p->pos] == '-') {
        p->pos++;
        return -arith_parse_unary(p);
    }

    return arith_parse_number(p);
}

/* Power operator: ** (right-associative). BUG-123: was a left-folding
 * while-loop giving 2**3**2 = (2**3)**2 = 64; bash is RIGHT-associative
 * (2**(3**2) = 512). Recurse on the right operand instead. */
static long arith_parse_power(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_unary(p);

    skip_whitespace(p);
    if (p->expr[p->pos] == '*' && p->expr[p->pos + 1] == '*') {
        p->pos += 2;
        long right = arith_parse_power(p);   /* right-associative */
        /* SECURITY: saturating power with an iteration cap (no overflow UB,
         * no uninterruptible loop on a huge exponent). */
        result = arith_pow(result, right);
    }

    return result;
}

/* Multiplicative: *, /, % */
static long arith_parse_mul(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_power(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        char op = p->expr[p->pos];
        if (op == '*' && p->expr[p->pos + 1] != '*') {
            p->pos++;
            result = arith_mul_sat(result, arith_parse_power(p)); /* no overflow UB */
        } else if (op == '/') {
            p->pos++;
            result = arith_div(result, arith_parse_power(p));     /* guards /0 and MIN/-1 */
        } else if (op == '%') {
            p->pos++;
            result = arith_mod(result, arith_parse_power(p));     /* guards %0 and MIN/-1 */
        } else {
            break;
        }
    }

    return result;
}

/* Additive: +, - */
static long arith_parse_add(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_mul(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        char op = p->expr[p->pos];
        if (op == '+' && p->expr[p->pos + 1] != '+') {
            p->pos++;
            result = arith_add_sat(result, arith_parse_mul(p)); /* no overflow UB */
        } else if (op == '-' && p->expr[p->pos + 1] != '-') {
            p->pos++;
            result = arith_sub_sat(result, arith_parse_mul(p)); /* no overflow UB */
        } else {
            break;
        }
    }

    return result;
}

/* Bitwise shift: <<, >> */
static long arith_parse_shift(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_add(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '<' && p->expr[p->pos + 1] == '<') {
            p->pos += 2;
            long shift_amount = arith_parse_add(p);
            result = arith_shl(result, shift_amount);   /* SECURITY: clamp [0,63] */
        } else if (p->expr[p->pos] == '>' && p->expr[p->pos + 1] == '>') {
            p->pos += 2;
            long shift_amount = arith_parse_add(p);
            result = arith_shr(result, shift_amount);   /* SECURITY: clamp [0,63] */
        } else {
            break;
        }
    }

    return result;
}

/* Bitwise AND: & */
static long arith_parse_bitwise_and(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_shift(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '&' && p->expr[p->pos + 1] != '&') {
            p->pos++;
            result &= arith_parse_shift(p);
        } else {
            break;
        }
    }

    return result;
}

/* Bitwise XOR: ^ */
static long arith_parse_bitwise_xor(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_bitwise_and(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '^') {
            p->pos++;
            result ^= arith_parse_bitwise_and(p);
        } else {
            break;
        }
    }

    return result;
}

/* Bitwise OR: | */
static long arith_parse_bitwise_or(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_bitwise_xor(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '|' && p->expr[p->pos + 1] != '|') {
            p->pos++;
            result |= arith_parse_bitwise_xor(p);
        } else {
            break;
        }
    }

    return result;
}

/* Comparison: <, >, <=, >=, ==, != */
static long arith_parse_comparison(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_bitwise_or(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '<' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            long right = arith_parse_bitwise_or(p);
            result = (result <= right) ? 1 : 0;
        } else if (p->expr[p->pos] == '>' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            long right = arith_parse_bitwise_or(p);
            result = (result >= right) ? 1 : 0;
        } else if (p->expr[p->pos] == '!' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            long right = arith_parse_bitwise_or(p);
            result = (result != right) ? 1 : 0;
        } else if (p->expr[p->pos] == '=' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            long right = arith_parse_bitwise_or(p);
            result = (result == right) ? 1 : 0;
        } else if (p->expr[p->pos] == '<' && p->expr[p->pos + 1] != '<') {
            p->pos++;
            long right = arith_parse_bitwise_or(p);
            result = (result < right) ? 1 : 0;
        } else if (p->expr[p->pos] == '>' && p->expr[p->pos + 1] != '>') {
            p->pos++;
            long right = arith_parse_bitwise_or(p);
            result = (result > right) ? 1 : 0;
        } else {
            break;
        }
    }

    return result;
}

/* Logical AND/OR: &&, || (bash: short-circuit, returns 0 or 1) */
static long arith_parse_logical(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_comparison(p);

    while (p->expr[p->pos]) {
        skip_whitespace(p);

        if (p->expr[p->pos] == '&' && p->expr[p->pos + 1] == '&') {
            p->pos += 2;
            long right = arith_parse_comparison(p);
            result = (result && right) ? 1 : 0;
        } else if (p->expr[p->pos] == '|' && p->expr[p->pos + 1] == '|') {
            p->pos += 2;
            long right = arith_parse_comparison(p);
            result = (result || right) ? 1 : 0;
        } else {
            break;
        }
    }

    return result;
}

/* Ternary conditional: cond ? then : else (right-associative).
 * SIDE EFFECT FIX (BUG-125): Only the selected branch is evaluated.
 * The non-selected branch is parsed only for position advancement but NOT evaluated. */
static long arith_parse_ternary(arith_parser* p) {
    long cond = arith_parse_logical(p);
    skip_whitespace(p);
    if (p->expr[p->pos] == '?') {
        p->pos++;
        if (cond) {
            long then_val = arith_parse_assignment(p);
            skip_whitespace(p);
            if (p->expr[p->pos] == ':') p->pos++;
            /* Skip else branch without evaluating */
            size_t else_start = p->pos;
            int depth = 1;
            while (p->expr[p->pos] && depth > 0) {
                if (p->expr[p->pos] == '?') depth++;
                else if (p->expr[p->pos] == ':') {
                    depth--;
                    if (depth == 0) break;
                }
                else if (p->expr[p->pos] == '\'') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '\'') p->pos++; }
                else if (p->expr[p->pos] == '"') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '"') { if (p->expr[p->pos] == '\\') p->pos++; p->pos++; } }
                else if (p->expr[p->pos] == '`') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '`') p->pos++; }
                p->pos++;
            }
            p->pos = else_start; /* reset for completeness */
            return then_val;
        } else {
            /* Skip then branch without evaluating */
            size_t then_start __attribute__((unused)) = p->pos;
            int depth = 1;
            while (p->expr[p->pos] && depth > 0) {
                if (p->expr[p->pos] == '?') depth++;
                else if (p->expr[p->pos] == ':') {
                    depth--;
                    if (depth == 0) break;
                }
                else if (p->expr[p->pos] == '\'') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '\'') p->pos++; }
                else if (p->expr[p->pos] == '"') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '"') { if (p->expr[p->pos] == '\\') p->pos++; p->pos++; } }
                else if (p->expr[p->pos] == '`') { p->pos++; while (p->expr[p->pos] && p->expr[p->pos] != '`') p->pos++; }
                p->pos++;
            }
            if (p->expr[p->pos] == ':') p->pos++;
            long else_val = arith_parse_assignment(p);
            return else_val;
        }
    }
    return cond;
}

/* Parse assignment operators: =, +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>= */
static long arith_parse_assignment(arith_parser* p) {
    if (!p || !p->expr) return 0;

    skip_whitespace(p);

    /* Check for identifier followed by assignment operator */
    if ((isalpha((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_') && p->state) {
        unsigned start_pos = p->pos;
        char name[64];
        unsigned n = 0;
        while (p->expr[p->pos] && n + 1 < sizeof(name) &&
               (isalnum((unsigned char)p->expr[p->pos]) || p->expr[p->pos] == '_')) {
            name[n++] = p->expr[p->pos++];
        }
        name[n] = 0;
        /* BUG-103: subscripted lvalue a[expr] — the scan stopped at '[', so
         * the operator detection saw '[' and matched nothing; "a[2] = 99"
         * backtracked into a plain READ with " = 99" silently dropped. */
        if (p->expr[p->pos] == '[') {
            int depth = 0;
            while (p->expr[p->pos] && n + 1 < sizeof(name)) {
                char cc = p->expr[p->pos++];
                name[n++] = cc;
                if (cc == '[') depth++;
                else if (cc == ']') { depth--; if (depth == 0) break; }
            }
            name[n] = 0;
        }

        skip_whitespace(p);

        /* Check for assignment operator. Two-char operators (<<=, >>=) must
         * be checked before the single-char table below, or "<<=" would
         * never match (the single-char loop only ever looks at pos/pos+1,
         * landing on '<','<' and missing the '=' at pos+2 entirely).
         * BUG-103 latent UB: `op` used to POINT INTO the block-local ops[]
         * array below — dangling once the else-block closed. It happened to
         * survive until a stack-layout change clobbered the dead slot and
         * op[0] read garbage (compound += silently became plain =). Store
         * the operator CHARACTER instead. */
        char opch = 0;
        int oplen = 0;
        char op2 = 0;  /* for 3-char ops: first of the two operator chars */
        if (p->expr[p->pos] == '<' && p->expr[p->pos + 1] == '<' && p->expr[p->pos + 2] == '=') {
            op2 = '<'; oplen = 3;
        } else if (p->expr[p->pos] == '>' && p->expr[p->pos + 1] == '>' && p->expr[p->pos + 2] == '=') {
            op2 = '>'; oplen = 3;
        } else {
            char ops[] = { '+', '-', '*', '/', '%', '&', '|', '^' };
            for (int i = 0; i < 8; i++) {
                if (p->expr[p->pos] == ops[i] && p->expr[p->pos + 1] == '=') {
                    opch = ops[i];
                    oplen = 2;
                    break;
                }
            }
        }
        if (!opch && !op2 && p->expr[p->pos] == '=' && p->expr[p->pos + 1] != '=') {
            opch = '=';
            oplen = 1;
        }

        if (opch || op2) {
            p->pos += oplen;
            long rhs = arith_parse_expr(p);
            long cur = 0;
            /* BUG-103: normalize a[expr] → a[N] ONCE and reuse the key for
             * both the read and the write (a subscript with side effects,
             * e.g. a[i++], must evaluate exactly once). */
            char lkey[128];
            arith_norm_key((jash_state_t*)p->state, name, lkey, sizeof(lkey));
            const char* val = jash_env_get((jash_state_t*)p->state, lkey);
            if (val && *val) {
                cur = strtol(val, NULL, 10);
            }
            long result = rhs;
            if (op2 == '<') result = arith_shl(cur, rhs);
            else if (op2 == '>') result = arith_shr(cur, rhs);
            else if (oplen == 2) {
                switch (opch) {
                    case '+': result = arith_add_sat(cur, rhs); break;
                    case '-': result = arith_sub_sat(cur, rhs); break;
                    case '*': result = arith_mul_sat(cur, rhs); break;
                    case '/': result = arith_div(cur, rhs); break;
                    case '%': result = arith_mod(cur, rhs); break;
                    case '&': result = cur & rhs; break;
                    case '|': result = cur | rhs; break;
                    case '^': result = cur ^ rhs; break;
                }
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", result);
            jash_env_set((jash_state_t*)p->state, lkey, buf);
            return result;
        }
        p->pos = start_pos;  /* backtrack before skipped whitespace */
    }

    return arith_parse_ternary(p);
}

/* Main expression parser (top level). Deliberately does NOT handle the
 * comma operator — this is also what an assignment's RHS parses through
 * (see arith_parse_assignment above), and comma is LOWER precedence than
 * assignment in C/bash. If this function ate commas, `x=1,y=2` would
 * parse as `x = (1, y=2)` (x getting the comma-expression's value, with
 * y=2 as a side effect of computing x's RHS) instead of the correct
 * `(x=1), (y=2)` — two independent comma-separated assignments. Use
 * arith_parse_comma() below at the actual statement/grouping level
 * (top-level $((...)) and parenthesized sub-expressions) instead. */
static long arith_parse_expr(arith_parser* p) {
    if (!p || !p->expr) return 0;

    return arith_parse_assignment(p);
}

/* Comma operator — lowest precedence, evaluates every comma-separated
 * sub-expression left to right and yields the value of the LAST one
 * (C/bash semantics: $((1,2,3)) == 3, $((x=1,y=2,x+y)) == 3 with x=1
 * and y=2 both actually taking effect). Callers: the top-level $((...))
 * entry point and parenthesized grouping — NOT assignment-RHS parsing,
 * see arith_parse_expr's comment above. */
static long arith_parse_comma(arith_parser* p) {
    if (!p || !p->expr) return 0;

    long result = arith_parse_assignment(p);
    skip_whitespace(p);
    while (p->expr[p->pos] == ',') {
        p->pos++;
        result = arith_parse_assignment(p);
        skip_whitespace(p);
    }
    return result;
}

long jash_eval_arithmetic(const char* expr, void* state) {
    if (!expr) return 0;

    arith_parser p = {
        .expr = expr,
        .pos = 0,
        .state = state,
    };

    return arith_parse_comma(&p);
}

int jash_has_arithmetic(const char* str) {
    if (!str) return 0;
    return (jash_expansion_mask(str) & JASH_EXP_ARITH) != 0;
}

/* Expand all $((expr)) substrings in `str`, replacing each with its decimal value.
 * Returns pointer to static buffer with the rewritten string.
 * If no expansion is needed, returns `str` unchanged. */
static const char* expand_arithmetic_impl(const char* str, void* state, int known_present) {
    if (!str || (!known_present && !jash_has_arithmetic(str))) return str;

    static char out[1024];
    unsigned o = 0;
    const char* p = str;
    /* Quote context: arithmetic expansion is suppressed inside single quotes
     * (and after a backslash), exactly like $VAR. Double quotes still allow it.
     * Without this, `echo '$((1+1))'` printed 2 instead of the literal text —
     * and, worse, a parent shell passing `jash -c '... $((N)) ...'` had the
     * arithmetic pre-expanded before the inner shell ever ran. F E R M I ∞ H A R T */
    int sq = 0, dq = 0;

    /* JINV-102: stack buffers for expression expansion */
    char expr_stack[1024];
    char* expr_buf = NULL;

    while (*p && o + 1 < sizeof(out)) {
        if (p[0] == '\\' && !sq && p[1]) {           /* backslash escape (not in '') */
            out[o++] = *p++;
            if (o + 1 < sizeof(out)) out[o++] = *p++;
            continue;
        }
        if (p[0] == '\'' && !dq) { sq = !sq; out[o++] = *p++; continue; }
        if (p[0] == '"'  && !sq) { dq = !dq; out[o++] = *p++; continue; }
        if (sq) { out[o++] = *p++; continue; }       /* literal inside '' */
        if (p[0] == '$' && p[1] == '(' && p[2] == '(') {
            p += 3;
            /* Find matching )) accounting for nested ( ) */
            const char* start = p;
            int depth = 1;
            while (*p) {
                if (p[0] == '(' ) { depth++; p++; }
                else if (p[0] == ')' && p[1] == ')' && depth == 1) {
                    break;
                }
                else if (p[0] == ')') { depth--; p++; }
                else p++;
            }
            if (!*p) break; /* unterminated — bail */
            unsigned elen = (unsigned)(p - start);

            /* JINV-102: use exact-size buffer, no truncation */
            expr_buf = elen < sizeof(expr_stack) ? expr_stack : NULL;
            if (!expr_buf) {
                expr_buf = malloc(elen + 1);
                if (!expr_buf) break; /* OOM: bail gracefully */
            }
            for (unsigned i = 0; i < elen; i++) expr_buf[i] = start[i];
            expr_buf[elen] = 0;
            p += 2; /* consume )) */

            long val = jash_eval_arithmetic(expr_buf, state);
            /* Format val into out */
            char num[32];
            int n = 0;
            int neg = 0;
            unsigned long u;
            /* SECURITY: negate in unsigned (defined for LONG_MIN; -val is UB). */
            if (val < 0) { neg = 1; u = -(unsigned long)val; }
            else u = (unsigned long)val;
            if (u == 0) num[n++] = '0';
            else {
                char tmp[32]; int t = 0;
                while (u > 0) { tmp[t++] = '0' + (u % 10); u /= 10; }
                while (t > 0) num[n++] = tmp[--t];
            }
            /* JINV-087: `$((…))` também é expansão — e uma palavra que passou
             * por expansão NÃO CITADA corta também nos caracteres de IFS
             * LITERAIS dela (`IFS=:; for d in $((1+1)):x` são dois campos no
             * bash). A marca só sai quando o `for` pediu E o `$IFS` tem
             * não-branco; fora disso a saída é byte a byte a de sempre. */
            if (!dq && jash_ifs_word_mark_wanted() && o + 1 < sizeof(out))
                out[o++] = JASH_EXP_MARK;
            if (neg && o + 1 < sizeof(out)) out[o++] = '-';
            for (int i = 0; i < n && o + 1 < sizeof(out); i++) out[o++] = num[i];
            if (expr_buf != expr_stack) free(expr_buf);
        } else {
            if (expr_buf != expr_stack) free(expr_buf);
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out;
}

const char* jash_expand_arithmetic(const char* str, void* state) {
    return expand_arithmetic_impl(str, state, 0);
}

const char* jash_expand_arithmetic_known(const char* str, void* state) {
    return expand_arithmetic_impl(str, state, 1);
}
