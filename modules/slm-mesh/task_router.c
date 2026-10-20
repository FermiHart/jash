// SPDX-License-Identifier: LicenseRef-SPL-1.0
// task_router.c — Routes tasks to appropriate LLM provider
// Part of Jash Cognitive OS (ADR-0022) — slm-mesh module
//
// 90% local SLM, 8% on-device medium, 2% cloud (ADR-0022 §3)

#include "cloud_bridge.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── Task Types ─────────────────────────────────────────────────────────────

typedef enum {
    TASK_SIMPLE,        // pattern matching, local SLM (90%)
    TASK_MEDIUM,        // reasoning, on-device (8%)
    TASK_COMPLEX,       // deep reasoning, cloud (2%)
} task_complexity_t;

typedef struct {
    const char* pattern;
    task_complexity_t complexity;
} task_pattern_t;

static const task_pattern_t TASK_PATTERNS[] = {
    // Simple — local SLM
    {"autocomplete",   TASK_SIMPLE},
    {"spellcheck",     TASK_SIMPLE},
    {"intent detect",  TASK_SIMPLE},
    {"risk assess",    TASK_SIMPLE},
    {"command correct", TASK_SIMPLE},

    // Medium — on-device
    {"code review",    TASK_MEDIUM},
    {"summarize",      TASK_MEDIUM},
    {"explain",        TASK_MEDIUM},
    {"translate",      TASK_MEDIUM},

    // Complex — cloud
    {"debug crash",    TASK_COMPLEX},
    {"security audit", TASK_COMPLEX},
    {"architecture",   TASK_COMPLEX},
    {"refactor",       TASK_COMPLEX},
    {"design",         TASK_COMPLEX},

    {NULL, TASK_SIMPLE}
};

// ── Routing Config ─────────────────────────────────────────────────────────

static struct {
    cb_provider_t cloud_provider;   // preferred cloud provider
    float local_confidence;         // min confidence to trust local SLM
    float cloud_fallback;           // confidence below which to escalate
} g_route_config = {
    .cloud_provider  = CB_PROVIDER_CLAUDE,
    .local_confidence = 0.80f,
    .cloud_fallback   = 0.50f,
};

void tr_set_cloud_provider(cb_provider_t provider) {
    if (provider < CB_PROVIDER_COUNT)
        g_route_config.cloud_provider = provider;
}

void tr_set_confidence_thresholds(float local, float cloud) {
    g_route_config.local_confidence = local;
    g_route_config.cloud_fallback = cloud;
}

// ── Task Classification ────────────────────────────────────────────────────

static task_complexity_t classify_task(const char* task_type) {
    if (!task_type) return TASK_SIMPLE;
    for (int i = 0; TASK_PATTERNS[i].pattern; i++) {
        if (strstr(task_type, TASK_PATTERNS[i].pattern))
            return TASK_PATTERNS[i].complexity;
    }
    return TASK_SIMPLE;
}

// ── Route a Query ──────────────────────────────────────────────────────────

typedef struct {
    cb_provider_t provider;
    const char* model;
    float confidence;
    int is_cloud;
} tr_route_t;

tr_route_t tr_route(const char* task_type, float local_confidence) {
    tr_route_t route = {0};

    task_complexity_t complexity = classify_task(task_type);

    // ADR-0022 §3: 90% local, 8% on-device, 2% cloud
    if (complexity == TASK_COMPLEX || local_confidence < g_route_config.cloud_fallback) {
        // Cloud fallback
        route.provider = g_route_config.cloud_provider;
        route.model = cb_provider_model(route.provider);
        route.confidence = 0.95f;
        route.is_cloud = 1;
    } else if (complexity == TASK_MEDIUM && local_confidence < g_route_config.local_confidence) {
        // On-device medium model
        if (cb_has_key(CB_PROVIDER_GROQ)) {
            route.provider = CB_PROVIDER_GROQ;
            route.model = cb_provider_model(CB_PROVIDER_GROQ);
        } else {
            route.provider = g_route_config.cloud_provider;
            route.model = cb_provider_model(route.provider);
        }
        route.confidence = 0.90f;
        route.is_cloud = 1;
    } else {
        // Local SLM (placeholder — returns local provider)
        route.provider = CB_PROVIDER_OLLAMA;
        route.model = "local-slm";
        route.confidence = local_confidence;
        route.is_cloud = 0;
    }

    return route;
}

// ── Route Statistics ───────────────────────────────────────────────────────

static struct {
    uint64_t local;
    uint64_t cloud;
    uint64_t total;
} g_stats = {0};

void tr_record_route(const tr_route_t* route) {
    if (!route) return;
    g_stats.total++;
    if (route->is_cloud) g_stats.cloud++;
    else g_stats.local++;
}

float tr_local_percent(void) {
    if (g_stats.total == 0) return 0.0f;
    return (float)g_stats.local / (float)g_stats.total * 100.0f;
}

float tr_cloud_percent(void) {
    if (g_stats.total == 0) return 0.0f;
    return (float)g_stats.cloud / (float)g_stats.total * 100.0f;
}

void tr_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}