// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: bg — resume job in background

#include <stdio.h>
#include <stdlib.h>
#include "builtin.h"
#include <job_control.h>

int jash_b_bg(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;
    if (!s || !s->jobs) { fprintf(stderr, "jash: bg: no job control\n"); return 1; }

    while (args && (*args == ' ' || *args == '\t')) args++;
    int job_id = jash_job_resolve_spec(s->jobs, args);
    if (job_id < 0) { fprintf(stderr, "jash: bg: no such job\n"); return 1; }

    int rc = jash_job_bg(s->jobs, job_id);
    if (rc < 0) { fprintf(stderr, "jash: bg: %s: no such job\n", args ? args : ""); return 1; }
    return 0;
}
