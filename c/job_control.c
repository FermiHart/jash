// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: job_control — real POSIX job control.
//
// Each job is a process GROUP. The job leader's pid IS the pgid; every
// process in the pipeline joins that group via setpgid. Signals go to the
// whole group: kill(-pgid, SIG). The shell lends the controlling terminal
// to a foreground group with tcsetpgrp() and takes it back afterwards.

#include "job_control.h"
#include <core/state.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>

/* Job states stored as the enum from the header. */
typedef struct {
    int   job_id;        /* Job number (1, 2, 3...) */
    pid_t pgid;          /* Process GROUP id (== leader pid) */
    int   state;         /* JASH_JOB_RUNNING / STOPPED / DONE */
    int   exit_status;   /* Exit code (if DONE) */
    int   notified;      /* 1 = a Done/Stopped notice was already printed */
    int   command_truncated;
    char  cmd[256];      /* Command line */
} jash_job;

struct jash_job_table {
    jash_job jobs[32];
    int count;
    int next_job_id;
    int current;   /* job id of "current" job (%+) */
    int previous;  /* job id of "previous" job (%-) */
};

/* ---- WIFSTOPPED-safe decode helpers (avoid macro portability issues) ---- */

static int job_decode(int status) {
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 128;
}

void jash_job_table_init(jash_job_table* table) {
    if (!table) return;
    memset(table, 0, sizeof(*table));
    table->next_job_id = 1;
}

jash_job_table* jash_job_table_create(void) {
    jash_job_table* table = (jash_job_table*)malloc(sizeof(jash_job_table));
    if (table) jash_job_table_init(table);
    return table;
}

static jash_job* job_by_id(jash_job_table* t, int job_id) {
    if (!t) return NULL;
    for (int i = 0; i < t->count; i++)
        if (t->jobs[i].job_id == job_id) return &t->jobs[i];
    return NULL;
}

static jash_job* job_by_pgid(jash_job_table* t, pid_t pgid) {
    if (!t) return NULL;
    for (int i = 0; i < t->count; i++)
        if (t->jobs[i].pgid == pgid) return &t->jobs[i];
    return NULL;
}

/* Track current/previous as bash does: most-recently-backgrounded/stopped
 * becomes current, the prior current becomes previous. */
static void job_mark_current(jash_job_table* t, int job_id) {
    if (!t) return;
    if (t->current != job_id) {
        t->previous = t->current;
        t->current = job_id;
    }
}

int jash_job_add(jash_job_table* table, pid_t pgid, const char* cmd) {
    if (!table || table->count >= 32) return -1;
    jash_job* job = &table->jobs[table->count];
    job->job_id = table->next_job_id++;
    job->pgid = pgid;
    job->state = JASH_JOB_RUNNING;
    job->exit_status = 0;
    job->notified = 0;
    job->command_truncated = cmd && strlen(cmd) >= sizeof(job->cmd);
    job->cmd[0] = 0;
    if (cmd) { strlcpy(job->cmd, cmd, sizeof(job->cmd)); job->cmd[sizeof(job->cmd) - 1] = 0; }
    table->count++;
    job_mark_current(table, job->job_id);
    return job->job_id;
}

void jash_job_update_status(jash_job_table* table, pid_t pid, int state, int exit_status) {
    jash_job* job = job_by_pgid(table, pid);
    if (job) { job->state = state; job->exit_status = exit_status; }
}

void jash_job_remove(jash_job_table* table, int job_id) {
    if (!table) return;
    for (int i = 0; i < table->count; i++) {
        if (table->jobs[i].job_id == job_id) {
            for (int j = i; j < table->count - 1; j++) table->jobs[j] = table->jobs[j + 1];
            table->count--;
            if (table->current == job_id)  table->current = table->previous, table->previous = 0;
            if (table->previous == job_id) table->previous = 0;
            return;
        }
    }
}

/* Reap one job's group without blocking; update state. Returns 1 if the
 * job changed state. WUNTRACED reports stops; WNOHANG keeps it async. */
