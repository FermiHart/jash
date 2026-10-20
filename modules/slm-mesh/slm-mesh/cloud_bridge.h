// SPDX-License-Identifier: LicenseRef-SPL-1.0
// cloud_bridge.h — HTTP/API bridge to external LLMs
// Part of Jash Cognitive OS (ADR-0022) — slm-mesh module

#ifndef CLOUD_BRIDGE_H
#define CLOUD_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

// Provider types
typedef enum {
    CB_PROVIDER_CLAUDE,     // Anthropic Messages API
    CB_PROVIDER_GPT,        // OpenAI Chat Completions API  
    CB_PROVIDER_GROQ,       // Groq API
    CB_PROVIDER_OLLAMA,     // Ollama local HTTP API
    CB_PROVIDER_COUNT
} cb_provider_t;

typedef enum {
    CB_OK = 0,
    CB_ERR_CONNECT,
    CB_ERR_AUTH,
    CB_ERR_TIMEOUT,
    CB_ERR_PARSE,
} cb_error_t;

typedef struct {
    const char* key_env_var;   // env var name for API key
    const char* endpoint;      // base URL
    const char* default_model;
    int         has_keys;      // 1 if key env var is set
} cb_config_t;

// Get config for a provider
const cb_config_t* cb_get_config(cb_provider_t provider);

// Check if provider has API key configured
int cb_has_key(cb_provider_t provider);

// Query an LLM via curl subprocess (same pattern as jash_ai.c)
// Returns 0 on success, fills response buffer.
cb_error_t cb_query(cb_provider_t provider, const char* model,
                    const char* prompt, const char* system_prompt,
                    char* response, size_t response_cap,
                    uint32_t* latency_ms);

// Provider name strings
const char* cb_provider_name(cb_provider_t provider);
const char* cb_provider_model(cb_provider_t provider);

#endif