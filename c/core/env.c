// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 The author makes music. Hear F E R M I ∞ H A R T wherever you stream.
//    No obligation. Just a wish.
//
// Module: core/env — environment variable table (implementation)
//
// Ported from user/jash-legacy/main.c env_* (manifesto §4 subsumption).

#include "env.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>   /* confstr(_CS_PATH) — system default PATH */
// Improvements over legacy:
//   - State passed by pointer (no file-static globals).
//   - Bounded copies with explicit limits in core/state.h.
//   - Insertion-order iteration via jash_env_at().
//   - No reliance on size_t string-API (uses raw byte loops).

#include "env.h"

/* FNV-1a hash — fast, good distribution, public domain */
static uint32_t env_hash_fnv1a(const char* key) {
    uint32_t hash = 2166136261u;
    while (*key) {
        hash ^= (unsigned char)*key++;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t env_hash_prefix(const char* key, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)key[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ── Native call frames (perf W12) ────────────────────────────────────
 *
 * Function depth + FUNCNAME live in jash_state_t while frames are active,
 * instead of ~15 env round-trips per call. Rules:
 * - Reads (jash_env_get) prefer native state when depth>0.
 * - Frame enter/exit touch native state only (+1 generation bump each).
 * - Every other write path (user code, builtins) is write-through: table
 *   AND native, so the two never diverge observably.
 * - Table listings and child env are materialized by jash_env_sync_frame
 *   (explicit call sites: env/declare/export/typeset/set/compgen dumps,
 *   execve envp builds). Exit restores the enter-time snapshot, so even
 *   synced-then-popped frames leave the table exactly as found.
 * - `__FUNC_DEPTH__` assigned by user code goes table-only (an integer
 *   counter cannot echo back arbitrary strings); reads still prefer the
 *   native counter while frames are active. Pathological either way;
 *   no test or builtin assigns it.
 * - Positional params and `local` scope stay fully table-managed. */

/* 0 = ordinary key, 1 = __FUNC_DEPTH__, 2 = FUNCNAME, 3 = FUNCNAME[n<32]. */
static int frame_key_kind(const char* key, unsigned* idx_out) {
    if (!key) return 0;
    if (key[0] == '_' && key[1] == '_' && key[2] == 'F') {
        if (strcmp(key, "__FUNC_DEPTH__") == 0) return 1;
        return 0;
    }
    if (key[0] != 'F') return 0;
    if (strcmp(key, "FUNCNAME") == 0) return 2;
    if (strncmp(key, "FUNCNAME[", 9) != 0) return 0;
    const char* p = key + 9;
    if (*p < '0' || *p > '9') return 0;
    unsigned n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10u + (unsigned)(*p - '0');
        if (n >= JASH_FRAME_MAX) return 0;
        p++;
    }
    if (*p != ']' || p[1] != '\0') return 0;
    if (idx_out) *idx_out = n;
    return 3;
}

/* Local helpers — keep linkage internal. */

static int env_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == *b);
}

/* Recognize exactly the shape aggregate expansion accepts: a non-empty base,
 * '[', a non-empty subscript, and a final ']'. */
static int env_array_key_base(const char* key, size_t* base_len) {
    if (!key || !key[0]) return 0;
    const char* open = strchr(key, '[');
    if (!open || open == key || open[1] == '\0' || open[1] == ']') return 0;
    size_t len = strlen(key);
    if (len < 3 || key[len - 1] != ']') return 0;
    *base_len = (size_t)(open - key);
    return 1;
}

static int env_array_base_matches(const char* key, const char* base,
                                  size_t base_len) {
    size_t key_base_len = 0;
    return env_array_key_base(key, &key_base_len) &&
           key_base_len == base_len &&
           strncmp(key, base, base_len) == 0;
}

void jash_env_array_index_init(jash_state_t* s) {
    if (!s) return;
    for (int i = 0; i < JASH_ENV_ARRAY_HASH_SIZE; i++)
        s->env_array_heads[i] = -1;
}

static void env_array_link(jash_state_t* s, uint32_t env_idx) {
    size_t base_len = 0;
    jash_env_entry_t* e = &s->env[env_idx];
    e->array_next = -1;
    if (!env_array_key_base(e->key, &base_len)) return;
    uint32_t bucket = env_hash_prefix(e->key, base_len) %
                      JASH_ENV_ARRAY_HASH_SIZE;
    e->array_next = s->env_array_heads[bucket];
    s->env_array_heads[bucket] = (int32_t)env_idx;
}

static void env_array_unlink(jash_state_t* s, uint32_t env_idx) {
    size_t base_len = 0;
    jash_env_entry_t* e = &s->env[env_idx];
    if (!env_array_key_base(e->key, &base_len)) {
        e->array_next = -1;
        return;
    }
    uint32_t bucket = env_hash_prefix(e->key, base_len) %
                      JASH_ENV_ARRAY_HASH_SIZE;
    int32_t cur = s->env_array_heads[bucket];
    int32_t prev = -1;
    while (cur >= 0) {
        if ((uint32_t)cur == env_idx) {
            if (prev < 0) s->env_array_heads[bucket] = e->array_next;
            else s->env[prev].array_next = e->array_next;
            e->array_next = -1;
            return;
        }
        prev = cur;
        cur = s->env[cur].array_next;
    }
    e->array_next = -1;
}

void jash_env_array_index_rebuild(jash_state_t* s) {
    if (!s) return;
    jash_env_array_index_init(s);
    for (uint32_t i = 0; i < s->env_count; i++) env_array_link(s, i);
}

