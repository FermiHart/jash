// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: continue — continue to next loop iteration

#include <stdio.h>
#include <stdlib.h>
#include "builtin.h"

int jash_b_continue(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w;
    (void)ctx;

    if (!s) return 1;

    /* BUG-166: `continue N` fully exits the N-1 innermost loops (like break)
     * and continues the Nth. Same loose-parsing convention as break. */
    int n = 1;
    if (args && *args) {
        char* end = NULL;
        long v = strtol(args, &end, 10);
        if (end != args && v >= 1) n = (int)v;
    }
    s->loop_break_level = n;
    /* JINV-073: mesma marca de canal de controle do `break` (state.h). */
    s->loop_ctl = JASH_LOOPCTL_CONTINUE;
    return JASH_RC_CONTINUE;  /* Special code for CONTINUE */
}
