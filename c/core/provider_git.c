// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_git — git context provider implementation.

#include "provider_git.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int git_cmd(const char* cmd, char* out, size_t outsz) {
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    size_t total = 0;
    int c;
    while (total < outsz - 1 && (c = fgetc(f)) != EOF)
        out[total++] = (char)c;
    out[total] = '\0';
    return (int)pclose(f);
}

static int git_collect(context_provider_t* self, context_basket_t* basket) {
    (void)self;
    if (!basket) return 0;
    char buf[4096];
    int added = 0;

    if (git_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' ')) buf[--len] = '\0';
        if (len > 0 && basket->n_items < CONTEXT_MAX_ITEMS) {
            context_item_t* ci = &basket->items[basket->n_items];
            ci->id = 0;
            ci->timestamp = (uint64_t)time(NULL);
            ci->provider = "git";
            snprintf(ci->tag, sizeof(ci->tag), "branch");
            ci->token_cost = 5;
            ci->priority = 160;
            ci->pinned = 0;
            snprintf(ci->source, sizeof(ci->source), "git:branch");
            snprintf(ci->summary, sizeof(ci->summary), "branch: %s", buf);
            snprintf(ci->raw, sizeof(ci->raw), "%s", buf);
            basket->n_items++;
            added++;
        }
    }

    if (git_cmd("git status --porcelain 2>/dev/null | wc -l", buf, sizeof(buf)) >= 0) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' ')) buf[--len] = '\0';
        if (len > 0 && basket->n_items < CONTEXT_MAX_ITEMS) {
            int nfiles = atoi(buf);
            context_item_t* ci = &basket->items[basket->n_items];
            ci->id = 0;
            ci->timestamp = (uint64_t)time(NULL);
            ci->provider = "git";
            snprintf(ci->tag, sizeof(ci->tag), "status");
            ci->token_cost = 10;
            ci->priority = nfiles > 0 ? 180 : 100;
            ci->pinned = 0;
            snprintf(ci->source, sizeof(ci->source), "git:status");
            snprintf(ci->summary, sizeof(ci->summary), "dirty files: %d", nfiles);
            snprintf(ci->raw, sizeof(ci->raw), "%d", nfiles);
            basket->n_items++;
            added++;
        }
    }

    if (git_cmd("git diff --stat 2>/dev/null", buf, sizeof(buf)) >= 0 && buf[0]) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == ' ')) buf[--len] = '\0';
        if (len > 0 && basket->n_items < CONTEXT_MAX_ITEMS) {
            context_item_t* ci = &basket->items[basket->n_items];
            ci->id = 0;
            ci->timestamp = (uint64_t)time(NULL);
            ci->provider = "git";
            snprintf(ci->tag, sizeof(ci->tag), "diffstat");
            ci->token_cost = (uint64_t)(len / 5);
            if (ci->token_cost < 10) ci->token_cost = 10;
            ci->priority = 150;
            ci->pinned = 0;
            snprintf(ci->source, sizeof(ci->source), "git:diffstat");
            snprintf(ci->summary, sizeof(ci->summary), "diff: %s", buf);
            snprintf(ci->raw, sizeof(ci->raw), "%s", buf);
            basket->n_items++;
            added++;
        }
    }
    return added;
}

static uint64_t git_estimate(context_provider_t* self) {
    (void)self;
    return 100;
}

static int git_prune(context_provider_t* self, uint64_t target) {
    (void)self; (void)target;
    return 0;
}

static void git_reset(context_provider_t* self) {
    (void)self;
}

static context_provider_t g_git_provider = {
    .name  = "git",
    .collect = git_collect,
    .estimate_tokens = git_estimate,
    .prune  = git_prune,
    .reset  = git_reset,
    .state  = NULL
};

context_provider_t* provider_git_get(void) {
    return &g_git_provider;
}