void jash_env_array_iter_init(jash_env_array_iter_t* it,
                              const jash_state_t* s, const char* base) {
    if (!it) return;
    it->state = s;
    it->base = base;
    it->base_len = base ? strlen(base) : 0;
    it->probes = 0;
    if (!s || !base || !base[0]) {
        it->next = -1;
        return;
    }
    uint32_t bucket = env_hash_prefix(base, it->base_len) %
                      JASH_ENV_ARRAY_HASH_SIZE;
    it->next = s->env_array_heads[bucket];
}

int jash_env_array_iter_next(jash_env_array_iter_t* it) {
    if (!it || !it->state) return -1;
    while (it->next >= 0) {
        int32_t idx = it->next;
        if ((uint32_t)idx >= it->state->env_count) {
            it->next = -1;
            return -1;
        }
        it->next = it->state->env[idx].array_next;
        it->probes++;
        if (env_array_base_matches(it->state->env[idx].key, it->base,
                                   it->base_len))
            return idx;
    }
    return -1;
}

static int env_is_positional_key(const char* key) {
    if (!key || !*key) return 0;
    int value = 0;
    for (const char* p = key; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        value = value * 10 + (*p - '0');
        if (value > JASH_POS_MAX) return 0;
    }
    return value >= 1;
}

/* Single pass over the env table counting the positional parameters
 * ($1..$N) currently set and the highest set index. Equivalent to probing
 * 1..JASH_POS_MAX with jash_env_get, but O(env_count) with no per-index
 * snprintf + hash lookup. Gaps (e.g. after `unset 2`) are counted exactly
 * like the old probing loops did. */
static void env_positional_scan(const jash_state_t* s, int* count, int* max_idx) {
    int n = 0, hi = 0;
    for (uint32_t i = 0; i < s->env_count; i++) {
        const char* k = s->env[i].key;
        if (k[0] < '1' || k[0] > '9') continue;
        int v = 0;
        const char* p = k;
        for (; *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (*p - '0');
            if (v > JASH_POS_MAX) { v = 0; break; }
        }
        if (v < 1 || *p != '\0') continue;
        n++;
        if (v > hi) hi = v;
    }
    *count = n;
    *max_idx = hi;
}

/* Bounded positional key writer ("1".."384"), no snprintf truncation risk. */
static void env_positional_key(char* buf, int val) {
    int ki = 0;
    if (val >= 100) buf[ki++] = (char)('0' + val / 100);
    if (val >= 10)  buf[ki++] = (char)('0' + (val / 10) % 10);
    buf[ki++] = (char)('0' + val % 10);
    buf[ki] = 0;
}

static int env_find_index(const jash_state_t* s, const char* key) {
    /* O(1) hash table lookup with linear probing */
    uint32_t h = env_hash_fnv1a(key);
    uint32_t bucket = h % JASH_ENV_HASH_SIZE;
    
    /* Linear probing: check bucket and neighbors */
    for (uint32_t probe = 0; probe < JASH_ENV_HASH_SIZE; probe++) {
        uint32_t idx = (bucket + probe) % JASH_ENV_HASH_SIZE;
        int32_t slot = s->env_hash[idx];
        if (slot < 0) return -1;  /* empty bucket = not found */
        if (s->env[slot].hash == h && env_streq(s->env[slot].key, key)) {
            return slot;
        }
    }
    return -1;  /* table full (shouldn't happen with 64 buckets, 128 entries) */
}

/* Insert hash entry into bucket table */
static void env_hash_insert(jash_state_t* s, uint32_t env_idx) {
    uint32_t h = s->env[env_idx].hash;
    uint32_t bucket = h % JASH_ENV_HASH_SIZE;
    
    for (uint32_t probe = 0; probe < JASH_ENV_HASH_SIZE; probe++) {
        uint32_t idx = (bucket + probe) % JASH_ENV_HASH_SIZE;
        if (s->env_hash[idx] < 0) {
            s->env_hash[idx] = (int32_t)env_idx;
            return;
        }
    }
}

/* Remove hash entry and rehash displaced entries */
static void env_hash_remove(jash_state_t* s, uint32_t env_idx) {
    uint32_t h = s->env[env_idx].hash;
    uint32_t bucket = h % JASH_ENV_HASH_SIZE;
    
    /* Find and remove the entry */
    uint32_t remove_idx = UINT32_MAX;
    for (uint32_t probe = 0; probe < JASH_ENV_HASH_SIZE; probe++) {
        uint32_t idx = (bucket + probe) % JASH_ENV_HASH_SIZE;
        if (s->env_hash[idx] == (int32_t)env_idx) {
            s->env_hash[idx] = -1;
            remove_idx = idx;
            break;
        }
    }
    
    if (remove_idx == UINT32_MAX) return;
    
    /* Rehash displaced entries (linear probing cleanup) */
    uint32_t idx = (remove_idx + 1) % JASH_ENV_HASH_SIZE;
    while (s->env_hash[idx] >= 0) {
        int32_t displaced = s->env_hash[idx];
        s->env_hash[idx] = -1;
        env_hash_insert(s, (uint32_t)displaced);
        idx = (idx + 1) % JASH_ENV_HASH_SIZE;
    }
}

/* Bounded copy. Returns number of bytes written (excluding NUL).
 * Always NUL-terminates as long as dst_max > 0. */
