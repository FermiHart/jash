// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/var_cache — variable expansion caching for performance

#ifndef JASH_VAR_CACHE_H
#define JASH_VAR_CACHE_H

#include <core/state.h>

/* Cache structure for variable expansions */
typedef struct {
    char key[64];    /* Variable name + original string pattern */
    char value[512]; /* Expanded result */
    time_t timestamp; /* Time of last expansion (LRU eviction only) */
    uint64_t generation; /* s->env_generation at store time (ADR-0036 W5) —
                           * the entry is only valid if this still matches
                           * the shell's CURRENT env_generation. */
    int valid;       /* 1 if entry is valid, 0 if empty */
} jash_var_cache_entry_t;

#define JASH_VAR_CACHE_SIZE 64  /* Number of cache entries */

typedef struct {
    jash_var_cache_entry_t entries[JASH_VAR_CACHE_SIZE];
    int size;
} jash_var_cache_t;

/* ── Cache API ───────────────────────────────────────────────────────────────── */

/* Initialize variable cache for shell state */
void jash_var_cache_init(jash_state_t* s);

/* Lookup cached expansion - returns NULL if not found */
const char* jash_var_cache_lookup(jash_state_t* s, const char* original);

/* Store expansion in cache */
void jash_var_cache_store(jash_state_t* s, const char* original, const char* expanded);

/* Invalidate expired cache entries */
void jash_var_cache_invalidate(jash_state_t* s);

#endif /* JASH_VAR_CACHE_H */
