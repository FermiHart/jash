// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/differentiators — single source of truth for differentiators
// (ADR-0040 W1). See differentiators.h for the contract.
//
// Semantics preserved from the pre-registry code on purpose:
//   NATIVE = exactly the 21 names guard.c counted as adoption (metrics must
//            not jump because of a refactor; widening is a W2+ decision).
//   UNIQUE = the union agent-guide already showed as "unique primitives".

#include "differentiators.h"

#include <string.h>

#define N_ JASH_DIFF_NATIVE
#define U_ JASH_DIFF_UNIQUE

static const jash_diff_t TABLE[] = {
    /* memory & context */
    {"recall",      "Memory",        "search attested command history and outcomes",            N_|U_},
    {"kv",          "Memory",        "persist key/value state across jash -c calls",            N_|U_},
    {"context",     "Memory",        "query-driven code digest: ask, map, fold, bundle",        N_|U_},
    {"digest",      "Memory",        "generate a cached project briefing for agents",           N_|U_},
    {"state",       "Memory",        "live system mirror: cwd, git, env, jobs in JSON",         N_|U_},
    {"brain",       "Memory",        "local Liquid Reasoning Engine: suggest/intent/impact/learn",  U_},
    {"goal",        "Memory",        "Intention Memory Caching: begin/step/verify/close/recall",    U_},
    {"llm-invoice", "Memory",        "token economics: measure tokens saved by jash mediation",     U_},
    {"graph",       "Memory",        "living graph: repo knowledge map that learns from attested usage", U_},
    {"mem",         "Memory",        "session memory distilled from the attested chain — verifiable, outcome-aware", U_},

    /* safety & undo */
    {"undo",        "Safety",        "revert the last destructive operation",                   N_|U_},
    {"redo",        "Safety",        "re-apply what undo just reverted",                        U_},
    {"timeline",    "Safety",        "show this session's mutation history",                    N_|U_},
    {"checkpoint",  "Safety",        "save a recoverable snapshot",                             N_|U_},
    {"rewind",      "Safety",        "restore a previous checkpoint",                           N_|U_},
    {"trash",       "Safety",        "recoverable delete (rm with undo)",                       N_|U_},
    {"speculate",   "Safety",        "run a command in a sandbox and preview the diff",         N_|U_},
    {"revert",      "Safety",        "cross-domain undo (git, docker, etc.)",                   N_|U_},

    /* proof & contracts */
    {"sigil",       "Proof",         "cryptographic attestation of every command",              N_|U_},
    {"verify",      "Proof",         "verify sigil chain / proofs",                             N_|U_},
    {"expect",      "Proof",         "assert rc/output before continuing",                      N_|U_},
    {"contract",    "Proof",         "run a command and enforce post-conditions",               N_|U_},
    {"gap",         "Proof",         "report capability gaps; fixes verified by attached proofs",   U_},

    /* cost & observability */
    {"cost",        "Observability", "estimate bytes/tokens/USD before/after a command",        N_|U_},
    {"dashboard",   "Observability", "single-view adoption, bypass, fleet, safety",                 U_},
    {"agent-guard", "Observability", "detect/block agents escaping to bash/sh/python",              U_},
    {"adopt",       "Observability", "adoption governor: level 0-3, scorecard, absorb envelope",    U_},

    /* structured CLIs */
    {"json",        "Structured",    "get git/npm/make/cmake output as JSON",                   N_|U_},

    /* coordination */
    {"mesh",        "Coordination",  "multi-model swarm task board",                            N_|U_},

    /* native/unique with no curated guide section (shown via live registry) */
    {"explain",     NULL, NULL, N_|U_},
    {"doctor",      NULL, NULL, N_|U_},
    {"intent",      NULL, NULL,    U_},
    {"budget",      NULL, NULL,    U_},
    {"module",      NULL, NULL,    U_},
    {"persist",     NULL, NULL,    U_},
    {"agent",       NULL, NULL,    U_},
    {"nudge",       NULL, NULL,    U_},
    {"spinner",     NULL, NULL,    U_},
    {"slm",         NULL, NULL,    U_},

    {NULL, NULL, NULL, 0}
};

#undef N_
#undef U_

static const char* const CATEGORIES[] = {
    "Memory", "Safety", "Proof", "Observability", "Structured", "Coordination",
    NULL
};

const jash_diff_t* jash_diff_table(size_t* count) {
    if (count) {
        size_t n = 0;
        while (TABLE[n].name) n++;
        *count = n;
    }
    return TABLE;
}

const jash_diff_t* jash_diff_find(const char* name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; TABLE[i].name; i++)
        if (strcmp(TABLE[i].name, name) == 0) return &TABLE[i];
    return NULL;
}

int jash_diff_is_native(const char* name) {
    const jash_diff_t* d = jash_diff_find(name);
    return d != NULL && (d->flags & JASH_DIFF_NATIVE) != 0;
}

int jash_diff_is_unique(const char* name) {
    const jash_diff_t* d = jash_diff_find(name);
    return d != NULL && (d->flags & JASH_DIFF_UNIQUE) != 0;
}

const char* const* jash_diff_categories(size_t* count) {
    if (count) {
        size_t n = 0;
        while (CATEGORIES[n]) n++;
        *count = n;
    }
    return CATEGORIES;
}

const char* jash_diff_brief(void) {
    return
"Superpowers already active for you (bash has none of these):\n"
"  recall did \"X\" | recall failed   attested memory of every command + outcome\n"
"  expect rc 0 | expect file PATH     assert results; wrong assumptions fail LOUD\n"
"  checkpoint / rewind auto [--scope]  undo anything — rm is auto-checkpointed\n"
"  undo [--dry] [id N] | redo        dry-run, selective revert, and re-apply\n"
"  explain \"CMD\"                      blast-radius preview WITHOUT running\n"
"  kv set/get | state | json git ...  structured state shared across -c calls\n";
}