static uint32_t env_copy_bounded(char* dst, uint32_t dst_max, const char* src) {
    if (dst_max == 0) return 0;
    uint32_t i = 0;
    while (src[i] && i < dst_max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/* ---- value helpers ---- */

/* Write val into an env entry using inline buffer for short values,
 * heap for long ones. Reuse identical heap values; otherwise replace storage. */
static int env_entry_set_val(jash_env_entry_t* e, const char* val) {
    size_t vlen = strlen(val);
    if (vlen < JASH_ENV_VAL_INLINE) {
        if (e->heap_val) { free(e->heap_val); e->heap_val = NULL; }
        memcpy(e->inline_val, val, vlen + 1);
        return 0;
    } else {
        /* The caller still advances generation and repairs the libc mirror. */
        if (e->heap_val && strcmp(e->heap_val, val) == 0) return 0;
        char* newbuf = (char*)malloc(vlen + 1);
        if (!newbuf) return -1;
        memcpy(newbuf, val, vlen + 1);
        if (e->heap_val) free(e->heap_val);
        e->heap_val = newbuf;
        return 0;
    }
}

/* Free heap storage for an entry (call before zeroing/reusing). */
static void env_entry_free_val(jash_env_entry_t* e) {
    if (e->heap_val) { free(e->heap_val); e->heap_val = NULL; }
}

/* ═══ JINV-126 — the shell was excluded from its own environment ═════════
 *
 * jash's contract is that the var table IS the child's environment: `V=1;
 * sh -c 'echo $V'` prints 1 with no `export` (see builtins/export.c). The
 * table was materialized into `envp` at exec time only — so children saw
 * every assignment and the shell's OWN process never did. `environ` stayed
 * frozen at whatever jash was launched with, and every `getenv()` inside
 * this process read that frozen copy.
 *
 * That silently broke 62 internal knobs. They are documented escape
 * hatches; several are printed by the banners that tell an agent how to
 * turn a layer off:
 *
 *     export JASH_MEMO=0     # banner says "JASH_MEMO=0 re-prints"
 *     wc -l f                # ...still collapsed. The override no-ops.
 *
 * The agent then concludes the FILE is unchanged rather than that its
 * override was ignored — a confident wrong answer, which is the failure
 * mode this shell exists to prevent.
 *
 * The direction matters for security. `export JASH_GUARD_BYPASS=1` failing
 * to take effect fails SAFE. But `export JASH_GUARD_STRICT=1`,
 * `JASH_MODULE_REQUIRE_HASH=1`, `JASH_CLAIM_STRICT=1` — a session trying to
 * HARDEN itself — failed OPEN, with no error and a shell that reported the
 * hardening as applied. Silent non-enforcement of a requested control is
 * strictly worse than refusing it.
 *
 * Fix: mirror the table into the real `environ` at the three mutation
 * points, so the process agrees with itself. The table remains the single
 * source of truth; `environ` is a mirror kept for `getenv()` (ours, libc's,
 * and any dlopened module's — modules link their own libc calls and could
 * not see the table at all).
 *
 * Mirroring is best-effort: on setenv() failure (ENOMEM / EINVAL for a key
 * containing '=') the assignment still succeeds, because the table — not
 * environ — is what jash executes from. A failed mirror can only cost a
 * knob its runtime override, never correctness of the command.
 *
 * Gate: tests/test_jinv126_env_mirror.sh */
static int env_mirror_set(const char* key, const char* val) {
    /* Skip the syscall-free common case: value already identical. getenv is
     * a pointer walk; setenv mallocs and rewrites environ. Hot assignment
     * loops that re-set the same value pay nothing. */
    const char* cur = getenv(key);
    if (cur && strcmp(cur, val) == 0) return 0;
    return setenv(key, val, 1);
}

static void env_mirror_unset(const char* key) {
    (void)unsetenv(key);
}

/* ---- public API ---- */


/* Table index n maps to stack slot depth-1-n: [0] is the innermost
 * frame, matching the table convention the shift loops implemented. */
static const char* frame_slot(const jash_state_t* s, unsigned n) {
    if (n >= (unsigned)s->func_depth) return NULL;
    return s->funcname_stack[s->func_depth - 1 - (int)n];
}

const char* jash_env_get(const jash_state_t* s, const char* key) {
    if (!s || !key) return 0;
    if (s->func_depth > 0) {
        unsigned fidx = 0;
        int fk = frame_key_kind(key, &fidx);
        if (fk == 1) return s->func_depth_str;
        if (fk == 2 || fk == 3) {
            const char* v = frame_slot(s, fk == 2 ? 0u : fidx);
            if (v) return v;
        }
    }
    int idx = env_find_index(s, key);
    return idx >= 0 ? JASH_ENV_VAL(&s->env[idx]) : 0;
}

static int env_set_impl(jash_state_t* s, const char* key, const char* val,
                        int mirror) {
    if (!s || !key || !val) return -2;
    if (!key[0]) return -2;

    int idx = env_find_index(s, key);
    if (idx < 0) {
        if (s->env_count >= JASH_MAX_ENV) return -1;
        if (!env_is_positional_key(key) && s->env_reserved_slots > 0 &&
            s->env_count >= JASH_MAX_ENV - s->env_reserved_slots)
            return -1;
        idx = (int)s->env_count;
        env_copy_bounded(s->env[idx].key, JASH_ENV_KEY_LEN, key);
        s->env[idx].hash = env_hash_fnv1a(key);
        s->env[idx].heap_val = NULL;
        s->env[idx].array_next = -1;
        if (env_entry_set_val(&s->env[idx], val) != 0) return -1;
        s->env_count++;
        env_hash_insert(s, (uint32_t)idx);
        env_array_link(s, (uint32_t)idx);
    } else if (strcmp(JASH_ENV_VAL(&s->env[idx]), val) == 0) {
        /* Same-value rewrite: skip the store (no memcpy, no free/malloc
         * churn) but keep the generation bump and the mirror repair —
         * both are pinned contract (tests/test_perf_env_fschange.c):
         * the bump keeps "every set invalidates" true for var_cache,
         * and the mirror heals an externally-tampered environ. */
        s->env_generation++;
        if (mirror) (void)env_mirror_set(key, val);
        return 0;
    } else if (env_entry_set_val(&s->env[idx], val) != 0) {
        return -1;
    }
    s->env_generation++;
    if (mirror) (void)env_mirror_set(key, val);   /* JINV-126 */
    return 0;
}

/* Adopt a user write into native frame state (write-through: the table
 * write above already happened). Keeps native reads exact after explicit
 * user assignments inside a frame. */
static void frame_slot_set(jash_state_t* s, unsigned n, const char* val) {
    int slot = s->func_depth - 1 - (int)n;
    if (slot < 0 || slot >= JASH_FRAME_MAX) return;
    if (val) {
        char* copy = strdup(val);
        if (!copy) return;
        free(s->funcname_stack[slot]);
        s->funcname_stack[slot] = copy;
    } else {
        free(s->funcname_stack[slot]);
        s->funcname_stack[slot] = NULL;
    }
}

static void frame_adopt_set(jash_state_t* s, const char* key, const char* val) {
    if (s->func_depth <= 0 || !val) return;
    unsigned fidx = 0;
    int fk = frame_key_kind(key, &fidx);
    if (fk == 2) frame_slot_set(s, 0u, val);
    else if (fk == 3 && fidx < (unsigned)s->func_depth)
        frame_slot_set(s, fidx, val);
    /* kind 1 (__FUNC_DEPTH__): table-only by design (see above). Larger
     * indices have no native slot: table-only on both paths. */
}

int jash_env_set(jash_state_t* s, const char* key, const char* val) {
    int rc = env_set_impl(s, key, val, 1);
    if (rc == 0) frame_adopt_set(s, key, val);
    return rc;
}

int jash_env_set_nomirror(jash_state_t* s, const char* key, const char* val) {
    return env_set_impl(s, key, val, 0);
}

int jash_env_apply_batch(jash_state_t* s, const char* const* keys,
                         const char* const* values, int count) {
    if (!s || count < 0 || (count && (!keys || !values))) return -1;
    jash_state_t* candidate = (jash_state_t*)calloc(1, sizeof(*candidate));
    if (!candidate) return -1;
    for (int i = 0; i < JASH_ENV_HASH_SIZE; i++) candidate->env_hash[i] = -1;
    jash_env_array_index_init(candidate);
    candidate->env_reserved_slots = s->env_reserved_slots;

    for (uint32_t i = 0; i < s->env_count; i++) {
        if (env_set_impl(candidate, s->env[i].key,
                         JASH_ENV_VAL(&s->env[i]), 0) != 0)
            goto fail;
    }
    for (int i = 0; i < count; i++) {
        if (env_set_impl(candidate, keys[i], values[i], 0) != 0)
            goto fail;
    }

    /* Update libc's mirror while both old and candidate tables are intact.
     * Failure leaves the authoritative table untouched and restores the
     * best-effort mirror to its prior values. */
    for (uint32_t i = 0; i < candidate->env_count; i++) {
        if (env_mirror_set(candidate->env[i].key,
                           JASH_ENV_VAL(&candidate->env[i])) != 0) {
            for (uint32_t j = 0; j < s->env_count; j++)
                (void)env_mirror_set(s->env[j].key, JASH_ENV_VAL(&s->env[j]));
            for (uint32_t j = 0; j < candidate->env_count; j++)
                if (env_find_index(s, candidate->env[j].key) < 0)
                    env_mirror_unset(candidate->env[j].key);
            goto fail;
        }
    }

    for (uint32_t i = 0; i < s->env_count; i++) env_entry_free_val(&s->env[i]);
    memcpy(s->env, candidate->env, sizeof s->env);
    memcpy(s->env_hash, candidate->env_hash, sizeof s->env_hash);
    memcpy(s->env_array_heads, candidate->env_array_heads,
           sizeof s->env_array_heads);
    s->env_count = candidate->env_count;
    s->env_generation++;
    for (uint32_t i = 0; i < candidate->env_count; i++)
        candidate->env[i].heap_val = NULL;
    free(candidate);
    return 0;

fail:
    for (uint32_t i = 0; i < candidate->env_count; i++)
        env_entry_free_val(&candidate->env[i]);
    free(candidate);
    return -1;
}

int jash_env_replace_positionals(jash_state_t* s,
                                 const char* const* values, int count) {
    if (!s || count < 0 || count > JASH_POS_MAX) return -1;

    int old_count = 0, old_max = 0;
    env_positional_scan(s, &old_count, &old_max);
    if (s->env_count - (uint32_t)old_count + (uint32_t)count > JASH_MAX_ENV)
        return -1;

    char* prepared_small[16] = {0};
    char** prepared = prepared_small;
    if ((size_t)count > sizeof(prepared_small) / sizeof(prepared_small[0])) {
        prepared = (char**)calloc((size_t)count, sizeof(char*));
        if (!prepared) return -1;
    }
    for (int i = 0; i < count; i++) {
        const char* value = values && values[i] ? values[i] : "";
        size_t len = strlen(value);
        if (len >= JASH_ENV_VAL_INLINE) {
            prepared[i] = (char*)malloc(len + 1);
            if (!prepared[i]) {
                for (int j = 0; j < i; j++) free(prepared[j]);
                if (prepared != prepared_small) free(prepared);
                return -1;
            }
            memcpy(prepared[i], value, len + 1);
        }
    }

    /* Clear only up to the highest positional actually set (gaps included,
     * since `unset N` can punch holes in 1..old_max). */
    for (int i = 1; i <= old_max; i++) {
        char key[8]; env_positional_key(key, i);
        jash_env_unset(s, key);
    }
    for (int i = 0; i < count; i++) {
        char key[8]; env_positional_key(key, i + 1);
        const char* value = values && values[i] ? values[i] : "";
        int idx = (int)s->env_count++;
        env_copy_bounded(s->env[idx].key, JASH_ENV_KEY_LEN, key);
        s->env[idx].hash = env_hash_fnv1a(key);
        s->env[idx].heap_val = prepared[i];
        s->env[idx].array_next = -1;
        if (prepared[i]) s->env[idx].inline_val[0] = 0;
        else memcpy(s->env[idx].inline_val, value, strlen(value) + 1);
        env_hash_insert(s, (uint32_t)idx);
        env_mirror_set(key, value);
    }
    s->env_generation++;
    if (prepared != prepared_small) free(prepared);
    return 0;
}

int jash_env_positional_count(const jash_state_t* s,
                                      int* count, int* max_idx) {
    if (!s || !count || !max_idx) return -1;
    env_positional_scan(s, count, max_idx);
    return 0;
}

static int replace_positionals_owned_impl(jash_state_t* s,
                                          char** values, int count,
                                          int old_count, int old_max) {
    if (s->env_count - (uint32_t)old_count + (uint32_t)count > JASH_MAX_ENV)
        return -1;
    for (int i = 0; i < count; i++) if (!values[i]) return -1;

    /* Clear only up to the highest positional actually set. */
    for (int i = 1; i <= old_max; i++) {
        char key[8]; env_positional_key(key, i);
        jash_env_unset(s, key);
    }
    for (int i = 0; i < count; i++) {
        char key[8]; env_positional_key(key, i + 1);
        int idx = (int)s->env_count++;
        env_copy_bounded(s->env[idx].key, JASH_ENV_KEY_LEN, key);
        s->env[idx].hash = env_hash_fnv1a(key);
        s->env[idx].heap_val = values[i];
        s->env[idx].array_next = -1;
        s->env[idx].inline_val[0] = 0;
        env_hash_insert(s, (uint32_t)idx);
        env_mirror_set(key, values[i]);
        values[i] = NULL;
    }
    s->env_generation++;
    return 0;
}

int jash_env_replace_positionals_owned(jash_state_t* s,
                                       char** values, int count) {
    if (!s || !values || count < 0 || count > JASH_POS_MAX) return -1;
    int old_count = 0, old_max = 0;
    env_positional_scan(s, &old_count, &old_max);
    return replace_positionals_owned_impl(s, values, count,
                                          old_count, old_max);
}

int jash_env_replace_positionals_owned_known(jash_state_t* s,
                                             char** values, int count,
                                             int old_count, int old_max) {
    if (!s || !values || count < 0 || count > JASH_POS_MAX) return -1;
    if (old_count < 0 || old_max < 0 || old_max > JASH_POS_MAX) return -1;
    return replace_positionals_owned_impl(s, values, count,
                                          old_count, old_max);
}

/* Append suffix to an existing env var in-place.
 * Returns 0 on success, -1 if key not found, -2 on NULL input. */
int jash_env_append(jash_state_t* s, const char* key, const char* suffix) {
    if (!s || !key || !suffix) return -2;
    int idx = env_find_index(s, key);
    if (idx < 0) return -1;
    jash_env_entry_t* e = &s->env[idx];
    const char* cur = JASH_ENV_VAL(e);
    size_t curlen = strlen(cur);
    size_t suflen = strlen(suffix);
    size_t newlen = curlen + suflen;
    if (newlen < JASH_ENV_VAL_INLINE) {
        if (e->heap_val) { free(e->heap_val); e->heap_val = NULL; }
        memcpy(e->inline_val + curlen, suffix, suflen + 1);
    } else {
        char* newbuf = (char*)malloc(newlen + 1);
        if (!newbuf) return -1;
        memcpy(newbuf, cur, curlen);
        memcpy(newbuf + curlen, suffix, suflen + 1);
        if (e->heap_val) free(e->heap_val);
        e->heap_val = newbuf;
        e->inline_val[0] = '\0';
    }
    s->env_generation++;
    env_mirror_set(key, JASH_ENV_VAL(e));   /* JINV-126 */
    return 0;
}

static int env_unset_impl(jash_state_t* s, const char* key, int mirror) {
    if (!s || !key) return -1;
    int idx = env_find_index(s, key);
    if (idx < 0) return -1;

    /* Free heap allocation of the removed slot first */
    env_array_unlink(s, (uint32_t)idx);
    env_entry_free_val(&s->env[idx]);

    /* Remove from hash table */
    env_hash_remove(s, (uint32_t)idx);

    /* Swap-with-last to compact */
    uint32_t last = s->env_count - 1;
    if ((uint32_t)idx != last) {
        env_array_unlink(s, last);
        env_hash_remove(s, last);
        for (uint32_t i = 0; i < JASH_ENV_KEY_LEN; i++)
            s->env[idx].key[i] = s->env[last].key[i];
        for (uint32_t i = 0; i < JASH_ENV_VAL_INLINE; i++)
            s->env[idx].inline_val[i] = s->env[last].inline_val[i];
        /* Transfer heap ownership — do NOT free, just move the pointer */
        s->env[idx].heap_val = s->env[last].heap_val;
        s->env[last].heap_val = NULL;
        s->env[idx].hash = s->env[last].hash;
        s->env[idx].array_next = -1;
        env_hash_insert(s, (uint32_t)idx);
        env_array_link(s, (uint32_t)idx);
    }

    /* Zero the abandoned slot */
    s->env[last].key[0] = '\0';
    s->env[last].inline_val[0] = '\0';
    s->env[last].heap_val = NULL;
    s->env[last].hash = 0;
    s->env[last].array_next = -1;
    s->env_count--;
    s->env_generation++;
    if (mirror) env_mirror_unset(key);   /* JINV-126 */
    return 0;
}

static void frame_adopt_unset(jash_state_t* s, const char* key) {
    if (s->func_depth <= 0) return;
    unsigned fidx = 0;
    int fk = frame_key_kind(key, &fidx);
    if (fk == 2) {
        /* Bare `unset FUNCNAME` also sweeps every indexed slot in the
         * old table world (the builtin's prefix sweep); clear the whole
         * native stack to match. */
        for (int i = 0; i < s->func_depth && i < JASH_FRAME_MAX; i++) {
            free(s->funcname_stack[i]);
            s->funcname_stack[i] = NULL;
        }
    } else if (fk == 3 && fidx < (unsigned)s->func_depth) {
        frame_slot_set(s, fidx, NULL);
    }
}

int jash_env_unset(jash_state_t* s, const char* key) {
    /* Adopt first: the table removal below may miss (absent key) while
     * native state still shadows it — the user intent is what counts. */
    if (s && s->func_depth > 0) frame_adopt_unset(s, key);
    return env_unset_impl(s, key, 1);
}

int jash_env_unset_nomirror(jash_state_t* s, const char* key) {
    return env_unset_impl(s, key, 0);
}

/* Snapshot one table slot into a frame shadow (strdup only when the
 * key exists; a failed strdup fails the whole enter loudly, never
 * silently drops a value that exit must restore). */
static int frame_shadow_take(jash_state_t* s, const char* key,
                             char** slot, int* present) {
    int idx = env_find_index(s, key);
    if (idx < 0) {
        *slot = NULL;
        *present = 0;
        return 0;
    }
    *slot = strdup(JASH_ENV_VAL(&s->env[idx]));
    if (!*slot) return -1;
    *present = 1;
    return 0;
}

static void frame_shadow_restore(jash_state_t* s, const char* key,
                                 char* slot, int present) {
    if (present) env_set_impl(s, key, slot, 1);
    else env_unset_impl(s, key, 1);
    free(slot);
}

static void frame_shadow_free(jash_frame_t* fr) {
    free(fr->sh_name);
    fr->sh_name = NULL;
    if (fr->sh_idx) {
        for (int i = 0; i < fr->sh_n; i++) free(fr->sh_idx[i]);
        free(fr->sh_idx);
        fr->sh_idx = NULL;
    }
    fr->sh_n = 0;
    fr->sh_mask = 0;
}

/* Push a native call frame. Table is untouched (except snapshot reads);
 * depth/FUNCNAME reads are served natively from here on. */
int jash_frame_enter(jash_state_t* s, const char* name) {
    if (!s || !name || s->func_depth < 0 ||
        s->func_depth >= JASH_FRAME_MAX)
        return -1;
    jash_frame_t* fr = &s->frame_stack[s->func_depth];
    memset(fr, 0, sizeof(*fr));
    char* copy = strdup(name);
    if (!copy) return -1;
    /* Snapshot the indexed slots this frame's syncs may clobber:
     * 0..depth inclusive, since a sync writes every slot below the top
     * and the top after this push is `depth` itself. Covering only
     * < depth leaked the frame's own slot (live bug: FUNCNAME[0]=h
     * surviving a `local` that triggered a sync). */
    fr->sh_n = s->func_depth + 1;
    {
        /* Probe before allocating: top-level calls essentially never
         * shadow an indexed slot, so skip the calloc/free pair. */
        int need = 0;
        for (int i = 0; i < fr->sh_n && !need; i++) {
            char key[32];
            snprintf(key, sizeof(key), "FUNCNAME[%d]", i);
            if (env_find_index(s, key) >= 0) need = 1;
        }
        if (need) {
            fr->sh_idx = (char**)calloc((size_t)fr->sh_n, sizeof(char*));
            if (!fr->sh_idx) { free(copy); return -1; }
            for (int i = 0; i < fr->sh_n; i++) {
                char key[32];
                snprintf(key, sizeof(key), "FUNCNAME[%d]", i);
                int idx = env_find_index(s, key);
                if (idx >= 0) {
                    fr->sh_idx[i] = strdup(JASH_ENV_VAL(&s->env[idx]));
                    if (!fr->sh_idx[i]) {
                        free(copy);
                        frame_shadow_free(fr);
                        memset(fr, 0, sizeof(*fr));
                        return -1;
                    }
                    fr->sh_mask |= 1u << i;
                }
            }
        }
    }
    /* FUNCNAME exact key: snapshot like indexed. __FUNC_DEPTH__ needs no
     * shadow: exit overwrites it unconditionally (as the old code did). */
    if (frame_shadow_take(s, "FUNCNAME", &fr->sh_name,
                          &fr->sh_name_present) != 0) {
        free(copy);
        frame_shadow_free(fr);
        memset(fr, 0, sizeof(*fr));
        return -1;
    }
    s->funcname_stack[s->func_depth] = copy;
    s->func_depth++;
    snprintf(s->func_depth_str, sizeof(s->func_depth_str), "%d",
             s->func_depth);
    s->env_generation++;
    s->frame_dirty = 1;
    return 0;
}

/* Pop a native call frame, restoring any table slots a sync clobbered. */
void jash_frame_exit(jash_state_t* s) {
    if (!s || s->func_depth <= 0) return;
    s->func_depth--;
    snprintf(s->func_depth_str, sizeof(s->func_depth_str), "%d",
             s->func_depth);
    jash_frame_t* fr = &s->frame_stack[s->func_depth];
    free(s->funcname_stack[s->func_depth]);
    s->funcname_stack[s->func_depth] = NULL;
    for (int i = 0; i < fr->sh_n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "FUNCNAME[%d]", i);
        char* slot = fr->sh_idx ? fr->sh_idx[i] : NULL;
        if (slot && (fr->sh_mask & (1u << i)))
            frame_shadow_restore(s, key, slot, 1);
        else
            frame_shadow_restore(s, key, slot, 0);
    }
    free(fr->sh_idx);
    fr->sh_idx = NULL;
    frame_shadow_restore(s, "FUNCNAME", fr->sh_name, fr->sh_name_present);
    fr->sh_name = NULL;
    if (s->func_depth == 0) {
        /* Top-level exit restores the exact pre-session observable:
         * depth counter back to "0" (as the old table writes did). */
        env_set_impl(s, "__FUNC_DEPTH__", "0", 1);
        s->frame_dirty = 0;
    } else {
        s->frame_dirty = 1;
    }
    s->env_generation++;
    memset(fr, 0, sizeof(*fr));
}

/* Materialize native frame state into the table for whole-table readers
 * (dumps, execve envp). No-op unless a frame dirtied state. Bounded:
 * depth key + FUNCNAME slots below the top. */
void jash_env_sync_frame(jash_state_t* s) {
    if (!s) return;
    if (!s->frame_dirty || s->func_depth <= 0) {
        s->frame_dirty = 0;
        return;
    }
    env_set_impl(s, "__FUNC_DEPTH__", s->func_depth_str, 1);
    for (int i = 0; i < s->func_depth && i < JASH_FRAME_MAX; i++) {
        char key[32];
        snprintf(key, sizeof(key), "FUNCNAME[%d]", i);
        const char* v = frame_slot(s, (unsigned)i);
        if (v) env_set_impl(s, key, v, 1);
        else env_unset_impl(s, key, 1);
    }
    const char* top = frame_slot(s, 0u);
    if (top) env_set_impl(s, "FUNCNAME", top, 1);
    else env_unset_impl(s, "FUNCNAME", 1);
    s->frame_dirty = 0;
}

uint32_t jash_env_count(const jash_state_t* s) {
    if (!s) return 0;
    /* Whole-table readers (dumps, execve envp builds) must see native
     * frame state materialized. Single predictable branch when clean;
     * the sync itself runs only with active frames. */
    if (s->frame_dirty) jash_env_sync_frame((jash_state_t*)s);
    return s->env_count;
}

int jash_env_at(const jash_state_t* s, uint32_t i,
                const char** out_key, const char** out_val) {
    if (!s || i >= s->env_count) return 0;
    if (out_key) *out_key = s->env[i].key;
    if (out_val) *out_val = JASH_ENV_VAL(&s->env[i]);
    return 1;
}

void jash_env_seed_defaults(jash_state_t* s) {
    if (!s) return;

    /* Inherit the full parent environment. A prior whitelist (PATH/HOME/USER/
     * TERM/NO_COLOR/JASH_STATUSBAR/SHELL) dropped any other inherited var,
     * which silently breaks wrapper invocation patterns: `FOO=bar jash -c cmd`
     * would lose FOO entirely (jash_env_get has no getenv fallback). The copy
     * is O(n) over a typically <100-entry environ — trivial cost vs the
     * correctness regression. */
    extern char **environ;
    if (environ) {
        for (char **ep = environ; *ep; ep++) {
            const char *eq = *ep;
            const char *p = eq;
            while (*p && *p != '=') p++;
            if (*p != '=') continue;
            char key[JASH_ENV_KEY_LEN];
            size_t length = (size_t)(p - eq);
            if (!length || length >= sizeof key) {
                fputs("jash: inherited environment key exceeds table limit\n", stderr);
                continue;
            }
            memcpy(key, eq, length); key[length] = 0;
            /* The original value is already owned by libc. Avoid replacing
             * environ while walking it, and copy the entire value into the
             * table's heap-backed storage instead of a 4096-byte scratch. */
            if (env_set_impl(s, key, p + 1, 0))
                fputs("jash: inherited environment exceeds table capacity\n", stderr);
        }
    }

    /* BUG-021: $0 — the shell/program name. bash reports the shell name in -c
     * and interactive modes; script mode overrides this with the script path.
     * Without seeding it, `echo $0` printed an empty line. */
    if (!jash_env_get(s, "0")) jash_env_set(s, "0", "jash");

#if defined(__APPLE__)
    /* macOS: fold /etc/paths + /etc/paths.d entries into PATH (what bash/zsh do
     * via /etc/profile → `path_helper -s`). Terminal.app launches the shell
     * with a minimal PATH (`/usr/bin:/bin`), so without this brew (in
     * /usr/local/bin), opencode, and anything Homebrew/cargo/bun install
     * are invisible from interactive jash.
     *
     * Read the path files DIRECTLY rather than invoking path_helper — popen()
     * of /bin/sh + path_helper costs ~50ms per jash boot vs ~1ms for a few
     * fopen()s, and we'd lose the 6.36ms boot time we benchmarked against
     * bash. The format is trivially simple (one path per line, comments
     * starting with #). */
    {
        /* BUG-062: fold ONLY when the inherited PATH is the Terminal.app
         * minimal one (or absent) — the case this feature exists for. An
         * explicit richer PATH must be trusted VERBATIM: unconditionally
         * prepending system dirs made /usr/bin/cc shadow a cc earlier in
         * the caller's PATH, violating POSIX precedence for every agent,
         * venv, toolchain wrapper and node_modules/.bin on the machine
         * (bash/zsh only run path_helper for LOGIN shells; `bash -c`
         * never rewrites an inherited PATH). */
        const char* inherited = jash_env_get(s, "PATH");
        int need_fold = !inherited || !*inherited ||
                        strcmp(inherited, "/usr/bin:/bin") == 0;
        char merged_path[8192];
        merged_path[0] = '\0';
        FILE* pf = need_fold ? fopen("/etc/paths", "r") : NULL;
        if (pf) {
            char line[1024];
            while (fgets(line, sizeof(line), pf)) {
                size_t L = strlen(line);
                while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r' ||
                                 line[L-1] == ' ' || line[L-1] == '\t')) line[--L] = '\0';
                if (L == 0 || line[0] == '#') continue;
                if (merged_path[0]) strlcat(merged_path, ":", sizeof(merged_path));
                strlcat(merged_path, line, sizeof(merged_path));
            }
            fclose(pf);
        }
        /* Also fold /etc/paths.d entries (e.g. /etc/paths.d/10-cryptex). */
        DIR* pd = need_fold ? opendir("/etc/paths.d") : NULL;
        if (pd) {
            struct dirent* ent;
            while ((ent = readdir(pd))) {
                if (ent->d_name[0] == '.') continue;
                char fname[512];
                snprintf(fname, sizeof(fname), "/etc/paths.d/%s", ent->d_name);
                FILE* fp = fopen(fname, "r");
                if (!fp) continue;
                char line[1024];
                while (fgets(line, sizeof(line), fp)) {
                    size_t L = strlen(line);
                    while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r' ||
                                     line[L-1] == ' ' || line[L-1] == '\t')) line[--L] = '\0';
                    if (L == 0 || line[0] == '#') continue;
                    if (merged_path[0]) strlcat(merged_path, ":", sizeof(merged_path));
                    strlcat(merged_path, line, sizeof(merged_path));
                }
                fclose(fp);
            }
            closedir(pd);
        }
        if (merged_path[0]) {
            const char* old = jash_env_get(s, "PATH");
            if (old && *old) {
                strlcat(merged_path, ":", sizeof(merged_path));
                strlcat(merged_path, old, sizeof(merged_path));
            }
            jash_env_set(s, "PATH", merged_path);
        }
    }
