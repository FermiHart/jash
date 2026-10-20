// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Job Control header

#ifndef JASH_JOB_CONTROL_H
#define JASH_JOB_CONTROL_H

#include "../builtins/builtin.h"
#include <stdbool.h>
#include <sys/types.h>

// Initialize job control
void job_control_init(void);

// Add a new job
int job_add(pid_t pid, const char* command, bool foreground);

// SIGTSTP handler (Ctrl+Z)
void sigtstp_handler(int sig);

// SIGCHLD handler
void sigchld_handler(int sig);

// Builtin: jobs
int jash_b_jobs(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

// Builtin: fg
int jash_b_fg(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

// Builtin: bg
int jash_b_bg(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

#endif // JASH_JOB_CONTROL_H
