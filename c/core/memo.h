// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/memo — read memoization for LLM agent sessions (ADR-0038 W1).
//
// Repeated read-only commands whose input fingerprints are unchanged answer
// with ONE attested line instead of re-emitting bytes the agent already has
// in its context window:
//
//   [jash memo] unchanged since 07:42:13 — identical to your previous run
//   (47 lines, 1.2KB, sha256:ab12cd34ef567890). inputs re-verified ·
//   JASH_MEMO=0 re-prints
//
// Scope (all conditions required — correctness by omission):
//   - agent session (ADR-0037 detection) with a session id
//     (CLAUDE_CODE_SESSION_ID or JASH_SESSION) — the claim "you already saw
//     this" is only valid for the same observer;
//   - non-interactive -c mode, output going to the session's top-level
//     stdout (never pipes, redirects, $(...) captures, pipelines);
//   - command name in a read-only whitelist, with per-command flag rules
//     (anything unrecognized or mutating simply skips memoization);
//   - every operand resolves to an existing file (dirs only for ls);
//   - JASH_MEMO != 0.
//
// Fingerprints: (size, mtime_ns, ctime_ns) per file; directories fold
// (name, size, mtime, ctime) of every entry. Same fingerprints + same
// command + same cwd + same locale ⇒ identical output for these tools, so
// the command is NOT re-executed (saves the run, not just the bytes).
//
// Store: ~/.jash/memo.tsv, ≤128 records, atomic rewrite. The read
// fingerprints double as the substrate for the stale-read guard (W3).
// F E R M I ∞ H A R T

#ifndef JASH_CORE_MEMO_H
#define JASH_CORE_MEMO_H

#include <core/state.h>
#include <theme/io.h>
#include <module/jash_crypto.h>

/* ADR-0037 harness-agnostic agent detection — defined in main.c. */
int jash_env_smells_like_agent(void);

/* main.c registers the (writer, ctx) pairs that represent the agent's
 * top-level stdout in -c mode. Anything else (capture writers, pipeline
 * writers, redirect buffers) is not memoizable. Two slots suffice. */
void jash_memo_mark_top(jash_writer_t w, void* ctx);

/* Single decision point, called before executing a simple command.
 * Returns:
 *   0 — not a memo case: execute normally, no capture;
 *   1 — memo candidate but cold/stale: execute, capture, then call one of
 *       the store functions below;
 *   2 — SERVED from memo: the attested line was written to w, *rc_out is
 *       the recorded exit code, do not execute the command. */
int jash_memo_begin(jash_state_t* s, const char* name, const char* args,
                    jash_writer_t w, void* ctx, int* rc_out);

/* Builtin capture tee: wrap the writer, dispatch, then store. */
typedef struct {
    jash_writer_t     real_w;
    void*             real_ctx;
    jash_sha256_ctx_t sha;
    unsigned long     bytes;
    unsigned long     lines;
    char              tmp_path[512]; /* ADR-0042 F1: captured bytes for delta */
    int               tmp_fd;
    int               suppress;      /* ADR-0042 F1: delta active → drop body */
} jash_memo_tee_t;

void          jash_memo_tee_init(jash_memo_tee_t* t, jash_writer_t w, void* ctx);
long          jash_memo_tee_write(void* ctx, const char* buf, size_t n);
void jash_memo_store_from_tee(jash_state_t* s, const char* name,
                              const char* args, jash_memo_tee_t* t, int rc);

/* External capture: hash/count the materialized stdout file (the output
 * budget temp file) after the child exited. */
void jash_memo_store_from_file(jash_state_t* s, const char* name,
                               const char* args, const char* path, int rc);

/* ADR-0042 F1: structural output diff (memoΔ).  Returns the path to the
 * previous run's captured output, or NULL when delta is disabled/inapplicable.
 * Called in exec.c to decide whether to suppress the full external output. */
const char* jash_memo_delta_active(void);
void jash_memo_delta_store_from_file(jash_state_t* s, const char* name,
                                     const char* args, const char* path,
                                     int rc, jash_writer_t w, void* ctx);

/* ADR-0042 F1: counts of the most recent memoΔ this invocation emitted, for
 * the JASH_AGENT envelope's "memo":{"delta":{...}} object. Returns 1 and fills
 * the out-params when a delta was emitted, 0 otherwise. */
int jash_memo_delta_last(int* add, int* del, int* age);

/* ADR-0042 F5: is this command NAME on the read-only memo whitelist? The
 * prefetch engine only speculatively runs commands that will actually memoize
 * (so the next real run is a memo hit). */
int jash_memo_name_memoizable(const char* name);

/* ADR-0038 W3: read ledger ───────────────────────────────────────────────
 * Every fingerprinted read (memo hit or miss) records "the agent last saw
 * THIS version of THIS file". The stale-write guard (fschange) compares a
 * mutated file's PRE-command mtime against this: a mismatch means the agent
 * acted on an outdated view. Returns 1 + fills mtime_ns/read_ts when the
 * current session has a recorded read of abspath. */
int jash_memo_ledger_lookup(const char* abspath,
                            unsigned long* mtime_ns, unsigned long* read_ts);

/* Agent detection — used by memo, record, and other subsystems */
int jash_env_smells_like_agent(void);

#endif /* JASH_CORE_MEMO_H */
