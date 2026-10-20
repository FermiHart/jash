// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/context_engine — unified context management.
//
// See Docs/ADR-0035-context-engine-stances-cargo.md

#include "context_engine.h"
#include "provider_git.h"
#include "provider_state.h"
#include "provider_digest.h"
#include "stance.h"
#include <journal/journal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Static Provider Registry ─────────────────────────────── */

static context_provider_t* g_providers[CONTEXT_MAX_PROVIDERS];
static int g_nproviders = 0;
static uint64_t g_next_item_id = 1;

/* ── Comparison Helpers ───────────────────────────────────── */

/* Lower sort-index = higher priority (for qsort ascending). */
static int item_priority_cmp(const void* a, const void* b) {
    const context_item_t* ia = (const context_item_t*)a;
    const context_item_t* ib = (const context_item_t*)b;
    /* Pinned items always come first. */
    if (ia->pinned != ib->pinned) return ib->pinned - ia->pinned;
    /* Higher priority number = more important, sort ascending. */
    if (ia->priority != ib->priority) return ib->priority - ia->priority;
    /* Tie-break: newer timestamp first. */
    if (ia->timestamp > ib->timestamp) return -1;
    if (ia->timestamp < ib->timestamp) return  1;
    return 0;
}

/* ── Lifecycle ─────────────────────────────────────────────── */

void context_init(void) {
    g_nproviders = 0;
    g_next_item_id = 1;
    for (int i = 0; i < CONTEXT_MAX_PROVIDERS; i++)
        g_providers[i] = NULL;
}

void context_destroy(void) {
    for (int i = 0; i < g_nproviders; i++) {
        if (g_providers[i] && g_providers[i]->reset)
            g_providers[i]->reset(g_providers[i]);
    }
    g_nproviders = 0;
}

/* ── Provider Registry ─────────────────────────────────────── */

int context_provider_register(context_provider_t* provider) {
    if (!provider || !provider->name || !provider->name[0]) return -1;
    if (g_nproviders >= CONTEXT_MAX_PROVIDERS) return -1;
    /* Check for duplicates. */
    for (int i = 0; i < g_nproviders; i++) {
        if (g_providers[i] && strcmp(g_providers[i]->name, provider->name) == 0)
            return -1;
    }
    g_providers[g_nproviders++] = provider;
    return 0;
}

int context_provider_unregister(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_nproviders; i++) {
        if (g_providers[i] && strcmp(g_providers[i]->name, name) == 0) {
            if (g_providers[i]->reset) g_providers[i]->reset(g_providers[i]);
            /* Shift remaining providers left. */
            for (int j = i; j < g_nproviders - 1; j++)
                g_providers[j] = g_providers[j + 1];
            g_providers[--g_nproviders] = NULL;
            return 0;
        }
    }
    return -1;
}

int context_provider_count(void) {
    return g_nproviders;
}

const char* context_provider_name(int index) {
    if (index < 0 || index >= g_nproviders || !g_providers[index])
        return NULL;
    return g_providers[index]->name;
}

/* ── Built-in: Journal Provider ────────────────────────────── */

static int journal_collect(context_provider_t* self, context_basket_t* basket) {
    (void)self;
    journal_tag_history_t hist;
    hist.count = 0;
    hist.cap = 64;
    int n = journal_tag_history_get(&hist, 64);
    if (n <= 0) return 0;

    int added = 0;
    for (int i = 0; i < n && basket->n_items < CONTEXT_MAX_ITEMS; i++) {
        journal_entry_t* je = &hist.items[i];
        context_item_t* ci = &basket->items[basket->n_items];
        ci->id = 0;
        ci->timestamp = (uint64_t)time(NULL);
        ci->provider = "journal";
        snprintf(ci->tag, sizeof(ci->tag), "%s", je->tag);
        ci->pinned = 0;

        /* Priority: err > retry > start/resume > sw: > other */
        if      (je->kind == JOURNAL_TAG_ERROR)   ci->priority = 200;
        else if (je->kind == JOURNAL_TAG_RETRY)   ci->priority = 180;
        else if (je->kind == JOURNAL_TAG_SESSION_START) ci->priority = 150;
        else if (je->kind == JOURNAL_TAG_RESUMED) ci->priority = 140;
        else if (je->kind == JOURNAL_TAG_SWITCHED) ci->priority = 120;
        else                                       ci->priority = 100;

        /* Penalize if no error context. */
        if (je->error_count == 0 && ci->priority > 100)
            ci->priority -= 20;

        snprintf(ci->source, sizeof(ci->source), "journal:tag=%s", je->tag);
        if (je->domain[0])
            snprintf(ci->summary, sizeof(ci->summary), "[%s] tag=%s err=%u",
                     je->domain, je->tag, je->error_count);
        else
            snprintf(ci->summary, sizeof(ci->summary), "tag=%s err=%u",
                     je->tag, je->error_count);

        /* Estimate ~15 tokens per tag entry. */
        ci->token_cost = 15;
        snprintf(ci->raw, sizeof(ci->raw), "%s domain=%s err=%u",
                 je->tag, je->domain, je->error_count);
        basket->n_items++;
        added++;
    }
    return added;
}

static uint64_t journal_estimate(context_provider_t* self) {
    (void)self;
    journal_tag_history_t hist;
    hist.count = 0;
    hist.cap = 64;
    int n = journal_tag_history_get(&hist, 64);
    return (uint64_t)n * 15ULL;
}

