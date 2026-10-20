// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/gold — the Builtin Renaissance shared contract (ADR-0044 Wave A0)
//
// See gold.h for the doctrine. This file is deliberately thin: each function is
// a facade over an existing in-process ledger (output_budget, salience,
// agent_json) or the generalized snapshot store lifted from ls --delta. No new
// subsystem — the superpower is process-shared ledgers, and this is the one
// place every gold builtin reaches them, so the contract is uniform.

#define _POSIX_C_SOURCE 200809L
#include "gold.h"
#include <core/output_budget.h>
#include <core/salience.h>
#include <core/agent_json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "gold_delta_checked.inc"

/* ── The gate (evaluated once, cached — one branch on the hot path) ───────── */

int jash_gold_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* g = getenv("JASH_GOLD");
        cached = (g && g[0] == '0' && g[1] == '\0') ? 1 : 0;
    }
    return cached;
}

int jash_gold_active(void) {
    static int cached = -1;
    if (cached < 0) {
        if (jash_gold_disabled()) { cached = 0; }
        else {
            /* Agent detected: explicit JASH_AGENT, Claude Code, or any session
             * id present (a bare interactive human has none of these). */
            int agent = jash_agent_enabled()
                     || (getenv("CLAUDECODE") != NULL)
                     || (getenv("CLAUDE_CODE_SESSION_ID") != NULL)
                     || (getenv("JASH_SESSION") != NULL);
            cached = agent ? 1 : 0;
        }
    }
    return cached;
}

/* ── Session identity + stable keys ───────────────────────────────────────── */

const char* jash_gold_session(void) {
    const char* s = getenv("CLAUDE_CODE_SESSION_ID");
    if (s && *s) return s;
    s = getenv("JASH_SESSION");
    if (s && *s) return s;
    return "nosession";
}

void jash_gold_key(const char* a, const char* b, char* out, size_t cap) {
    unsigned int h = 2166136261u;
    for (const char* p = a; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= (unsigned char)'|'; h *= 16777619u;
    for (const char* p = b; p && *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    snprintf(out, cap, "%08x", h);
}

/* ── Delta persistence (generalized from ls --delta) ──────────────────────── */

/* Legacy wrappers retain their signatures and baseline convention while
 * sharing the checked, byte-preserving store with structured consumers. */
int jash_gold_delta_load(const char* builtin, const char* key,
                         jash_gold_dent_t* prev, int max, long long* age_s) {
    if (max < 0) return gold_checked_fail(EINVAL);
    jash_gold_delta_observation_t observed;
    if (jash_gold_delta_load_checked(builtin, key, prev, (size_t)max, &observed)) return -1;
    if (!observed.present) return gold_checked_fail(ENOENT);
    if (age_s) *age_s = (long long)observed.age_seconds;
    return (int)observed.count;
}

void jash_gold_delta_store(const char* builtin, const char* key,
                           const jash_gold_dent_t* cur, int n) {
    if (n < 0 || (unsigned)n > GOLD_CHECKED_ITEMS) { errno = E2BIG; return; }
    jash_gold_dent_t* previous = calloc(GOLD_CHECKED_ITEMS, sizeof(*previous));
    if (!previous) return;
    jash_gold_delta_observation_t observed;
    int status = jash_gold_delta_load_checked(builtin, key, previous, GOLD_CHECKED_ITEMS, &observed);
    free(previous);
    if (status) return;
    jash_gold_delta_publication_t publication;
    (void)jash_gold_delta_store_checked(builtin, key, cur, (size_t)n, &observed, NULL, NULL, &publication);
}

void jash_gold_age(long long age_s, char* out, size_t cap) {
    long long d = age_s < 0 ? 0 : age_s;
    if (d < 5)           snprintf(out, cap, "just now");
    else if (d < 90)     snprintf(out, cap, "%llds ago", d);
    else if (d < 5400)   snprintf(out, cap, "%lldm ago", d / 60);
    else if (d < 129600) snprintf(out, cap, "%lldh ago", d / 3600);
    else                 snprintf(out, cap, "%lldd ago", d / 86400);
}

/* ── Budget-cap + envelope facades ────────────────────────────────────────── */

long jash_gold_budget(void) { return jash_budget_limit(); }

int jash_gold_flush_file(const char* path, const char* cmdname,
                         jash_writer_t w, void* ctx) {
    return jash_salience_flush(path, jash_budget_limit(), cmdname, 1, w, ctx);
}

void jash_gold_json_esc(const char* s, char* out, size_t cap) {
    jash_json_esc(s, out, cap);
}
