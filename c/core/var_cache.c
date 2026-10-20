// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/var_cache.c — variable expansion caching implementation

#include "var_cache.h"
#include <core/state.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Initialize variable cache for shell state */
void jash_var_cache_init(jash_state_t* s) {
    if (!s) return;
    
    /* Initialize cache structure */
    if (!s->var_cache) {
        s->var_cache = malloc(sizeof(jash_var_cache_t));
        if (s->var_cache) {
            memset(s->var_cache, 0, sizeof(jash_var_cache_t));
        }
    }
}

/* Lookup cached expansion - returns NULL if not found */
const char* jash_var_cache_lookup(jash_state_t* s, const char* original) {
    if (!s || !s->var_cache || !original) return NULL;
    
    jash_var_cache_t* cache = (jash_var_cache_t*)s->var_cache;
    
    /* Hash the original string to find potential slot */
    unsigned int hash = 0;
    for (const char* p = original; *p; p++) {
        hash = hash * 31 + *p;
    }
    hash %= JASH_VAR_CACHE_SIZE;
    
    /* Linear probe for collision resolution */
    for (int i = 0; i < JASH_VAR_CACHE_SIZE; i++) {
        int idx = (hash + i) % JASH_VAR_CACHE_SIZE;
        if (cache->entries[idx].valid &&
            cache->entries[idx].generation == s->env_generation &&
            strcmp(cache->entries[idx].key, original) == 0) {
            return cache->entries[idx].value;
        }
    }

    return NULL;
}

/* Store expansion in cache */
void jash_var_cache_store(jash_state_t* s, const char* original, const char* expanded) {
    if (!s || !s->var_cache || !original || !expanded) return;
    
    jash_var_cache_t* cache = (jash_var_cache_t*)s->var_cache;
    
    /* Hash the original string to find slot */
    unsigned int hash = 0;
    for (const char* p = original; *p; p++) {
        hash = hash * 31 + *p;
    }
    hash %= JASH_VAR_CACHE_SIZE;
    
    /* Linear probe to find an empty or stale (older generation) slot */
    for (int i = 0; i < JASH_VAR_CACHE_SIZE; i++) {
        int idx = (hash + i) % JASH_VAR_CACHE_SIZE;
        if (!cache->entries[idx].valid ||
            cache->entries[idx].generation != s->env_generation) {
            strlcpy(cache->entries[idx].key, original, sizeof(cache->entries[idx].key));
            strlcpy(cache->entries[idx].value, expanded, sizeof(cache->entries[idx].value));
            cache->entries[idx].timestamp = time(NULL);
            cache->entries[idx].generation = s->env_generation;
            cache->entries[idx].valid = 1;
            return;
        }
    }

    /* Cache full and every candidate slot is current-generation — evict the
     * oldest entry (pure LRU-ish tiebreak, not a correctness concern). */
    int oldest_idx = 0;
    time_t oldest_time = cache->entries[0].timestamp;
    for (int i = 1; i < JASH_VAR_CACHE_SIZE; i++) {
        if (cache->entries[i].timestamp < oldest_time) {
            oldest_time = cache->entries[i].timestamp;
            oldest_idx = i;
        }
    }

    strlcpy(cache->entries[oldest_idx].key, original, sizeof(cache->entries[oldest_idx].key));
    strlcpy(cache->entries[oldest_idx].value, expanded, sizeof(cache->entries[oldest_idx].value));
    cache->entries[oldest_idx].timestamp = time(NULL);
    cache->entries[oldest_idx].generation = s->env_generation;
    cache->entries[oldest_idx].valid = 1;
}

/* Invalidate expired cache entries */
void jash_var_cache_invalidate(jash_state_t* s) {
    if (!s || !s->var_cache) return;
    
    jash_var_cache_t* cache = (jash_var_cache_t*)s->var_cache;
    time_t now = time(NULL);
    
    for (int i = 0; i < JASH_VAR_CACHE_SIZE; i++) {
        if (cache->entries[i].valid && now - cache->entries[i].timestamp > 30) {
            cache->entries[i].valid = 0;
        }
    }
}
