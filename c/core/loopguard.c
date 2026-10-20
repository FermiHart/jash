// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/loopguard — anti-loop circuit breaker (ADR-0042 F3).
// F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "loopguard.h"
#include "chain_read.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_RING_SIZE 32
#define LG_WARN_THRESHOLD  3
#define LG_BLOCK_THRESHOLD 5

typedef struct {
    char name[64];
    char tokens[10][128];
    int  ntokens;
    int  rc;
} lg_record_t;

static lg_record_t g_ring[LG_RING_SIZE];
static int g_ring_pos = 0;
static int g_ring_count = 0;

static const char* lg_env_mode(void) {
    const char* e = getenv("JASH_LOOPGUARD");
    if (!e || !*e) return "warn";
    return e;
}

static void lg_tokenize_cmd(const char* cmd, lg_record_t* r) {
    r->ntokens = 0;
    /* Work on a mutable copy so we can null-terminate tokens */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cmd);
    char* p = buf;
    while (*p && r->ntokens < 10) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char quote = 0;
        if (*p == '\'' || *p == '"') { quote = *p; p++; }
        int wi = 0;
        while (*p && wi < 127) {
            if (quote) {
                if (*p == quote) { p++; break; }
            } else {
                if (*p == ' ' || *p == '\t') break;
            }
            r->tokens[r->ntokens][wi++] = *p++;
        }
        r->tokens[r->ntokens][wi] = '\0';
        r->ntokens++;
    }
}

static double lg_jaccard(lg_record_t* a, lg_record_t* b) {
    /* Jaccard over tokens AFTER argv[0] (argv[0] similarity is checked
     * separately by the caller — lg_similar already confirms same name). */
    int na = a->ntokens > 1 ? a->ntokens - 1 : 0;
    int nb = b->ntokens > 1 ? b->ntokens - 1 : 0;
    if (na == 0 && nb == 0) return 1.0;
    int inter = 0;
    for (int i = 1; i < a->ntokens; i++) {
        for (int j = 1; j < b->ntokens; j++) {
            if (strcmp(a->tokens[i], b->tokens[j]) == 0) { inter++; break; }
        }
    }
    int uni = na + nb - inter;
    return uni > 0 ? (double)inter / (double)uni : 0.0;
}

static int lg_similar(lg_record_t* a, lg_record_t* b) {
    if (strcmp(a->name, b->name) != 0) return 0;
    if (a->ntokens <= 1 && b->ntokens <= 1) return 1; /* same name, no args */
    double j = lg_jaccard(a, b);
    return j >= 0.7;
}

/* Scan recent chain entries from chain.log to bootstrap the ring with
 * historical failures (so the loopguard works across -c invocations). */
static void lg_bootstrap_from_chain(void) {
    static int bootstrapped = 0;
    if (bootstrapped) return;
    bootstrapped = 1;

    jash_chain_entry_t entries[32];
    int n = jash_chain_tail(entries, 32, 32);
    /* entries are newest-first; we want oldest-first for the ring */
    for (int i = n - 1; i >= 0; i--) {
        if (entries[i].rc == 0) continue; /* only bootstrap failures */
        lg_record_t r = {0};
        snprintf(r.name, sizeof(r.name), "%s", entries[i].cmd);
        /* extract argv[0] */
        const char* sp = entries[i].cmd;
        while (*sp == ' ') sp++;
        int wi = 0;
        while (sp[wi] && sp[wi] != ' ' && sp[wi] != '\t' && wi < 63) {
            r.name[wi] = sp[wi]; wi++;
        }
        r.name[wi] = '\0';
        lg_tokenize_cmd(entries[i].cmd, &r);
        r.rc = entries[i].rc;
        g_ring[g_ring_pos] = r;
        g_ring_pos = (g_ring_pos + 1) % LG_RING_SIZE;
        if (g_ring_count < LG_RING_SIZE) g_ring_count++;
    }
}

int jash_loopguard_verdict(const char* name, const char* cmd) {
    const char* mode = lg_env_mode();
    if (strcmp(mode, "off") == 0) return 0;
    if (!name || !cmd || !*cmd) return 0;

    lg_bootstrap_from_chain();

    /* Build a tentative record for the incoming command */
    lg_record_t incoming = {0};
    snprintf(incoming.name, sizeof(incoming.name), "%s", name);
    lg_tokenize_cmd(cmd, &incoming);
    incoming.rc = -1; /* unknown yet */

    /* Count consecutive similar failures at the tail of the ring */
    int similar_streak = 0;
    for (int i = g_ring_count - 1; i >= 0; i--) {
        int idx = (g_ring_pos - 1 - i + LG_RING_SIZE) % LG_RING_SIZE;
        if (g_ring[idx].rc == 0) break; /* success resets the streak */
        if (lg_similar(&incoming, &g_ring[idx])) {
            similar_streak++;
        } else {
            break; /* different command breaks the streak */
        }
    }

    if (similar_streak >= LG_BLOCK_THRESHOLD && strcmp(mode, "warn") != 0) {
        return 2; /* block */
    }
    if (similar_streak >= LG_WARN_THRESHOLD) {
        return 1; /* warn */
    }
    return 0;
}

void jash_loopguard_record(const char* name, const char* cmd, int rc) {
    if (!name || !cmd) return;
    /* The ring is only ever consulted by jash_loopguard_verdict, which
     * returns immediately when the mode is off. Recording (snprintf +
     * quote-aware tokenize per command) into an unread ring is pure
     * overhead — skip it. */
    if (strcmp(lg_env_mode(), "off") == 0) return;

    lg_record_t r = {0};
    snprintf(r.name, sizeof(r.name), "%s", name);
    lg_tokenize_cmd(cmd, &r);
    r.rc = rc;

    g_ring[g_ring_pos] = r;
    g_ring_pos = (g_ring_pos + 1) % LG_RING_SIZE;
    if (g_ring_count < LG_RING_SIZE) g_ring_count++;
}
