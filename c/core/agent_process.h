// SPDX-License-Identifier: LicenseRef-SPL-1.0
// External process supervisor for the structured execution IR.

#ifndef JASH_CORE_AGENT_PROCESS_H
#define JASH_CORE_AGENT_PROCESS_H

#include "execution_ir.h"
#include "agent_cgroup.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define JASH_AGENT_PROCESS_PATH_MAX       4096u
#define JASH_AGENT_PROCESS_MAX_ENV        256u
#define JASH_AGENT_PROCESS_MAX_ENV_BYTES  65536u
#define JASH_AGENT_PROCESS_DEFAULT_GRACE_MS 200u
#define JASH_AGENT_PROCESS_MAX_GRACE_MS   60000u
#define JASH_AGENT_PROCESS_DEFAULT_OUTPUT_LIMIT (64ull * 1024ull * 1024ull)

typedef enum {
    JASH_AGENT_PROCESS_NEW = 0,
    JASH_AGENT_PROCESS_RUNNING = 1,
    JASH_AGENT_PROCESS_TERM_SENT = 2,
    JASH_AGENT_PROCESS_KILL_SENT = 3,
    JASH_AGENT_PROCESS_INDETERMINATE = 4,
    JASH_AGENT_PROCESS_DONE = 5,
} jash_agent_process_state_t;

typedef struct {
    int cwd_fd;                   /* Caller-owned through start(); -1 inherits. */
    const char* const* envp;      /* NULL inherits environ. */
    int stdin_fd;                 /* -1 means /dev/null outside PTY mode. */
    int stdin_pipe;               /* Runtime-owned writable non-PTY stdin. */
    const char* stdout_path;      /* Required secure regular-file spool. */
    const char* stderr_path;      /* Required and distinct from stdout_path. */
    int background;               /* Foreground start waits before returning. */
    int pty;                      /* Linux controlling PTY for stdin/stdout. */
    unsigned pty_rows;
    unsigned pty_cols;
    unsigned cancel_grace_ms;
    uint64_t output_limit_bytes;  /* Per output file; defaults to 64 MiB. */
    int spool_limit_only;         /* Internal: poll spools without RLIMIT_FSIZE.
                                  * May overshoot; never a hard file-size cap.
                                  * Default 0 preserves the protocol limit. */
    int cgroup_parent_fd;         /* Caller-owned through start(); default -1. */
    const jash_agent_limits_t* resources; /* NULL: legacy. Otherwise all-or-refused.
                                         * CPU bandwidth, not cumulative CPU time;
                                         * see agent_cgroup.h migration caveat. */
    /* Runtime-owned session descriptors must not be retained by a fork-only
     * launcher. Borrowed through start; stdio entries are replaced by /dev/null. */
    const int* close_in_launcher;
    size_t close_in_launcher_count;
} jash_agent_process_options_t;

typedef struct {
    pid_t launcher_pid;           /* Reaped by this module at DONE. */
    pid_t pgid;                   /* Worker group; launcher remains its reaper. */
    jash_agent_process_state_t state;
    int background;
    int pty;
    int cancel_requested;
    int kill_sent;
    int exit_code;                /* 0..255 when DONE. */
    int term_signal;              /* Exact last pipeline worker signal. */
    int terminal_status_valid;    /* Last worker wait status was received. */
    unsigned completion_version; /* Versioned private launcher notification. */
    unsigned completion_event_kind; /* 1: Linux pidfd, 0: bounded poll fallback. */
    uint64_t completion_notifications,wait_wakeups;
    int launcher_unexpected;      /* Launcher ended without terminal status. */
    int launcher_term_signal;     /* Exact launcher signal, if unexpected. */
    int quiescent;                /* Worker PGID is confirmed absent. */
    int quiescence_forced;        /* SIGKILL was needed to make it absent. */
    int descendants_quiescent;    /* Launcher proved it had no children. */
    int descendants_forced;       /* Detached descendants required SIGKILL. */
    int indeterminate;            /* Full descendant absence was not proved. */
    int cleanup_error_number;     /* Nonzero when cleanup could not be proved. */
    uint64_t output_limit_bytes;  /* Effective configured per-file limit. */
    int output_limit_hit;         /* An output spool reached its hard cap. */
    int resources_enforced;
    int resource_timed_out;       /* Launcher-enforced deadline, no client polling needed. */
    int resource_empty;           /* Aggregate group confirmed unpopulated. */
    int resource_violation;       /* Sticky max events or timeout, even with exit_code 0. */
    uint64_t resource_memory_events;
    uint64_t resource_pids_events;
    jash_agent_limits_t resources; /* Copied effective request. Swap=0, oom.group=1. */
} jash_agent_process_result_t;

typedef struct jash_agent_process jash_agent_process_t;

void jash_agent_process_options_init(jash_agent_process_options_t* options);

int jash_agent_process_start(const jash_execution_ir_t* ir,
                             const jash_agent_process_options_t* options,
                             jash_agent_process_t** out,
                             char* error, size_t error_cap);

/* poll returns 1 when done, 0 when still running, and -1 on supervisor I/O
 * failure. wait returns 0 when done, 1 on timeout, and -1 on failure. */
int jash_agent_process_poll(jash_agent_process_t* process, int timeout_ms);
int jash_agent_process_wait(jash_agent_process_t* process, int timeout_ms);

/* Synchronously sends SIGTERM to the worker PGID, waits grace_ms, then sends
 * SIGKILL if needed. Resource jobs also receive immediate aggregate cgroup.kill
 * so detached descendants cannot ignore cancellation. The launcher survives
 * to reap every direct worker. */
int jash_agent_process_cancel(jash_agent_process_t* process,
                              unsigned grace_ms);

int jash_agent_process_resize_pty(jash_agent_process_t* process,
                                  unsigned rows, unsigned cols);

/* Returns 0 when all bytes were accepted, 1 on deadline, and -1 on error.
 * The deadline covers both PTY writes and output draining. accepted_bytes is
 * always set and may be nonzero for deadline/error. */
int jash_agent_process_write_pty(jash_agent_process_t* process,
                                 const void* data, size_t length,
                                 int timeout_ms, size_t* accepted_bytes);

/* Runtime-owned pipe writes are bounded by a finite deadline. Closing stdin
 * is an idempotent half-close that lets the child observe EOF. */
int jash_agent_process_write_stdin(jash_agent_process_t* process,
                                   const void* data, size_t length,
                                   int timeout_ms, size_t* accepted_bytes);
int jash_agent_process_close_stdin(jash_agent_process_t* process);

const jash_agent_process_result_t*
jash_agent_process_get_result(const jash_agent_process_t* process);

/* Destroying a running process cancels and reaps it before releasing FDs.
 * Checked destruction copies the final observation before releasing the
 * handle. A permanent source/signal error or cleanup deadline returns -1,
 * sets errno and leaves final_result indeterminate with quiescent=false.
 * A deadline never establishes absence. final_result may be NULL; process
 * is consumed on success and failure. The legacy void wrapper preserves
 * errno on failure but cannot replace checking a result before publication. */
int jash_agent_process_destroy_checked(jash_agent_process_t* process,
                                       jash_agent_process_result_t* final_result);
void jash_agent_process_destroy(jash_agent_process_t* process);

#endif
