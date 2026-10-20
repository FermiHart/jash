// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: job_control — background jobs, job control, fg/bg/jobs/wait
//
// Real POSIX job control: each job is a process GROUP (pgid == leader pid).
// Signals are delivered to the whole group via kill(-pgid, sig). The shell
// hands the controlling terminal to a foreground job with tcsetpgrp and
// reclaims it when the job stops or exits.

#ifndef JASH_JOB_CONTROL_H
#define JASH_JOB_CONTROL_H

#include <sys/types.h>

/* Opaque type for job table (structure defined in job_control.c) */
typedef struct jash_job_table jash_job_table;
typedef struct {
    int id;
    pid_t pgid;
    int state, exit_status, notified, current, previous;
    const char* command;
    int command_truncated;
} jash_job_view_t;
size_t jash_job_count(const jash_job_table*);
int jash_job_at(const jash_job_table*, size_t index, jash_job_view_t*);
void jash_job_refresh_all(jash_job_table*);

/* Forward decl: the shell state (full definition in core/state.h). */
struct jash_state;

/* Job states (exposed for callers that report status). */
enum { JASH_JOB_RUNNING = 0, JASH_JOB_STOPPED = 1, JASH_JOB_DONE = 2 };

/* Initialize job table */
void jash_job_table_init(jash_job_table* table);

/* Allocate and initialize a new job table */
jash_job_table* jash_job_table_create(void);

/* Add a new job. `pgid` is the process-group id (leader pid) of the job.
 * Returns the assigned job id, or -1 on failure. */
int jash_job_add(jash_job_table* table, pid_t pgid, const char* cmd);

/* Update job state (e.g., after wait) */
void jash_job_update_status(jash_job_table* table, pid_t pid, int state, int exit_status);

/* Remove a job from the table (does NOT signal it). */
void jash_job_remove(jash_job_table* table, int job_id);

/* List all jobs (reaps/notes finished ones first). */
void jash_job_list(jash_job_table* table);

/* Non-blocking reap: poll all jobs for state changes (done/stopped),
 * print "[n] Done/Stopped" notices, and drop finished jobs. */
void jash_job_poll(jash_job_table* table);

/* Wait for a job to complete (blocking). */
int jash_job_wait(jash_job_table* table, int job_id);

/* Wait for ALL running jobs to finish. */
int jash_job_wait_all(jash_job_table* table);

/* Move job to foreground: hand it the terminal, SIGCONT if stopped, wait. */
int jash_job_fg(struct jash_state* s, jash_job_table* table, int job_id);

/* Resume job in background (SIGCONT, mark running). */
int jash_job_bg(jash_job_table* table, int job_id);

/* Remove a job from the table without signaling it (disown). */
int jash_job_disown(jash_job_table* table, int job_id);

/* Resolve a job spec ("%1", "%+", "%-", "1", or empty=current) to a job id.
 * Returns the job id or -1 if not found. */
int jash_job_resolve_spec(jash_job_table* table, const char* spec);

#endif
