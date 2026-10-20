// SPDX-License-Identifier: LicenseRef-SPL-1.0
// llm_status.c — LLM Status Provider for Cognitive Statusbar
// Part of Jash Cognitive OS (ADR-0022)
//
// Tracks LLM provider status, latency, tokens, and requests.
// Used by the Cognitive Statusbar mode to show real-time LLM info.

#include "llm_status.h"
#include <string.h>
#include <stdio.h>

// ── Module State ───────────────────────────────────────────────────────────

static llm_status_t g_status = {0};
static llm_latency_tracker_t g_latency = {0};

// ── Provider Names ─────────────────────────────────────────────────────────

static const char* PROVIDER_NAMES[] = {
    "none",
    "Claude",
    "GPT",
    "Groq",
    "Ollama",
    "Local SLM"
};

static const char* PROVIDER_ICONS[] = {
    "⚪",
    "🟣",  // Claude (purple)
    "🟢",  // GPT (green)
    "🟡",  // Groq (yellow)
    "🔵",  // Ollama (blue)
    "🟠"   // Local SLM (orange)
};

// ── Init/Cleanup ───────────────────────────────────────────────────────────

void llm_status_init(void) {
    memset(&g_status, 0, sizeof(g_status));
    memset(&g_latency, 0, sizeof(g_latency));
    g_status.provider = LLM_PROVIDER_NONE;
    g_status.connected = 0;
}

void llm_status_cleanup(void) {
    // Save final stats if needed
    memset(&g_status, 0, sizeof(g_status));
    memset(&g_latency, 0, sizeof(g_latency));
}

// ── Provider Management ────────────────────────────────────────────────────

void llm_status_set_provider(llm_provider_type_t provider, const char* model) {
    if (provider >= LLM_PROVIDER_COUNT) return;
    
    g_status.provider = provider;
    if (model) {
        strlcpy(g_status.model_name, model, sizeof(g_status.model_name));
        g_status.model_name[sizeof(g_status.model_name) - 1] = '\0';
    } else {
        g_status.model_name[0] = '\0';
    }
    g_status.connected = 1;
}

void llm_status_set_connected(int connected) {
    g_status.connected = connected;
}

llm_provider_type_t llm_status_get_provider(void) {
    return g_status.provider;
}

const char* llm_status_get_provider_name(llm_provider_type_t provider) {
    if (provider >= LLM_PROVIDER_COUNT) return "unknown";
    return PROVIDER_NAMES[provider];
}

const char* llm_status_get_provider_icon(llm_provider_type_t provider) {
    if (provider >= LLM_PROVIDER_COUNT) return "❓";
    return PROVIDER_ICONS[provider];
}

// ── Latency Tracking ───────────────────────────────────────────────────────

void llm_status_update_latency(uint32_t latency_ms) {
    // Store in circular buffer
    g_latency.samples[g_latency.index] = latency_ms;
    g_latency.index = (g_latency.index + 1) % LLM_LATENCY_HISTORY;
    if (g_latency.count < LLM_LATENCY_HISTORY) g_latency.count++;
    
    // Update status
    g_status.last_latency_ms = latency_ms;
    
    // Calculate average
    uint64_t sum = 0;
    for (uint32_t i = 0; i < g_latency.count; i++) {
        sum += g_latency.samples[i];
    }
    g_status.avg_latency_ms = (uint32_t)(sum / g_latency.count);
}

uint32_t llm_status_get_avg_latency(void) {
    return g_status.avg_latency_ms;
}

uint32_t llm_status_get_last_latency(void) {
    return g_status.last_latency_ms;
}

// ── Token Tracking ─────────────────────────────────────────────────────────

void llm_status_add_tokens(uint64_t tokens) {
    g_status.total_tokens += tokens;
}

uint64_t llm_status_get_total_tokens(void) {
    return g_status.total_tokens;
}

// ── Request Tracking ───────────────────────────────────────────────────────

void llm_status_increment_requests(void) {
    g_status.total_requests++;
}

void llm_status_increment_errors(void) {
    g_status.error_count++;
}

uint64_t llm_status_get_total_requests(void) {
    return g_status.total_requests;
}

uint64_t llm_status_get_error_count(void) {
    return g_status.error_count;
}

// ── Inference State ────────────────────────────────────────────────────────

void llm_status_set_inferring(int inferring) {
    g_status.inferring = inferring;
}

int llm_status_get_inferring(void) {
    return g_status.inferring;
}

// ── Get Full Status ────────────────────────────────────────────────────────

llm_status_t llm_status_get(void) {
    return g_status;
}

// ── Status String for Statusbar ────────────────────────────────────────────

void llm_status_to_string(char* buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    
    if (!g_status.connected || g_status.provider == LLM_PROVIDER_NONE) {
        snprintf(buf, buf_len, "⚪ offline");
        return;
    }
    
    const char* icon = llm_status_get_provider_icon(g_status.provider);
    const char* model = g_status.model_name[0] ? g_status.model_name : 
                        llm_status_get_provider_name(g_status.provider);
    
    if (g_status.inferring) {
        snprintf(buf, buf_len, "%s %s ◐ %ums", icon, model, g_status.last_latency_ms);
    } else if (g_status.last_latency_ms > 0) {
        snprintf(buf, buf_len, "%s %s %ums", icon, model, g_status.last_latency_ms);
    } else {
        snprintf(buf, buf_len, "%s %s ●", icon, model);
    }
}
