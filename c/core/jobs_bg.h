// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/jobs_bg — cross-`-c` detached async jobs (ADR-0042 F6).
//
// `jash --bg -c 'make -j8'` returns immediately with a job id; the command runs
// detached (setsid) with full sigil attestation, its combined stdout+stderr to
// ~/.jash/jobs/<id>.out. On child exit the record is finalized to done. A later
// `-c` learns of completion WITHOUT polling: the JASH_AGENT envelope reports
// "jobs_done":[<id>] (a notify flag is committed after a successful envelope
// write; concurrent readers may see duplicates, and client receipt is not acked). `wait <id>`
// blocks for the completion record.
//
// On-disk: ~/.jash/jobs/{nextid, <id>.json, <id>.out} under a flocked
// ~/.jash/jobs/.lock. Records are single-line JSON authored only by jash, so a
// tiny fixed-shape reader suffices (no general JSON parser). Knob: none — bg is
// explicit via --bg. See SPEC.md § Background jobs. F E R M I ∞ H A R T
#ifndef JASH_CORE_JOBS_BG_H
#define JASH_CORE_JOBS_BG_H

#include <stddef.h>

#define JASH_BGJOB_CMD_MAX 1024   /* stored JSON-escaped */

typedef struct {
    int  id;
    int  running;    /* 1 = running, 0 = done */
    long pid;
    int  rc;
    long ms;
    long start_ts;
    long end_ts;
    int  notified;
    char cmd[JASH_BGJOB_CMD_MAX];   /* JSON-escaped, as stored on disk */
} jash_bgjob_t;

// Allocate the next job id (atomic bump of ~/.jash/jobs/nextid under flock).
// Returns the id (>=1), or 0 on failure (no HOME / I/O).
int jash_jobs_bg_alloc(void);

// Write the initial "running" record for id (cmd is raw; stored JSON-escaped).
// Returns 0 on success.
int jash_jobs_bg_start(int id, long pid, const char* cmd);

// Rewrite id's record as "done" with rc/ms and end_ts=now, notified=false,
// preserving pid/start_ts/cmd. Returns 0 on success.
int jash_jobs_bg_finalize(int id, int rc, long ms);

// Fill out with ~/.jash/jobs/<id>.out. Returns 1 on success, 0 on failure.
int jash_jobs_bg_out_path(int id, char* out, size_t cap);

// Collect ids of jobs that are done && !notified, set their notified flag, and
// return the count (<=max). One-shot: feeds the envelope's "jobs_done".
int jash_jobs_bg_done_scan(int* ids, int max);
/* Delivery-aware envelope path: peek does not consume notifications; acknowledge
 * only after successful output. Concurrent readers/crash may duplicate IDs. */
int jash_jobs_bg_done_peek(int* ids, int max);
int jash_jobs_bg_done_ack(const int* ids, int count);

// Emit a JSON array of all jobs (merged) into out. Returns bytes written.
int jash_jobs_bg_list_json(char* out, size_t cap);

// Load one record. Returns 1 if found, 0 otherwise.
int jash_jobs_bg_load(int id, jash_bgjob_t* out);

#endif /* JASH_CORE_JOBS_BG_H */
