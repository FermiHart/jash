// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_state — shell state context provider implementation.

#include "provider_state.h"
#include "env.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    jash_state_t* state;
} state_provider_priv_t;

static int state_collect(context_provider_t* self, context_basket_t* basket) {
    if (!self || !self->state || !basket) return 0;
    state_provider_priv_t* p = (state_provider_priv_t*)self->state;
    if (!p->state) return 0;
    jash_state_t* s = p->state;
    int added = 0;

    if (basket->n_items >= CONTEXT_MAX_ITEMS) return 0;

    /* 1. CWD */
    {
        context_item_t* ci = &basket->items[basket->n_items];
        ci->id = 0;
        ci->timestamp = (uint64_t)time(NULL);
        ci->provider = "state";
        snprintf(ci->tag, sizeof(ci->tag), "cwd");
        ci->token_cost = 5;
        ci->priority = 180;
        ci->pinned = 0;
        snprintf(ci->source, sizeof(ci->source), "state:cwd");
        snprintf(ci->summary, sizeof(ci->summary), "cwd: %s", s->cwd);
        snprintf(ci->raw, sizeof(ci->raw), "%s", s->cwd);
        basket->n_items++;
        added++;
    }

    /* 2. Last exit status */
    if (basket->n_items < CONTEXT_MAX_ITEMS) {
        context_item_t* ci = &basket->items[basket->n_items];
        ci->id = 0;
        ci->timestamp = (uint64_t)time(NULL);
        ci->provider = "state";
        snprintf(ci->tag, sizeof(ci->tag), "last_status");
        ci->token_cost = 10;
        ci->priority = s->last_status.code != 0 ? 200 : 120;
        ci->pinned = 0;
        snprintf(ci->source, sizeof(ci->source), "state:last_status");
        snprintf(ci->summary, sizeof(ci->summary), "last_status: code=%d cmd=%s",
                 s->last_status.code, s->last_status.cmd);
        snprintf(ci->raw, sizeof(ci->raw), "%d %s %s",
                 s->last_status.code, s->last_status.cmd, s->last_status.msg);
        basket->n_items++;
        added++;
    }

    /* 3. Shell opts (key ones) */
    if (basket->n_items < CONTEXT_MAX_ITEMS) {
        char opts[256];
        int pos = 0;
        if (s->shell_opts.errexit)  { pos += snprintf(opts + pos, sizeof(opts) - (size_t)pos, "errexit "); }
        if (s->shell_opts.nounset)  { pos += snprintf(opts + pos, sizeof(opts) - (size_t)pos, "nounset "); }
        if (s->shell_opts.xtrace)   { pos += snprintf(opts + pos, sizeof(opts) - (size_t)pos, "xtrace "); }
        if (s->shell_opts.noglob)   { pos += snprintf(opts + pos, sizeof(opts) - (size_t)pos, "noglob "); }
        if (s->shell_opts.monitor)  { pos += snprintf(opts + pos, sizeof(opts) - (size_t)pos, "monitor "); }
        if (pos == 0) snprintf(opts, sizeof(opts), "default");
        context_item_t* ci = &basket->items[basket->n_items];
        ci->id = 0;
        ci->timestamp = (uint64_t)time(NULL);
        ci->provider = "state";
        snprintf(ci->tag, sizeof(ci->tag), "shell_opts");
        ci->token_cost = 5;
        ci->priority = 100;
        ci->pinned = 0;
        snprintf(ci->source, sizeof(ci->source), "state:shell_opts");
        snprintf(ci->summary, sizeof(ci->summary), "shell_opts: %s", opts);
        snprintf(ci->raw, sizeof(ci->raw), "%s", opts);
        basket->n_items++;
        added++;
    }

    /* 4. Interactive flag + PID */
    if (basket->n_items < CONTEXT_MAX_ITEMS) {
        char meta[128];
        snprintf(meta, sizeof(meta), "interactive=%d pid=%ld", s->interactive, s->pid);
        context_item_t* ci = &basket->items[basket->n_items];
        ci->id = 0;
        ci->timestamp = (uint64_t)time(NULL);
        ci->provider = "state";
        snprintf(ci->tag, sizeof(ci->tag), "session");
        ci->token_cost = 5;
        ci->priority = 90;
        ci->pinned = 0;
        snprintf(ci->source, sizeof(ci->source), "state:session");
        snprintf(ci->summary, sizeof(ci->summary), "session: %s", meta);
        snprintf(ci->raw, sizeof(ci->raw), "%s", meta);
        basket->n_items++;
        added++;
    }
    return added;
}

static uint64_t state_estimate(context_provider_t* self) {
    (void)self;
    return 30;
}

static int state_prune(context_provider_t* self, uint64_t target) {
    (void)self; (void)target;
    return 0;
}

static void state_reset(context_provider_t* self) {
    (void)self;
}

context_provider_t* provider_state_get(jash_state_t* jash_state) {
    static state_provider_priv_t priv;
    static int initialized = 0;
    static context_provider_t provider;

    if (!initialized) {
        priv.state = jash_state;
        provider.name  = "state";
        provider.collect = state_collect;
        provider.estimate_tokens = state_estimate;
        provider.prune  = state_prune;
        provider.reset  = state_reset;
        provider.state  = &priv;
        initialized = 1;
    }
    return &provider;
}
