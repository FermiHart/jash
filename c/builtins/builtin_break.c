// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: break — break out of loop

#include <stdio.h>
#include <stdlib.h>
#include "builtin.h"

int jash_b_break(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w;
    (void)ctx;

    if (!s) return 1;

    /* BUG-166: `break N` exits N enclosing loops, not just the innermost.
     * Loose parsing (bare atoi-style, no error path) matches this codebase's
     * existing convention for numeric loop args (grep's -m/-A/-B/-C). N<1
     * clamps to 1 (still breaks the innermost loop) rather than erroring. */
    int n = 1;
    if (args && *args) {
        char* end = NULL;
        long v = strtol(args, &end, 10);
        if (end != args && v >= 1) n = (int)v;
    }
    s->loop_break_level = n;
    /* JINV-073: marca o CANAL DE CONTROLE. O rc 255 continua saindo (os
     * consumidores ainda nao migrados dependem dele), mas quem ja migrou
     * exige esta marca — e por isso um `exit 255` de verdade, que nunca passa
     * por aqui, deixou de ser confundido com um pedido de break. */
    s->loop_ctl = JASH_LOOPCTL_BREAK;
    return JASH_RC_BREAK;  /* Special code for BREAK */
}
