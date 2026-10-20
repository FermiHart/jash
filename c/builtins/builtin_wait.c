// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: wait — wait for background jobs

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "builtin.h"
#include <job_control.h>
#include <core/jobs_bg.h>

/* ADR-0042 F6: block until a file-backed --bg job is done, print its completion
 * record to (w,ctx), and return its rc. Crash-safety: a "running" record whose
 * pid is gone is finalized as rc=-1 so a wait never hangs. */
static int bg_wait_file(int id, jash_writer_t w, void* ctx) {
    for (;;) {
        jash_bgjob_t j;
        if (!jash_jobs_bg_load(id, &j)) return 127;
        if (!j.running) {
            char rec[JASH_BGJOB_CMD_MAX + 128];
            int n = snprintf(rec, sizeof rec,
                "{\"id\":%d,\"state\":\"done\",\"rc\":%d,\"ms\":%ld}\n",
                j.id, j.rc, j.ms);
            if (n > 0) (void)w(ctx, rec, (size_t)n);
            return j.rc;
        }
        if (j.pid > 0 && kill((pid_t)j.pid, 0) != 0 && errno == ESRCH) {
            jash_jobs_bg_finalize(id, -1, 0);   /* child vanished → reap */
            continue;
        }
        usleep(50000);   /* 50 ms poll */
    }
}

int jash_b_wait(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;
    if (!s || !s->jobs) { fprintf(stderr, "jash: wait: no job control\n"); return 1; }

    while (args && (*args == ' ' || *args == '\t')) args++;

    int nonblock = 0;

    /* Parse -n flag */
    while (args && *args == '-') {
        args++;
        while (args && *args && *args != ' ' && *args != '\t') {
            if (*args == 'n') nonblock = 1;
            args++;
        }
        while (args && (*args == ' ' || *args == '\t')) args++;
    }

    if (nonblock) {
        /* Non-blocking: check if any children are available */
        int status = 0;
        pid_t pid = waitpid(-s->shell_pgid, &status, WNOHANG | WUNTRACED);
        if (pid == 0) {
            /* No children to wait for */
            return 127;
        }
        if (pid < 0) {
            /* No children at all */
            return 127;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    /* No args: wait for ALL running background jobs. */
    if (!args || !*args) return jash_job_wait_all(s->jobs);

    /* If the argument is a bare positive integer it is a PID, not a job
     * number.  Try to find it in the job table first (for SIGCONT / job
     * metadata); if not found, fall back to direct waitpid(pid, …) so
     * that `wait $!` works even when the child has already exited and the
     * job table entry was reaped. */
    int last_rc = 0;
    const char* ap = args;
    while (*ap) {
        while (*ap == ' ' || *ap == '\t') ap++;
        if (!*ap) break;

        /* Collect one token */
        const char* tok_start = ap;
        while (*ap && *ap != ' ' && *ap != '\t') ap++;
        unsigned tok_len = (unsigned)(ap - tok_start);
        if (tok_len == 0) continue;
        char tok[64];
        if (tok_len >= sizeof(tok)) tok_len = sizeof(tok) - 1;
        memcpy(tok, tok_start, tok_len);
        tok[tok_len] = '\0';

        /* Bare integer → PID */
        int is_pid = 1;
        for (unsigned i = 0; i < tok_len; i++) {
            if (tok[i] < '0' || tok[i] > '9') { is_pid = 0; break; }
        }
        if (is_pid && tok_len > 0) {
            /* ADR-0042 F6: a bare integer that names a file-backed --bg job
             * (a <id>.json record exists) is a job id, not a pid. */
            int fid = atoi(tok);
            jash_bgjob_t fj;
            if (fid > 0 && jash_jobs_bg_load(fid, &fj)) {
                last_rc = bg_wait_file(fid, w, ctx);
                continue;
            }
            pid_t pid = (pid_t)atoi(tok);
            /* Try job table first (updates state on reap) */
            if (s->jobs) {
                /* Search job table for this pgid/pid */
                int found_jid = -1;
                for (int ji = 0; ji < 32; ji++) {
                    /* jash_job_resolve_spec by pid: no API exists; use waitpid */
                }
                (void)found_jid;
            }
            /* Direct waitpid: works even after job table entry removed */
            int wstatus = 0;
            pid_t wr;
            do { wr = waitpid(pid, &wstatus, 0); } while (wr < 0 && errno == EINTR);
            if (wr < 0) {
                /* Process not a child or already reaped — return stored status */
                last_rc = 127;
            } else if (WIFEXITED(wstatus)) {
                last_rc = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                last_rc = 128 + WTERMSIG(wstatus);
            }
        } else {
            /* Job spec (%1, %+, etc.) */
            int job_id = jash_job_resolve_spec(s->jobs, tok);
            if (job_id < 0) { last_rc = 127; continue; }
            int rc = jash_job_wait(s->jobs, job_id);
            last_rc = rc < 0 ? 127 : rc;
        }
    }
    return last_rc;
}
