// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/alias — alias table (key -> expansion text)
//
// Aliases are lexical-style: scoped to the current jash_state_t. In
// Phase 3, function bodies will get a *child* state with a snapshot
// alias table, so aliases defined inside a function do not leak out.
// For now, single global table per jash instance.

#ifndef JASH_CORE_ALIAS_H
#define JASH_CORE_ALIAS_H

#include "state.h"

/* Set or replace an alias. Returns:
 *   0   ok
 *  -1   table full
 *  -2   bad input (NULL or empty key) */
int jash_alias_set(jash_state_t* s, const char* key, const char* val);

/* Look up. Returns expansion text (read-only) or NULL. */
const char* jash_alias_get(const jash_state_t* s, const char* key);

/* Remove. Returns 0 on success, -1 if not found. */
int jash_alias_unset(jash_state_t* s, const char* key);

uint32_t jash_alias_count(const jash_state_t* s);

/* Iterate. Returns 1 with key/val on success, 0 if out of range. */
int jash_alias_at(const jash_state_t* s, uint32_t i,
                  const char** out_key, const char** out_val);

#endif /* JASH_CORE_ALIAS_H */
