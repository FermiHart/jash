// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: return — return from function with exit code

#include <stdio.h>
#include <stdlib.h>
#include "builtin.h"

int jash_b_return(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w;
    (void)ctx;

    int exit_code = 0;
    if (args && *args) {
        exit_code = atoi(args);
    } else if (s) {
        /* BUG-084: bare `return` yields the exit status of the LAST command
         * executed in the function (POSIX), not a hardcoded 0. */
        exit_code = s->last_status.code;
    }

    /* BUG-012: signal the command-list executor to stop and the function
     * executor to unwind. jash_eval_function_with_redir consumes/clears the
     * flag at the function boundary (mirrors exit_requested). Bare `return`
     * outside a function is harmless — the top-level driver just sees rc. */
    if (s) {
        s->return_requested = 1;
        s->return_code = exit_code;
    }
    return exit_code;
}
