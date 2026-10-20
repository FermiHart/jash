// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/alias — alias table implementation
//
// The legacy jash had a 5-LOC stub for `alias`. We implement the table
// from scratch here following the same pattern as core/env: linear
// scan, swap-with-last compaction, no heap.

#include "alias.h"

static int alias_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int alias_find(const jash_state_t* s, const char* key) {
    for (uint32_t i = 0; i < s->alias_count; i++) {
        if (alias_streq(s->aliases[i].key, key)) return (int)i;
    }
    return -1;
}

static void alias_copy(char* dst, uint32_t cap, const char* src) {
    if (cap == 0) return;
    uint32_t i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int jash_alias_set(jash_state_t* s, const char* key, const char* val) {
    if (!s || !key || !val) return -2;
    if (!key[0]) return -2;

    int idx = alias_find(s, key);
    if (idx < 0) {
        if (s->alias_count >= JASH_ALIAS_MAX) return -1;
        idx = (int)s->alias_count++;
        alias_copy(s->aliases[idx].key, JASH_ALIAS_KEY_LEN, key);
    }
    alias_copy(s->aliases[idx].val, JASH_ALIAS_VAL_LEN, val);
    return 0;
}

const char* jash_alias_get(const jash_state_t* s, const char* key) {
    if (!s || !key) return 0;
    int idx = alias_find(s, key);
    return idx >= 0 ? s->aliases[idx].val : 0;
}

int jash_alias_unset(jash_state_t* s, const char* key) {
    if (!s || !key) return -1;
    int idx = alias_find(s, key);
    if (idx < 0) return -1;

    uint32_t last = s->alias_count - 1;
    if ((uint32_t)idx != last) {
        for (uint32_t i = 0; i < JASH_ALIAS_KEY_LEN; i++)
            s->aliases[idx].key[i] = s->aliases[last].key[i];
        for (uint32_t i = 0; i < JASH_ALIAS_VAL_LEN; i++)
            s->aliases[idx].val[i] = s->aliases[last].val[i];
    }
    s->aliases[last].key[0] = '\0';
    s->aliases[last].val[0] = '\0';
    s->alias_count--;
    return 0;
}

uint32_t jash_alias_count(const jash_state_t* s) {
    return s ? s->alias_count : 0;
}

int jash_alias_at(const jash_state_t* s, uint32_t i,
                  const char** out_key, const char** out_val) {
    if (!s || i >= s->alias_count) return 0;
    if (out_key) *out_key = s->aliases[i].key;
    if (out_val) *out_val = s->aliases[i].val;
    return 1;
}