static int job_refresh(jash_job* job) {
    int status = 0;
    /* wait on the whole group: -pgid */
    pid_t r = waitpid(-job->pgid, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if (r <= 0) return 0;
    if (WIFSTOPPED(status)) {
        if (job->state != JASH_JOB_STOPPED) { job->state = JASH_JOB_STOPPED; job->notified = 0; return 1; }
        return 0;
    }
#ifdef WIFCONTINUED
    if (WIFCONTINUED(status)) {
        if (job->state != JASH_JOB_RUNNING) { job->state = JASH_JOB_RUNNING; return 1; }
        return 0;
    }
#endif
    /* exited or signaled */
    job->state = JASH_JOB_DONE;
    job->exit_status = job_decode(status);
    job->notified = 0;
    return 1;
}

size_t jash_job_count(const jash_job_table* table) { return table ? (size_t)table->count : 0; }
int jash_job_at(const jash_job_table* table, size_t index, jash_job_view_t* view) {
    if (!table || !view || index >= (size_t)table->count) return 0;
    const jash_job* job = &table->jobs[index];
    *view = (jash_job_view_t){job->job_id, job->pgid, job->state, job->exit_status,
        job->notified, job->job_id == table->current, job->job_id == table->previous,
        job->cmd, job->command_truncated};
    return 1;
}
void jash_job_refresh_all(jash_job_table* table) {
    if (table) for (int i = 0; i < table->count; i++) job_refresh(&table->jobs[i]);
}

void jash_job_poll(jash_job_table* table) {
    if (!table) return;
    for (int i = 0; i < table->count; ) {
        jash_job* job = &table->jobs[i];
        job_refresh(job);
        if (job->state == JASH_JOB_DONE && !job->notified) {
            printf("[%d]   Done                    %s\n", job->job_id, job->cmd);
            job->notified = 1;
            jash_job_remove(table, job->job_id);
            continue; /* table shifted; re-check this slot */
        }
        if (job->state == JASH_JOB_STOPPED && !job->notified) {
            printf("[%d]+  Stopped                 %s\n", job->job_id, job->cmd);
            job->notified = 1;
        }
        i++;
    }
    fflush(stdout);
}

void jash_job_list(jash_job_table* table) {
    if (!table || table->count == 0) return;
    /* refresh first so the listing is current */
    for (int i = 0; i < table->count; i++) job_refresh(&table->jobs[i]);
    for (int i = 0; i < table->count; i++) {
        jash_job* job = &table->jobs[i];
        const char* st = "Running";
        if (job->state == JASH_JOB_STOPPED) st = "Stopped";
        else if (job->state == JASH_JOB_DONE) st = "Done";
        char mark = ' ';
        if (job->job_id == table->current) mark = '+';
        else if (job->job_id == table->previous) mark = '-';
        printf("[%d]%c  %-22s  %s\n", job->job_id, mark, st, job->cmd);
    }
    fflush(stdout);
}

int jash_job_wait(jash_job_table* table, int job_id) {
    jash_job* job = job_by_id(table, job_id);
    if (!job) return -1;
    int status = 0;
    int last = job->exit_status;
    /* Block on the whole group until it has no more non-stopped members. */
    for (;;) {
        pid_t r = waitpid(-job->pgid, &status, WUNTRACED);
        if (r < 0) break; /* ECHILD: all reaped */
        if (WIFSTOPPED(status)) { job->state = JASH_JOB_STOPPED; return 128 + WSTOPSIG(status); }
        if (WIFEXITED(status) || WIFSIGNALED(status)) last = job_decode(status);
    }
    job->state = JASH_JOB_DONE;
    job->exit_status = last;
    jash_job_remove(table, job_id);
    return last;
}

int jash_job_wait_all(jash_job_table* table) {
    if (!table) return 0;
    int last = 0;
    /* snapshot ids because the table shrinks as jobs finish */
    int ids[32]; int n = 0;
    for (int i = 0; i < table->count && n < 32; i++)
        if (table->jobs[i].state == JASH_JOB_RUNNING) ids[n++] = table->jobs[i].job_id;
    for (int i = 0; i < n; i++) {
        int rc = jash_job_wait(table, ids[i]);
        if (rc >= 0) last = rc;
    }
    return last;
}

int jash_job_fg(struct jash_state* s, jash_job_table* table, int job_id) {
    jash_job* job = job_by_id(table, job_id);
    if (!job) return -1;

    printf("%s\n", job->cmd);   /* bash prints the command being foregrounded */
    fflush(stdout);
    int active = (s && s->job_control_active);
    int tty = (s ? s->shell_tty_fd : -1);

    /* Hand the terminal to the job's group. */
    if (active && tty >= 0) tcsetpgrp(tty, job->pgid);

    /* Resume if stopped. */
    if (job->state == JASH_JOB_STOPPED) {
        kill(-job->pgid, SIGCONT);
        job->state = JASH_JOB_RUNNING;
    }

    /* Wait (allowing it to stop again under Ctrl-Z). */
    int status = 0, last = job->exit_status, stopped = 0;
    for (;;) {
        pid_t r = waitpid(-job->pgid, &status, WUNTRACED);
        if (r < 0) break;
        if (WIFSTOPPED(status)) { job->state = JASH_JOB_STOPPED; stopped = 1; last = 128 + WSTOPSIG(status); break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) last = job_decode(status);
    }

    /* Reclaim the terminal for the shell. */
    if (active && tty >= 0) tcsetpgrp(tty, s->shell_pgid);

    if (stopped) {
        printf("\n[%d]+  Stopped                 %s\n", job->job_id, job->cmd);
        return last;
    }
    job->state = JASH_JOB_DONE;
    job->exit_status = last;
    jash_job_remove(table, job_id);
    return last;
}

int jash_job_bg(jash_job_table* table, int job_id) {
    jash_job* job = job_by_id(table, job_id);
    if (!job) return -1;
    if (job->state == JASH_JOB_STOPPED) {
        kill(-job->pgid, SIGCONT);
        job->state = JASH_JOB_RUNNING;
    }
    printf("[%d]+ %s &\n", job->job_id, job->cmd);
    fflush(stdout);
    return 0;
}

int jash_job_disown(jash_job_table* table, int job_id) {
    jash_job* job = job_by_id(table, job_id);
    if (!job) return -1;
    jash_job_remove(table, job_id);
    return 0;
}

int jash_job_resolve_spec(jash_job_table* table, const char* spec) {
    if (!table || table->count == 0) return -1;
    /* empty or "%+"/"%%" => current; "%-" => previous */
    if (!spec || !*spec) return table->current ? table->current : table->jobs[table->count - 1].job_id;
    if (spec[0] == '%') spec++;
    if (!*spec || spec[0] == '+' || spec[0] == '%') return table->current;
    if (spec[0] == '-') return table->previous;
    if (spec[0] >= '0' && spec[0] <= '9') return atoi(spec);
    return -1;
}
