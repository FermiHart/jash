// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/claims — distributed file intent-locks (ADR-0042 F9).
//
// mesh claims TASKS; F9 claims FILES. When many agents share a workspace, two
// editing the same file is silent corruption discovered only at build time —
// the #1 multi-agent bug class. A claim records "agent OWNER intends to work on
// PATH for TTL seconds". Before a mutating op (rm/mv) on a path a *live* OTHER
// agent has claimed, the mutation guard warns (advisory) or blocks
// (JASH_CLAIM_STRICT=1). Claims are heartbeat-refreshed; a claim whose owner
// goes silent past its TTL is auto-reclaimed (ignored, then pruned).
//
// On-disk: ~/.jash/mesh/claims.tsv — "path\towner\tclaim_ts\tttl\tnote" per
// line, under the shared ~/.jash/mesh/.lock flock (same critical section as
// tasks). Knobs: JASH_CLAIM=0 disables enforcement; JASH_CLAIM_STRICT=1 blocks.
//
// F E R M I ∞ H A R T
#ifndef JASH_CORE_CLAIMS_H
#define JASH_CORE_CLAIMS_H

#include <stddef.h>
#include "mesh_store.h"

#define JASH_CLAIM_PATH_MAX    512
#define JASH_CLAIM_OWNER_MAX   160
#define JASH_CLAIM_NOTE_MAX    256
#define JASH_CLAIM_DEFAULT_TTL 900   /* seconds, when --ttl is omitted */
#define JASH_CLAIM_MAX         512   /* claims.tsv line cap (defensive) */

typedef struct {
    char path[JASH_CLAIM_PATH_MAX];
    char owner[JASH_CLAIM_OWNER_MAX];
    long claim_ts;
    long ttl;
    char note[JASH_CLAIM_NOTE_MAX];
} jash_claim_t;

/* Low-level checked store operations for a mesh transaction which already
 * owns its directory/lock. read_at returns the full count or -1; it never
 * prunes, silently skips malformed rows, or truncates at the caller's cap.
 * write_at separates applied publication from durability and later errors. */
int jash_claims_read_at(int directory, jash_claim_t*, int maximum);
int jash_claims_write_at(int directory, const jash_claim_t*, int count, jash_mesh_io_t*);
int jash_claims_live(const jash_claim_t*, long now);

// Normalize a user path to a stable absolute key: realpath when it resolves,
// else cwd-prefixed (so a relative claim and a later guard check on the same
// file compare equal across cwds). Returns 1 on success, 0 on failure.
int jash_claims_norm(const char* path, char* out, size_t cap);

// Upsert OWNER's claim on norm_path (already normalized). owner must be
// non-empty. ttl<=0 → JASH_CLAIM_DEFAULT_TTL. note may be NULL. Returns 0 on
// success, 1 if a *live* claim by a DIFFERENT owner already holds the path
// (refused — you cannot claim what another live agent holds), -1 on I/O error.
int jash_claims_put(const char* norm_path, const char* owner, long ttl,
                    const char* note);

// Remove OWNER's claim on norm_path. Returns 1 if removed, 0 if none, -1 on error.
int jash_claims_remove(const char* norm_path, const char* owner);

// Heartbeat: set claim_ts=now for every claim owned by OWNER. Returns the count
// refreshed (0 when the owner holds none), or -1 on I/O/validation error.
int jash_claims_refresh_owner(const char* owner);

// Dead-agent sweep: drop every claim owned by OWNER (their process is gone,
// so their intent-locks must not block the living). Returns count or -1 on error.
int jash_claims_drop_owner(const char* owner);

// Is there a LIVE claim (now-claim_ts <= ttl) on norm_path owned by someone
// != self_owner? self_owner may be "" (anonymous → any live claim conflicts).
// On a conflict returns 1 and fills owner_out and *ttl_out (either may be
// NULL); else 0. Returns -1 on a source error, 0 when the store is absent.
int jash_claims_conflict(const char* norm_path, const char* self_owner,
                         char* owner_out, size_t owner_cap, long* ttl_out);

// Load all LIVE claims without writing or pruning. A cap/source error returns
// -1, rather than a successful shortened set. Returns the live count otherwise.
int jash_claims_load_all(jash_claim_t* arr, int max);

// ── Envelope accumulator (session-scoped; mirrors guard metrics) ─────────────
// The guard notes each distinct conflicting path it saw THIS process; the
// agent-JSON envelope reads them back for "claim_conflicts":[...].
void jash_claims_note_conflict(const char* path);
int  jash_claims_conflicts(const char** out, int max);  // fills out[], returns n
int  jash_claims_conflicts_truncated(void);

#endif /* JASH_CORE_CLAIMS_H */