static int journal_prune(context_provider_t* self, uint64_t target) {
    (void)self; (void)target;
    /* Journal is a fixed 64-slot ring — no pruning needed. */
    return 0;
}

static void journal_provider_reset(context_provider_t* self) {
    (void)self;
    journal_reset();
}

static context_provider_t g_journal_provider = {
    .name  = "journal",
    .collect = journal_collect,
    .estimate_tokens = journal_estimate,
    .prune  = journal_prune,
    .reset  = journal_provider_reset,
    .state  = NULL
};

/* ── Built-in Provider Registration ────────────────────────── */

void context_register_builtins(void* jash_state) {
    context_provider_register(&g_journal_provider);
    context_provider_register(provider_git_get());
    context_provider_register(provider_state_get((jash_state_t*)jash_state));
    context_provider_register(provider_digest_get());
}

/* ── Item ID Allocation ─────────────────────────────────────── */

uint64_t context_next_item_id(void) {
    return g_next_item_id++;
}

/* ── Core: Collect ─────────────────────────────────────────── */

void context_collect(context_basket_t* basket) {
    if (!basket) return;
    int prev_count = 0;
    basket->n_items = 0;

    for (int i = 0; i < g_nproviders; i++) {
        if (g_providers[i] && g_providers[i]->collect)
            g_providers[i]->collect(g_providers[i], basket);
    }

    /* Assign sequential IDs to items that providers set to 0. */
    for (int i = prev_count; i < basket->n_items; i++) {
        if (basket->items[i].id == 0)
            basket->items[i].id = g_next_item_id++;
    }
}

/* ── Core: Budget ──────────────────────────────────────────── */

void context_budget_calc(context_basket_t* basket, uint64_t total_budget) {
    if (!basket) return;
    basket->budget.total_budget = total_budget;
    basket->budget.used = 0;
    basket->budget.reserved = 0;
    basket->budget.n_providers = 0;

    /* Count per-provider tokens and total. */
    for (int i = 0; i < basket->n_items; i++) {
        context_item_t* item = &basket->items[i];
        basket->budget.used += item->token_cost;
        if (item->pinned) basket->budget.reserved += item->token_cost;

        /* Track per-provider. */
        int found = 0;
        for (int p = 0; p < basket->budget.n_providers; p++) {
            if (strcmp(basket->budget.per_provider[p].provider, item->provider) == 0) {
                basket->budget.per_provider[p].tokens += item->token_cost;
                found = 1;
                break;
            }
        }
        if (!found && basket->budget.n_providers < CONTEXT_MAX_PROVIDERS) {
            int p = basket->budget.n_providers++;
            basket->budget.per_provider[p].provider = item->provider;
            basket->budget.per_provider[p].tokens = item->token_cost;
        }
    }
}

/* ── Core: Prioritize ──────────────────────────────────────── */

void context_prioritize(context_basket_t* basket) {
    if (!basket || basket->n_items <= 0) return;
    qsort(basket->items, (size_t)basket->n_items, sizeof(context_item_t),
          item_priority_cmp);
}

/* ── Core: Prune ───────────────────────────────────────────── */

int context_prune(context_basket_t* basket, uint64_t target_tokens) {
    if (!basket) return -1;
    if (basket->budget.used <= target_tokens) return 0;

    /* Prune starts from the end (lowest priority). */
    int pruned = 0;
    while (basket->n_items > 0 && basket->budget.used > target_tokens) {
        /* Find the lowest-priority non-pinned item. */
        int worst = -1;
        for (int i = 0; i < basket->n_items; i++) {
            if (basket->items[i].pinned) continue;
            if (worst < 0) { worst = i; continue; }
            /* Compare using reverse priority (lowest first). */
            if (basket->items[i].priority < basket->items[worst].priority)
                worst = i;
            else if (basket->items[i].priority == basket->items[worst].priority &&
                     basket->items[i].timestamp > basket->items[worst].timestamp)
                worst = i;
        }
        if (worst < 0) break; /* All remaining items are pinned. */

        basket->budget.used -= basket->items[worst].token_cost;
        /* Shift left. */
        for (int j = worst; j < basket->n_items - 1; j++)
            basket->items[j] = basket->items[j + 1];
        basket->n_items--;
        pruned++;
    }

    /* Recalculate budget after pruning. */
    context_budget_calc(basket, basket->budget.total_budget);
    return pruned;
}

/* ── Core: Pin / Unpin ─────────────────────────────────────── */

int context_pin(context_basket_t* basket, uint64_t item_id) {
    if (!basket) return -1;
    for (int i = 0; i < basket->n_items; i++) {
        if (basket->items[i].id == item_id) {
            basket->items[i].pinned = 1;
            return 0;
        }
    }
    return -1;
}

int context_unpin(context_basket_t* basket, uint64_t item_id) {
    if (!basket) return -1;
    for (int i = 0; i < basket->n_items; i++) {
        if (basket->items[i].id == item_id) {
            basket->items[i].pinned = 0;
            return 0;
        }
    }
    return -1;
}

/* ── Core: Get Basket ──────────────────────────────────────── */

int context_get_basket(context_basket_t* basket) {
    if (!basket) return -1;
    context_collect(basket);
    context_prioritize(basket);
    context_budget_calc(basket, 8000);
    return basket->n_items;
}