#else
    /* Linux/BSD: the same problem, and it had NO answer here.
     *
     * A shell started without a PATH in its environment — a login shell, a
     * display manager, a systemd unit, `env -i`, a container entrypoint — got
     * an EMPTY $PATH from jash, where bash and sh both substitute a built-in
     * default. Measured on Ubuntu 24.04:
     *
     *   env -i /bin/bash -c 'echo $PATH'
     *     -> /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
     *   env -i jash      -c 'echo $PATH'
     *     -> (empty)
     *
     * The symptom is confusing rather than obvious: jash's own command lookup
     * still finds things in the system directories, so `bash`, `git` and `node`
     * launch fine and the shell looks healthy — while anything installed under
     * $HOME (~/.local/bin, ~/.opencode/bin, ~/.cargo/bin, ~/.bun/bin) is
     * invisible, and every child process inherits the empty PATH too.
     *
     * Linux has no /etc/paths. The system default lives in /etc/environment
     * (Debian/Ubuntu, read by PAM), with confstr(_CS_PATH) as the POSIX
     * fallback and a compiled-in list as the last resort — the same ladder
     * bash walks.
     *
     * As on the macOS side (BUG-062): only fill in when PATH is absent or
     * empty. An inherited PATH is trusted VERBATIM — prepending system
     * directories to an explicit PATH would shadow the caller's toolchain and
     * break POSIX precedence for every venv, wrapper and node_modules/.bin. */
    {
        const char* inherited = jash_env_get(s, "PATH");
        if (!inherited || !*inherited) {
            char def[4096];
            def[0] = '\0';

            FILE* ef = fopen("/etc/environment", "r");
            if (ef) {
                char line[2048];
                while (fgets(line, sizeof(line), ef)) {
                    const char* v = NULL;
                    if      (!strncmp(line, "PATH=",  5))  v = line + 5;
                    else if (!strncmp(line, "PATH =", 6))  v = line + 6;
                    if (!v) continue;
                    while (*v == ' ' || *v == '\t') v++;
                    if (*v == '"' || *v == '\'') v++;          /* PATH="..." */
                    strlcpy(def, v, sizeof(def));
                    size_t L = strlen(def);
                    while (L > 0 && (def[L-1] == '\n' || def[L-1] == '\r' ||
                                      def[L-1] == '"'  || def[L-1] == '\'' ||
                                      def[L-1] == ' '  || def[L-1] == '\t'))
                        def[--L] = '\0';
                    if (def[0]) break;
                }
                fclose(ef);
            }

            if (!def[0]) {
                size_t n = confstr(_CS_PATH, def, sizeof(def));
                if (n == 0 || n > sizeof(def)) def[0] = '\0';
            }
            if (!def[0])
                strlcpy(def,
                        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                        sizeof(def));

            jash_env_set(s, "PATH", def);
        }
    }
#endif

    /* Shell-specific overrides (only if not already set by inherit) */
    if (!jash_env_get(s, "SHELL"))        jash_env_set(s, "SHELL",        "/bin/jash");
#if defined(JASH_TARGET_BEYOND) && !defined(JASH_SURFACE_TERMIOS)
    /* Legacy projections without a terminal compatibility adapter cannot use ANSI
     * escape sequences or Unicode icons. TERM=dumb + NO_COLOR make
     * the canonical shell emit plain text that renders correctly. */
    jash_env_set(s, "TERM",         "dumb");
    jash_env_set(s, "NO_COLOR",     "1");
    jash_env_set(s, "COLORTERM",    "");
    jash_env_set(s, "JASH_STATUSBAR", "0");
#else
    /* INV-CONSOLE-004: real surface-persona backends keep normal terminal/theme
     * defaults; transport failures are not hidden by forcing NO_COLOR. */
    if (!jash_env_get(s, "TERM"))         jash_env_set(s, "TERM",         "xterm-256color");
    if (!jash_env_get(s, "COLORTERM"))    jash_env_set(s, "COLORTERM",    "truecolor");
#endif
    if (!jash_env_get(s, "JASH_VERSION")) jash_env_set(s, "JASH_VERSION", "0.1");
}
