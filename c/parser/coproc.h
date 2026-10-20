// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — coprocess management (coproc)
//
// Implements bash-compatible coproc:
//   coproc [NAME] COMMAND [REDIRECTIONS]
//
// Creates two pipes connecting parent and child. Parent reads from
// COPROC[0] and writes to COPROC[1]. $! holds the child PID.

#ifndef COPROC_H
#define COPROC_H

#include <core/state.h>

/* Maximum concurrent coprocesses (named + default). */
#define COPROC_MAX 4

typedef struct {
    int    active;         /* 1 if slot is in use                         */
    char   name[64];       /* "" for the default (anonymous) coproc      */
    long   pid;            /* child PID                                   */
    int    rd_fd;          /* parent read fd  (child stdout → parent)     */
    int    wr_fd;          /* parent write fd (parent → child stdin)      */
    int    status;         /* waitpid status when reaped, -1 if running   */
} coproc_t;

/* Table of active coprocesses. */
typedef struct coproc_table_s {
    coproc_t slots[COPROC_MAX];
    int      count;
} coproc_table_t;

/* Initialise the coproc table (call once at startup). */
void jash_coproc_init(coproc_table_t* ct);

/* Start a coprocess. Returns 0 on success, -1 on error.
 * `name` may be "" for the default coproc.
 * `cmd` is the command line to execute (already expanded).
 * On success, sets COPROC[0], COPROC[1] in env and $! to pid. */
int jash_coproc_start(coproc_table_t* ct, jash_state_t* s,
                      const char* name, const char* cmd);

/* Reap any finished coprocs (non-blocking waitpid). Called from eval loop. */
void jash_coproc_reap(coproc_table_t* ct, jash_state_t* s);

/* Shutdown all coprocs (close fds, kill children). */
void jash_coproc_shutdown(coproc_table_t* ct);

#endif /* COPROC_H */
