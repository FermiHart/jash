// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_digest — session digest context provider implementation.

#include "provider_digest.h"
#include "digest_engine.h"
#include "stance.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static char g_digest_buf[CONTEXT_RAW_LEN];
static size_t g_digest_pos = 0;

static long digest_capture_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
    size_t remain = sizeof(g_digest_buf) - 1 - g_digest_pos;
    size_t to_copy = n < remain ? n : remain;
    if (to_copy > 0) {
        memcpy(g_digest_buf + g_digest_pos, buf, to_copy);
        g_digest_pos += to_copy;
        g_digest_buf[g_digest_pos] = '\0';
    }
    return (long)n;
}

static int digest_collect(context_provider_t* self, context_basket_t* basket) {
    (void)self;
    if (!basket || basket->n_items >= CONTEXT_MAX_ITEMS) return 0;

    /* Determine digest level from current stance. */
    const jash_stance_cfg_t* scfg = stance_cfg_current();
    digest_session_level_t level = scfg ? scfg->digest_level : SESH_STANDARD;

    /* Configure session digest. */
    digest_session_cfg_t dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    dcfg.level = level;
    dcfg.last_n = (level == SESH_BRIEF) ? 10 : (level == SESH_STANDARD) ? 25 : 50;
    dcfg.include_git = 1;
    dcfg.output_path = NULL;

    /* Capture digest output — handle gracefully if session unavailable. */
    g_digest_pos = 0;
    g_digest_buf[0] = '\0';

    int drc = digest_session_run(&dcfg, digest_capture_writer, NULL);
    if (drc != 0 || g_digest_pos == 0)
        return 0;

    size_t len = g_digest_pos;
    context_item_t* ci = &basket->items[basket->n_items];
    ci->id = 0;
    ci->timestamp = (uint64_t)time(NULL);
    ci->provider = "digest";
    snprintf(ci->tag, sizeof(ci->tag), "session_digest");
    ci->token_cost = (uint64_t)(len / 4); /* ~4 chars per token */
    if (ci->token_cost < 20) ci->token_cost = 20;
    ci->priority = 170;
    ci->pinned = 0;
    snprintf(ci->source, sizeof(ci->source), "digest:session");
    {
        const char* lname = "standard";
        if (level == SESH_BRIEF) lname = "brief";
        else if (level == SESH_DEEP) lname = "deep";
        snprintf(ci->summary, sizeof(ci->summary), "session_digest: level=%s tokens=%llu",
                 lname, (unsigned long long)ci->token_cost);
    }
    size_t rawlen = len < sizeof(ci->raw) ? len : sizeof(ci->raw) - 1;
    memcpy(ci->raw, g_digest_buf, rawlen);
    ci->raw[rawlen] = '\0';
    basket->n_items++;
    return 1;
}

static uint64_t digest_estimate(context_provider_t* self) {
    (void)self;
    const jash_stance_cfg_t* scfg = stance_cfg_current();
    if (!scfg) return 500;
    if (scfg->digest_level == SESH_BRIEF) return 200;
    if (scfg->digest_level == SESH_DEEP) return 1500;
    return 500;
}

static int digest_prune(context_provider_t* self, uint64_t target) {
    (void)self; (void)target;
    return 0;
}

static void digest_reset(context_provider_t* self) {
    (void)self;
}

static context_provider_t g_digest_provider = {
    .name  = "digest",
    .collect = digest_collect,
    .estimate_tokens = digest_estimate,
    .prune  = digest_prune,
    .reset  = digest_reset,
    .state  = NULL
};

context_provider_t* provider_digest_get(void) {
    return &g_digest_provider;
}
