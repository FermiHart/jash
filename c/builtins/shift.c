// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: shift — shift positional parameters

#include "builtin.h"
#include <core/env.h>
#include <string.h>

static void make_key(char* buf, int val) {
    int ki = 0;
    if (val >= 100) buf[ki++] = '0' + val / 100;
    if (val >= 10) buf[ki++] = '0' + (val / 10) % 10;
    buf[ki++] = '0' + val % 10;
    buf[ki] = 0;
}

int jash_b_shift(jash_state_t *s, jash_writer_t w, void *ctx,
                 const char *args) {
    (void)w; (void)ctx;
    /* Find the highest set positional param (= $#). */
    int hi = 0;
    for (int i = 1; i <= JASH_POS_MAX; i++) {
        char key[8]; make_key(key, i);
        if (jash_env_get(s, key)) hi = i;
        else break;
    }

    int n = 1;
    if (args) {
        while (*args == ' ' || *args == '\t') args++;
        if (*args) {
            /* BUG-139: a non-numeric argument is an error (rc=1), params
             * left untouched. */
            const char* d = args;
            if (!(*d >= '0' && *d <= '9')) {
                jash_writef(jash_stderr_writer, 0,
                    "jash: shift: %s: numeric argument required\n", args);
                return 1;
            }
            n = 0;
            while (*d >= '0' && *d <= '9') { n = n * 10 + (*d - '0'); d++; }
            while (*d == ' ' || *d == '\t') d++;
            if (*d) {
                jash_writef(jash_stderr_writer, 0,
                    "jash: shift: %s: numeric argument required\n", args);
                return 1;
            }
        }
    }
    /* BUG-137: shift count greater than $# is an error (rc=1), positionals
     * UNCHANGED (jash used to clear them). BUG-138: `shift 0` is a valid
     * no-op (the old `if (n<1) n=1` turned it into `shift 1`). */
    if (n > hi) return 1;
    if (n == 0) return 0;
    /* Shift params: i -> i-n. Scan low to high; dest < src so no overwrite. */
    for (int i = n + 1; i <= hi; i++) {
        char sk[8]; make_key(sk, i);
        const char *val = jash_env_get(s, sk);
        if (!val) break;
        char dk[8]; make_key(dk, i - n);
        jash_env_set(s, dk, val);
    }
    /* Unset the trailing params that fell off */
    for (int i = hi; i > hi - n && i >= 1; i--) {
        char key[8]; make_key(key, i);
        jash_env_unset(s, key);
    }
    return 0;
}
