// SPDX-License-Identifier: LicenseRef-SPL-1.0
// llm_status.h — LLM Status Provider for Cognitive Statusbar
// Part of Jash Cognitive OS (ADR-0022)

#ifndef JASH_CORE_LLM_STATUS_H
#define JASH_CORE_LLM_STATUS_H

#include <stdint.h>
#include <stddef.h>

// ── LLM Provider Types ─────────────────────────────────────────────────────
typedef enum {
    LLM_PROVIDER_NONE = 0,
    LLM_PROVIDER_CLAUDE,        // Anthropic Claude
    LLM_PROVIDER_GPT,           // OpenAI GPT
    LLM_PROVIDER_GROQ,          // Groq (Llama, Mixtral)
    LLM_PROVIDER_OLLAMA,        // Ollama local
    LLM_PROVIDER_LOCAL_SLM,     // Local GGUF model
    LLM_PROVIDER_COUNT
} llm_provider_type_t;

// ── LLM Status ─────────────────────────────────────────────────────────────
typedef struct {
    llm_provider_type_t provider;
    char model_name[64];
    uint32_t last_latency_ms;
    uint32_t avg_latency_ms;
    uint64_t total_tokens;
    uint64_t total_requests;
    uint64_t error_count;
    int connected;
    int inferring;              // 1 if currently processing
} llm_status_t;

// ── Latency Tracking ───────────────────────────────────────────────────────
#define LLM_LATENCY_HISTORY 16

typedef struct {
    uint32_t samples[LLM_LATENCY_HISTORY];
    uint32_t index;
    uint32_t count;
} llm_latency_tracker_t;

// ── Public API ─────────────────────────────────────────────────────────────

// Init/cleanup
void llm_status_init(void);
void llm_status_cleanup(void);

// Provider management
void llm_status_set_provider(llm_provider_type_t provider, const char* model);
void llm_status_set_connected(int connected);
llm_provider_type_t llm_status_get_provider(void);
const char* llm_status_get_provider_name(llm_provider_type_t provider);
const char* llm_status_get_provider_icon(llm_provider_type_t provider);

// Latency tracking
void llm_status_update_latency(uint32_t latency_ms);
uint32_t llm_status_get_avg_latency(void);
uint32_t llm_status_get_last_latency(void);

// Token tracking
void llm_status_add_tokens(uint64_t tokens);
uint64_t llm_status_get_total_tokens(void);

// Request tracking
void llm_status_increment_requests(void);
void llm_status_increment_errors(void);
uint64_t llm_status_get_total_requests(void);
uint64_t llm_status_get_error_count(void);

// Inference state
void llm_status_set_inferring(int inferring);
int llm_status_get_inferring(void);

// Get full status
llm_status_t llm_status_get(void);

// Status string for statusbar
void llm_status_to_string(char* buf, size_t buf_len);

#endif // JASH_CORE_LLM_STATUS_H
