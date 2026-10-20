// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — coprocess management (coproc) implementation

#include "coproc.h"
#include <core/env.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

void jash_coproc_init(coproc_table_t* ct) {
    if (!ct) return;
    for (int i = 0; i < COPROC_MAX; i++) {
        ct->slots[i].active = 0;
        ct->slots[i].name[0] = '\0';
        ct->slots[i].pid = 0;
        ct->slots[i].rd_fd = -1;
        ct->slots[i].wr_fd = -1;
        ct->slots[i].status = -1;
    }
    ct->count = 0;
}

/* Set COPROC[0], COPROC[1], and optionally NAME[0], NAME[1] in env. */
static void coproc_set_env(jash_state_t* s, const coproc_t* c) {
    char v0[32], v1[32];
    snprintf(v0, sizeof(v0), "%d", c->rd_fd);
    snprintf(v1, sizeof(v1), "%d", c->wr_fd);

    jash_env_set(s, "COPROC", v0);     /* COPROC = read fd (scalar) */
    /* Store as array elements so ${COPROC[0]} / ${COPROC[1]} work. */
    jash_env_set(s, "COPROC[0]", v0);
    jash_env_set(s, "COPROC[1]", v1);

    if (c->name[0]) {
        char k0[96], k1[96];
        snprintf(k0, sizeof(k0), "%s[0]", c->name);
        snprintf(k1, sizeof(k1), "%s[1]", c->name);
        jash_env_set(s, k0, v0);
        jash_env_set(s, k1, v1);
    }
}

/* Clear COPROC env vars for a coproc slot. */
static void coproc_clear_env(jash_state_t* s, const coproc_t* c) {
    jash_env_unset(s, "COPROC");
    jash_env_unset(s, "COPROC[0]");
    jash_env_unset(s, "COPROC[1]");
    if (c->name[0]) {
        char k0[96], k1[96];
        snprintf(k0, sizeof(k0), "%s[0]", c->name);
        snprintf(k1, sizeof(k1), "%s[1]", c->name);
        jash_env_unset(s, k0);
        jash_env_unset(s, k1);
    }
}

int jash_coproc_start(coproc_table_t* ct, jash_state_t* s,
                      const char* name, const char* cmd) {
    if (!ct || !s || !cmd || !*cmd) return -1;

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < COPROC_MAX; i++) {
        if (!ct->slots[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;  /* table full */

    /* Create two pipes:
     *   child_stdin_pipe:  parent writes  → child reads  (stdin)
     *   child_stdout_pipe: child writes   → parent reads  (stdout) */
    int in_pipe[2];   /* [0]=child_read,  [1]=parent_write */
    int out_pipe[2];  /* [0]=parent_read, [1]=child_write  */
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return -1;
    }

    long pid = sys_fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* ── Child ── */
        /* Restore default signal dispositions. */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        /* Close parent-side fds. */
        close(in_pipe[1]);   /* parent write end */
        close(out_pipe[0]);  /* parent read end  */

        /* Wire up: stdin ← in_pipe[0], stdout → out_pipe[1] */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);

        /* Execute the command via shell -c (handles pipelines, etc). */
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        /* fallback: try direct exec of first word */
        _exit(127);
    }

    /* ── Parent ── */
    /* Close child-side fds. */
    close(in_pipe[0]);   /* child read end  */
    close(out_pipe[1]);  /* child write end */

    /* Fill the coproc slot. */
    coproc_t* c = &ct->slots[slot];
    c->active = 1;
    if (name && *name) {
        unsigned nlen = 0;
        while (name[nlen] && nlen + 1 < sizeof(c->name)) { c->name[nlen] = name[nlen]; nlen++; }
        c->name[nlen] = '\0';
    } else {
        c->name[0] = '\0';
    }
    c->pid = pid;
    c->rd_fd = out_pipe[0];  /* parent reads from child's stdout */
    c->wr_fd = in_pipe[1];   /* parent writes to child's stdin   */
    c->status = -1;
    ct->count++;

    /* Set $! and COPROC env. */
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%ld", pid);
    jash_env_set(s, "!", pid_buf);
    coproc_set_env(s, c);

    return 0;
}

void jash_coproc_reap(coproc_table_t* ct, jash_state_t* s) {
    if (!ct) return;
    for (int i = 0; i < COPROC_MAX; i++) {
        coproc_t* c = &ct->slots[i];
        if (!c->active || c->pid <= 0) continue;
        int status;
        pid_t r = waitpid((pid_t)c->pid, &status, WNOHANG);
        if (r > 0) {
            c->status = status;
            /* Close parent-side fds. */
            if (c->rd_fd >= 0) { close(c->rd_fd); c->rd_fd = -1; }
            if (c->wr_fd >= 0) { close(c->wr_fd); c->wr_fd = -1; }
            c->active = 0;
            ct->count--;
            coproc_clear_env(s, c);
        }
    }
}

void jash_coproc_shutdown(coproc_table_t* ct) {
    if (!ct) return;
    for (int i = 0; i < COPROC_MAX; i++) {
        coproc_t* c = &ct->slots[i];
        if (!c->active) continue;
        /* Close fds. */
        if (c->rd_fd >= 0) { close(c->rd_fd); c->rd_fd = -1; }
        if (c->wr_fd >= 0) { close(c->wr_fd); c->wr_fd = -1; }
        /* Send SIGHUP then SIGTERM then SIGKILL. */
        if (c->pid > 0) {
            kill((pid_t)c->pid, SIGHUP);
            kill((pid_t)c->pid, SIGTERM);
            /* Give it a moment, then force kill. */
            int status;
            pid_t r = waitpid((pid_t)c->pid, &status, WNOHANG);
            if (r == 0) {
                kill((pid_t)c->pid, SIGKILL);
                waitpid((pid_t)c->pid, &status, 0);
            }
        }
        c->active = 0;
    }
    ct->count = 0;
}
