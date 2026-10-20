// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: suspend — stop the shell itself by sending SIGSTOP to our own
// process group (bash uses SIGSTOP). Resumed when the parent SIGCONTs us.

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include "builtin.h"

int jash_b_suspend(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;

    /* `suspend -f` would force-suspend a login shell; we accept and ignore it. */
    while (args && (*args == ' ' || *args == '\t')) args++;

    /* A login shell normally refuses to suspend; jash standalone allows it.
     * Send SIGSTOP to ourselves; control returns after a SIGCONT. */
    int pgid = (s && s->shell_pgid > 0) ? s->shell_pgid : (int)getpgrp();
    if (kill(-pgid, SIGSTOP) != 0) {
        /* fall back to stopping just this process */
        raise(SIGSTOP);
    }
    return 0;
}
