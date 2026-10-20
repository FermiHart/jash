// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: jobs — list background jobs. ADR-0042 F6 adds file-backed detached
// jobs (`jash --bg`): `jobs --json` lists them; `jobs out <id>` streams output.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "builtin.h"
#include <job_control.h>
#include <core/jobs_bg.h>

int jash_b_jobs(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;

    /* ADR-0042 F6: `jobs out <id>` — stream ~/.jash/jobs/<id>.out. Satisfies
     * "output readable" without touching the recall module. */
    if (strncmp(a, "out", 3) == 0 && (a[3] == '\0' || a[3] == ' ' || a[3] == '\t')) {
        const char* p = a + 3;
        while (*p == ' ' || *p == '\t') p++;
        int id = atoi(p);
        if (id <= 0) { jash_writes(jash_stderr_writer, 0, "jobs out: need a job id\r\n"); return 2; }
        char op[680];
        if (!jash_jobs_bg_out_path(id, op, sizeof op)) return 1;
        FILE* f = fopen(op, "r");
        if (!f) { jash_writef(jash_stderr_writer, 0, "jobs: no output for job %d\r\n", id); return 1; }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) (void)w(ctx, buf, n);
        (void)fclose(f);
        return 0;
    }

    /* ADR-0042 F6: `jobs --json` — merged file-backed jobs (what agents parse). */
    if (strstr(a, "--json")) {
        char* buf = malloc(65536);
        if (!buf) return 1;
        int n = jash_jobs_bg_list_json(buf, 65536);
        if (n > 0) (void)w(ctx, buf, (size_t)n);
        (void)w(ctx, "\n", 1);
        free(buf);
        return 0;
    }

    /* Bare `jobs`: the in-process job table (interactive parity). */
    if (!s || !s->jobs) { fprintf(stderr, "jash: jobs: no job control\n"); return 1; }
    jash_job_list(s->jobs);
    return 0;
}
