// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/eval — evaluate arguments as a command line.

#include "builtin.h"
#include <parser/eval.h>
#include <core/argv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Multi-line scripts route through the same quote-aware logical-line reader
 * and compound accumulator that `-c` uses. */
extern int jash_run_multiline(jash_state_t* state, const char* sp);

int jash_b_eval(jash_state_t *s, jash_writer_t w, void *ctx,
                const char *args) {
    if (!args || !*args) return 0;

    /* BUG-059: POSIX eval = (1) expand its arguments (done by the caller
     * before dispatch), (2) REMOVE one layer of quoting, (3) join the words
     * with spaces, (4) re-parse the result. Step (2) was missing — the raw
     * quoted text was re-parsed directly, so text quoted to survive TWO
     * parses (the `'\''` idiom every agent harness emits, e.g. Claude Code
     * wraps commands as `eval '<cmd>'`) kept its outer quote layer and came
     * out mangled. jash_argv_build IS the POSIX quote removal: outer quotes
     * drop, quote characters inside quotes survive. */
    jash_argv_t av;
    int n = jash_argv_build(args, &av);
    if (n < 0) {
        jash_writes(w, ctx, "eval: unterminated quote\n");
        return 2;
    }
    if (n == 0) { jash_argv_free(&av); return 0; }

    size_t need = 1;
    for (int i = 0; i < av.argc; i++) need += strlen(av.argv[i]) + 1;
    char* joined = (char*)malloc(need);
    if (!joined) { jash_argv_free(&av); return 1; }
    size_t o = 0;
    for (int i = 0; i < av.argc; i++) {
        if (i) joined[o++] = ' ';
        size_t l = strlen(av.argv[i]);
        memcpy(joined + o, av.argv[i], l);
        o += l;
    }
    joined[o] = '\0';
    jash_argv_free(&av);

    /* Multi-line results take the same quote-aware logical-line reader and
     * compound accumulator a multi-line -c takes; single lines take the
     * normal evaluator. */
    int rc;
    if (strchr(joined, '\n')) rc = jash_run_multiline(s, joined);
    else                      rc = jash_eval_line(s, w, ctx, joined);
    free(joined);
    return rc;
}
