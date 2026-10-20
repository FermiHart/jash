// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/exit — terminate the shell.

#include "builtin.h"
#include <theme/colors.h>
#include <core/timeline.h>
#ifndef JASH_AGENT_ONLY
#include <liquid/jash_liquid.h>
#endif

extern void sys_exit_or_loop_break(int code) __attribute__((weak));

int jash_b_exit(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;
    /* BUG-091: bare `exit` uses the status of the LAST command executed
     * (POSIX), not a hardcoded 0 — capture BEFORE anything overwrites it. */
    int code = s ? s->last_status.code : 0;
    if (args) {
        while (*args == ' ' || *args == '\t') args++;
        int neg = 0;
        const char* d = args;
        if (*d == '-') { neg = 1; d++; }
        int v = 0, saw_digit = 0;
        while (*d >= '0' && *d <= '9') { v = v*10 + (*d - '0'); d++; saw_digit = 1; }
        if (saw_digit) code = neg ? -v : v;
    }
    /* BUG-092: exit status is 8-bit (wait-status semantics) — bash reports
     * `exit 300` as 44 and `exit -1` as 255. */
    code &= 0xFF;
    /* Timeline teardown: wipe the session backup dir + free the ring. */
    jash_timeline_shutdown();
    /* ADR-0033 Wave 2: persist Liquid Reasoning Engine state. */
#ifndef JASH_AGENT_ONLY
    jash_liquid_cleanup();
#endif
    /* Mark intent; the main loop checks this flag and breaks out. */
    s->last_status.code = code;
    if (sys_exit_or_loop_break) sys_exit_or_loop_break(code);
    /* Soft signal: REPL checks s->exit_requested at the top of each loop.
     * (Used to flip s->interactive=0 here, but the REPL deliberately ignores
     * that flag mid-run — see comment in main.c near the REPL loop.) */
    s->exit_requested = 1;
    return code;
}
