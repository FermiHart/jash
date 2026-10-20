// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/context_engine — unified context management for agent LLM sessions.
//
// Collects, prioritizes, budgets, prunes, and pins context items from
// multiple providers (journal, git, digest, shell state, goals).
//
// See Docs/ADR-0035-context-engine-stances-cargo.md

#ifndef JASH_CONTEXT_ENGINE_H
#define JASH_CONTEXT_ENGINE_H

#include <stdint.h>
#include <stdlib.h>

/* ── Constants ────────────────────────────────────────────── */

#define CONTEXT_MAX_PROVIDERS   8
#define CONTEXT_MAX_ITEMS       256
#define CONTEXT_TAG_LEN         32
#define CONTEXT_PROVENANCE_LEN  256
#define CONTEXT_SUMMARY_LEN     512
#define CONTEXT_RAW_LEN         4096

/* ── Provider Interface ───────────────────────────────────── */

typedef struct context_provider_s context_provider_t;
typedef struct context_basket_s   context_basket_t;

struct context_provider_s {
    const char* name;                                          /* provider id */
    int  (*collect)(context_provider_t* self, context_basket_t* basket);
    uint64_t (*estimate_tokens)(context_provider_t* self);
    int  (*prune)(context_provider_t* self, uint64_t target_tokens);
    void (*reset)(context_provider_t* self);
    void* state;                                               /* provider-private */
};

/* ── Context Item ─────────────────────────────────────────── */

typedef struct {
    uint64_t id;                                               /* unique item id */
    uint64_t timestamp;                                        /* CLOCK_REALTIME sec */
    const char* provider;                                      /* provider name */
    char     tag[CONTEXT_TAG_LEN];                             /* causal tag */
    uint64_t token_cost;                                       /* estimated tokens */
    int      priority;                                         /* 0-255, higher = more important */
    int      pinned;                                           /* 1 = cannot be pruned */
    char     source[CONTEXT_PROVENANCE_LEN];                   /* provenance string */
    char     summary[CONTEXT_SUMMARY_LEN];                     /* human-readable summary */
    char     raw[CONTEXT_RAW_LEN];                             /* full content */
} context_item_t;

/* ── Budget ────────────────────────────────────────────────── */

typedef struct {
    uint64_t total_budget;                                     /* max tokens */
    uint64_t used;                                             /* tokens consumed */
    uint64_t reserved;                                         /* tokens pinned/must-keep */
    struct {
        const char* provider;
        uint64_t    tokens;
    } per_provider[CONTEXT_MAX_PROVIDERS];
    int n_providers;
} context_budget_t;

/* ── Basket ────────────────────────────────────────────────── */

struct context_basket_s {
    context_item_t  items[CONTEXT_MAX_ITEMS];
    int             n_items;
    context_budget_t budget;
};

/* ── Public API ───────────────────────────────────────────── */

/* Lifecycle */
void context_init(void);
void context_destroy(void);

/* Provider registry */
int  context_provider_register(context_provider_t* provider);
int  context_provider_unregister(const char* name);
int  context_provider_count(void);
const char* context_provider_name(int index);

/* Item ID allocation (providers set id=0; engine assigns on collect) */
uint64_t context_next_item_id(void);

/* Core operations */
void context_collect(context_basket_t* basket);
void context_budget_calc(context_basket_t* basket, uint64_t total_budget);
void context_prioritize(context_basket_t* basket);
int  context_prune(context_basket_t* basket, uint64_t target_tokens);

/* Pin/unpin */
int  context_pin(context_basket_t* basket, uint64_t item_id);
int  context_unpin(context_basket_t* basket, uint64_t item_id);

/* Full basket */
int  context_get_basket(context_basket_t* basket);

/* Built-in providers (register on init) — pass jash_state_t for shell state */
void context_register_builtins(void* jash_state);

#endif /* JASH_CONTEXT_ENGINE_H */
