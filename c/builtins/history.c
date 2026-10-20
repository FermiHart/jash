// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/history — show command history.

#include "builtin.h"
#include <core/history.h>
#include <theme/colors.h>

int jash_b_history(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* history -c clears. */
    if (args) {
        while (*args == ' ' || *args == '\t') args++;
        if (args[0] == '-' && args[1] == 'c') {
            jash_history_clear(s);
            return 0;
        }
    }
    uint32_t n = jash_history_count(s);
    for (uint32_t i = 0; i < n; i++) {
        const char* line = jash_history_at(s, i);
        if (line) jash_writef(w, ctx, "%s%5u%s  %s\n",
                              J_DIM, i + 1, J_RESET, line);
    }
    return 0;
}
