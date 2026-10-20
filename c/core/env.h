// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 The author makes music. Hear F E R M I ∞ H A R T wherever you stream.
//    No obligation. Just a wish.
//
// Module: core/env — environment variable table
//
// (FHCL-1.0 — F E R M I ∞ H A R T Creativity License — pending draft;
//  Unlicense placeholder until then.)

#ifndef JASH_CORE_ENV_H
#define JASH_CORE_ENV_H

#include "state.h"

/* env API — all operate on a jash_state_t* instance.
 *
 * Returns:
 *   jash_env_get : pointer to value (read-only) or NULL if not set.
 *   jash_env_set : 0 on success, -1 if table is full, -2 on bad input.
 *   jash_env_unset : 0 on success, -1 if key not found.
 *   jash_env_count : number of entries currently set.
 *
 * Memory: fully inline. No heap. Bounded by JASH_MAX_ENV (see state.h).
 *
 * Order: insertion order. `env` builtin lists in that order. Unset
 * compacts by swapping the last entry into the hole — order of the
 * remaining keys is preserved EXCEPT for the unset slot's neighbor.
 * This is intentional: cosmetic ordering is not contractual. */

const char* jash_env_get(const jash_state_t* s, const char* key);
int         jash_env_set(jash_state_t* s, const char* key, const char* val);
/* Internal bookkeeping writes (function-frame depth, FUNCNAME stack,
 * local-scope lists): identical table semantics, but the libc process
 * mirror is skipped. Reads through jash_env_get/`env`/declare are
 * unaffected; only children started mid-frame lose sight of these
 * `__`-prefixed keys (bash parity: call depth is not inherited). */
int         jash_env_set_nomirror(jash_state_t* s, const char* key,
                                  const char* val);
int         jash_env_unset_nomirror(jash_state_t* s, const char* key);
int         jash_env_apply_batch(jash_state_t* s,
                                 const char* const* keys,
                                 const char* const* values, int count);
int         jash_env_replace_positionals(jash_state_t* s,
                                         const char* const* values, int count);
int         jash_env_replace_positionals_owned(jash_state_t* s,
                                               char** values, int count);
/* Same as above, but the caller already knows the current positional
 * tally (from jash_env_positional_count) and the table has not changed
 * since: skips one O(env_count) rescan. old_count/old_max must match
 * what a fresh scan would report. */
int         jash_env_replace_positionals_owned_known(jash_state_t* s,
                                               char** values, int count,
                                               int old_count, int old_max);
/* O(env_count) tally of $1..$N with no allocation: *count = keys set,
 * *max_idx = highest index set (0 when none). */
int         jash_env_positional_count(const jash_state_t* s,
                                      int* count, int* max_idx);
int         jash_env_append(jash_state_t* s, const char* key, const char* suffix);
int         jash_env_unset(jash_state_t* s, const char* key);
/* Native call frames (perf W12): push/pop depth + FUNCNAME without table
 * traffic. Reads of those keys prefer native state while depth>0. */
int         jash_frame_enter(jash_state_t* s, const char* name);
void        jash_frame_exit(jash_state_t* s);
/* Materialize native frame state into the table (dumps, execve envp).
 * No-op unless frames are active and unsynced. */
void        jash_env_sync_frame(jash_state_t* s);
uint32_t    jash_env_count(const jash_state_t* s);

/* Iteration: returns the i-th entry's key+value via out pointers, or
 * 0 if i is out of range. Use jash_env_count() to bound the loop. */
int jash_env_at(const jash_state_t* s, uint32_t i,
                const char** out_key, const char** out_val);

/* Iterate only env entries belonging to `base[...]`. `probes` counts hash-chain
 * entries examined, making the O(members + bucket collisions) bound testable. */
typedef struct {
    const jash_state_t* state;
    const char* base;
    size_t base_len;
    int32_t next;
    uint32_t probes;
} jash_env_array_iter_t;

void jash_env_array_index_init(jash_state_t* s);
void jash_env_array_index_rebuild(jash_state_t* s);
void jash_env_array_iter_init(jash_env_array_iter_t* it,
                              const jash_state_t* s, const char* base);
int  jash_env_array_iter_next(jash_env_array_iter_t* it);

/* Seed canonical defaults (PATH, HOME, USER, SHELL, TERM, COLORTERM,
 * LANG, JOSH_VERSION, JOSH_KERNEL, JOSH_RELEASE). Called once at
 * jash startup, after state_init. */
void jash_env_seed_defaults(jash_state_t* s);

#endif /* JASH_CORE_ENV_H */
