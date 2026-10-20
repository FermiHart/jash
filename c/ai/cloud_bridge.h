// SPDX-License-Identifier: LicenseRef-SPL-1.0
// c/ai/cloud_bridge.h — multi-provider LLM bridge (no libcurl dependency)
// F E R M I ∞ H A R T

#ifndef JASH_AI_CLOUD_BRIDGE_H
#define JASH_AI_CLOUD_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    CB_PROVIDER_AUTO   = -1,  /* pick best available           */
    CB_PROVIDER_CLAUDE =  0,  /* ANTHROPIC_API_KEY             */
    CB_PROVIDER_GROQ   =  1,  /* GROQ_API_KEY (llama-3.3-70b)  */
    CB_PROVIDER_OLLAMA =  2,  /* localhost:11434 (no key)      */
    CB_PROVIDER_COUNT  =  3,
} cb_provider_t;

typedef enum {
    CB_OK          = 0,
    CB_ERR_AUTH    = 1,  /* missing/invalid API key  */
    CB_ERR_CONNECT = 2,  /* curl failed / no network */
    CB_ERR_TIMEOUT = 3,
    CB_ERR_PARSE   = 4,  /* response not parseable   */
    CB_ERR_NO_PROV = 5,  /* no provider configured   */
} cb_error_t;

/* Returns CB_PROVIDER_AUTO (-1) when no provider is configured. */
cb_provider_t cb_best_provider(void);

/* Query an LLM.  provider=CB_PROVIDER_AUTO picks the best available one.
 * system_prompt may be NULL.  API key is passed via a temp file — never
 * appears in argv/ps output.  Fills response[] on success. */
cb_error_t cb_query(cb_provider_t provider,
                    const char* prompt, const char* system_prompt,
                    char* response, size_t response_cap,
                    uint32_t* latency_ms_out);

const char* cb_provider_name(cb_provider_t p);
const char* cb_provider_model(cb_provider_t p);
int         cb_has_key(cb_provider_t p);
const char* cb_error_str(cb_error_t e);

#endif /* JASH_AI_CLOUD_BRIDGE_H */
