// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/stance — operational modes.
//
// See Docs/ADR-0035-context-engine-stances-cargo.md

#include "stance.h"
#include <core/context_engine.h>
#include <string.h>

/* ── Static State ──────────────────────────────────────────── */

static jash_stance_t g_current_stance = JASH_STANCE_EXPLORE;

/* ── Stance Configurations ─────────────────────────────────── */

static const jash_stance_cfg_t g_stance_configs[JASH_STANCE_COUNT] = {
    /* JASH_STANCE_EXPLORE — broad context, all providers, high budget */
    {
        .id = JASH_STANCE_EXPLORE,
        .name = "explore",
        .description = "broad context gathering, lots of history, all providers active",
        .context_budget = 8000,
        .digest_level = SESH_DEEP,
        .journal_priority_tags = {0},
        .n_priority_tags = 0
    },
    /* JASH_STANCE_BUILD — focused on file changes and build output */
    {
        .id = JASH_STANCE_BUILD,
        .name = "build",
        .description = "focused on file changes and build output, git diff prioritized",
        .context_budget = 6000,
        .digest_level = SESH_STANDARD,
        .journal_priority_tags = {0},
        .n_priority_tags = 0
    },
    /* JASH_STANCE_DEBUG — errors, failure patterns, recent failures */
    {
        .id = JASH_STANCE_DEBUG,
        .name = "debug",
        .description = "focused on errors, failure patterns, recent failed commands",
        .context_budget = 4000,
        .digest_level = SESH_BRIEF,
        .journal_priority_tags = {0},
        .n_priority_tags = 0
    },
    /* JASH_STANCE_REVIEW — git state, code review, diff context */
    {
        .id = JASH_STANCE_REVIEW,
        .name = "review",
        .description = "focused on git state, code review workflows, diff context",
        .context_budget = 5000,
        .digest_level = SESH_STANDARD,
        .journal_priority_tags = {0},
        .n_priority_tags = 0
    },
    /* JASH_STANCE_DEPLOY — deployment state, configuration, environment */
    {
        .id = JASH_STANCE_DEPLOY,
        .name = "deploy",
        .description = "focused on deployment state, configuration, environment",
        .context_budget = 4000,
        .digest_level = SESH_BRIEF,
        .journal_priority_tags = {0},
        .n_priority_tags = 0
    }
};

/* ── API ───────────────────────────────────────────────────── */

void stance_init(void) {
    g_current_stance = JASH_STANCE_EXPLORE;
}

jash_stance_t stance_get(void) {
    return g_current_stance;
}

int stance_set(jash_stance_t id) {
    if (id < 0 || id >= JASH_STANCE_COUNT) return -1;
    g_current_stance = id;
    return stance_apply();
}

const char* stance_name(jash_stance_t id) {
    if (id < 0 || id >= JASH_STANCE_COUNT) return NULL;
    return g_stance_configs[id].name;
}

const char* stance_description(jash_stance_t id) {
    if (id < 0 || id >= JASH_STANCE_COUNT) return NULL;
    return g_stance_configs[id].description;
}

const jash_stance_cfg_t* stance_cfg(jash_stance_t id) {
    if (id < 0 || id >= JASH_STANCE_COUNT) return NULL;
    return &g_stance_configs[id];
}

const jash_stance_cfg_t* stance_cfg_current(void) {
    return &g_stance_configs[g_current_stance];
}

jash_stance_t stance_from_name(const char* name) {
    if (!name) return JASH_STANCE_COUNT;
    for (int i = 0; i < JASH_STANCE_COUNT; i++) {
        if (strcmp(g_stance_configs[i].name, name) == 0)
            return (jash_stance_t)i;
    }
    return JASH_STANCE_COUNT;
}

int stance_apply(void) {
    const jash_stance_cfg_t* cfg = stance_cfg_current();
    if (!cfg) return -1;

    /* Push config to context engine: collect with the stance's budget. */
    context_basket_t basket;
    context_get_basket(&basket);
    context_budget_calc(&basket, cfg->context_budget);
    context_prioritize(&basket);
    context_prune(&basket, cfg->context_budget);

    /* Note: digest level integration happens at call-site when the
     * agent/user runs `digest --session`. The stance just sets the
     * default level that the digest builtin reads. */
    return 0;
}
