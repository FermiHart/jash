// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/gold — the Builtin Renaissance shared contract (ADR-0044 Wave A0)
//
// "Gold for LLMs": the cross-cutting capabilities that make a jash builtin
// objectively better for an LLM agent than the upstream command it shadows.
// Built ONCE here, called by every gold builtin — a CONTRACT, not per-builtin
// copy-paste. These are thin facades over jash's existing in-process ledgers
// (output_budget, salience, world-delta, agent_json, timeline, chain) — the
// superpower is that a builtin runs in the SAME process as those ledgers, which
// bash/GNU structurally cannot. Keep this layer allocation-light and syscall-
// light: it is on the agent hot path and performance is paramount.
//
// THE GATE (purity is a property of the gate, not of code location):
//   jash_gold_disabled()  — the explicit kill-switch (JASH_GOLD=0). When true a
//                           gold builtin MUST fall back to byte-identical POSIX
//                           behavior. This is how we prove upstream parity.
//   jash_gold_active()    — the implicit persona: agent detected AND not
//                           disabled. Governs DEFAULT-ON enrichments (salience
//                           ordering, inline annotations). Explicit flags like
//                           --delta / --json do NOT need this — they only need
//                           !disabled — so a human can still opt in by typing
//                           the flag.
//
// Both are cached after first evaluation: the gate costs one branch on the hot
// path, and the default path pays nothing.

#ifndef JASH_CORE_GOLD_H
#define JASH_CORE_GOLD_H

#include <stddef.h>
#include <stdint.h>
#include <theme/io.h>

/* ── The gate ──────────────────────────────────────────────────────────────
 * disabled: JASH_GOLD=0 (explicit). active: agent-mode && !disabled. Both are
 * evaluated once and cached (env does not change within a -c invocation). */
int jash_gold_disabled(void);
int jash_gold_active(void);

/* ── Session identity + stable keys ────────────────────────────────────────
 * Every "since I last looked" feature keys on (session, logical-target). The
 * session id is CLAUDE_CODE_SESSION_ID, else JASH_SESSION, else "nosession".
 * jash_gold_key derives an 8-hex FNV-1a key from two strings (e.g. sid | absdir)
 * so distinct sessions/targets never collide on disk. */
const char* jash_gold_session(void);
void        jash_gold_key(const char* a, const char* b, char* out, size_t cap);

/* ── Capability: DELTA (state-over-full-dump) ──────────────────────────────
 * Generalizes ls --delta so ANY builtin can answer "what changed since I last
 * ran this here?" cheaply. The builtin owns RENDERING (how a change looks); the
 * contract owns PERSISTENCE (per-(session,key) snapshot under
 * ~/.jash/gold-delta/<builtin>/<key>.snap, atomic tmp+rename).
 *
 * Each entry is a name + a 64-bit fingerprint (the caller's notion of "changed"
 * — nanosecond mtime, content hash, size, …) + an optional size + a 1-char tag
 * (type: 'f' file, 'd' dir, 'l' symlink, or builtin-specific). Records are
 * stored sorted by name so the caller can merge-diff prev vs cur in one pass. */
typedef struct {
    char      name[256];
    long long fp;     /* fingerprint: caller-defined "did it change" value      */
    long long size;   /* optional payload (bytes); 0 if unused                  */
    char      tag;    /* type/category char; 0 if unused                        */
} jash_gold_dent_t;

/* Load the prior snapshot for (builtin, key) into prev[], newest known state.
 * Returns entry count (>=0), or -1 if no snapshot exists yet (first look).
 * On success *age_s (may be NULL) is set to seconds since the snapshot. */
int  jash_gold_delta_load(const char* builtin, const char* key,
                          jash_gold_dent_t* prev, int max, long long* age_s);

/* Atomically write cur[0..n-1] (MUST be sorted by name) as the new snapshot. */
void jash_gold_delta_store(const char* builtin, const char* key,
                           const jash_gold_dent_t* cur, int n);

/* Checked observation/publication for structured callers. A missing snapshot
 * is complete and empty; an unreadable/malformed snapshot is an error. Reads
 * never create directories. Entries remain caller-owned, sorted by raw name.
 * The new length-aware JGOLD2 format preserves every non-NUL filename byte;
 * complete valid JGOLD1 files remain readable. max is a hard bound, not a page.
 * bytes/digest bind the complete source; a failed observation cannot authorize
 * a publish. No claim of cryptographic provenance is made by the digest. */
typedef struct {
    int present, complete, error_number;
    size_t count, bytes;
    int64_t timestamp_seconds, age_seconds;
    unsigned char digest[32];
    char path[4096];
} jash_gold_delta_observation_t;
typedef struct {
    int applied, durable, directories_created, lock_created, error_number;
    const char* phase;
    size_t bytes_written;
} jash_gold_delta_publication_t;
int jash_gold_delta_load_checked(const char* builtin, const char* key,
                          jash_gold_dent_t* prev, size_t max,
                          jash_gold_delta_observation_t* observation);
/* Caller admits its output before publication. The store locks, rereads and
 * compares the source digest/presence, and refuses stale observations. Actual
 * rename and directory fsync are separate applied/durable facts. Optional poll
 * is cooperative while waiting for the lock and before the publish boundary;
 * nonzero poll stops with ECANCELED without disturbing the lock owner. */
int jash_gold_delta_store_checked(const char* builtin, const char* key,
                          const jash_gold_dent_t* cur, size_t count,
                          const jash_gold_delta_observation_t* expected,
                          int (*poll)(void*), void* poll_context,
                          jash_gold_delta_publication_t* publication);

/* Human age string ("just now", "3m ago", "2d ago") from a snapshot age. */
void jash_gold_age(long long age_s, char* out, size_t cap);

/* ── Capability: BUDGET-CAP (never flood the window) ───────────────────────
 * The effective output budget for this invocation (JASH_LLM_BUDGET / governor).
 * jash_gold_flush_file salience-ranks a materialized temp file under that
 * budget: if it fits, verbatim; else the top salient lines + a footer, with the
 * full body spilled to ~/.jash/overflow/ for `recall`. Returns 1 fit / 0 capped
 * / -1 declined (caller falls back to its own tail path). Thin facade over
 * salience.c + output_budget.c. */
long jash_gold_budget(void);
int  jash_gold_flush_file(const char* path, const char* cmdname,
                          jash_writer_t w, void* ctx);

/* ── Capability: ENVELOPE (one structured --json contract) ─────────────────
 * One shared escaper so every gold --json speaks the same dialect (kills the
 * bespoke per-builtin JSON drift). Facade over agent_json's jash_json_esc. */
void jash_gold_json_esc(const char* s, char* out, size_t cap);

#endif /* JASH_CORE_GOLD_H */
