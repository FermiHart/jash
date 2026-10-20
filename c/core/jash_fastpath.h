// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_fastpath.h — Zero-LLM Smalltalk Classifier
// Routes only short greetings, thanks, farewells, reactions to the fast path.
// Bilingual PT/EN — port of BearCode's bear_fastpath.py.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_FASTPATH_H
#define JASH_CORE_FASTPATH_H

#include <stdint.h>

// ── Fastpath Result ──────────────────────────────────────────────────────────

typedef enum {
    FASTPATH_NONE = 0,          // not smalltalk — route to LLM
    FASTPATH_GREETING,          // "oi", "hello", "bom dia"
    FASTPATH_THANKS,            // "obrigado", "thanks"
    FASTPATH_FAREWELL,          // "tchau", "bye"
    FASTPATH_REACTION,          // "legal", "nice", "show"
    FASTPATH_AGREE,             // "sim", "yes", "claro"
    FASTPATH_DISAGREE,          // "nao", "no", "nunca"
    FASTPATH_CONFUSION,         // "como?", "what?", "huh"
} jash_fastpath_kind_t;

// ── Fastpath Response ────────────────────────────────────────────────────────

typedef struct {
    jash_fastpath_kind_t kind;
    const char* response;          // static response string
    float confidence;              // 0.0 — 1.0
    int had_vocative;              // 1 if the user's name was detected + stripped
} jash_fastpath_result_t;

// ── Public API ───────────────────────────────────────────────────────────────

// Classify a user input. Returns fastpath result.
// If input is smalltalk, kind != FASTPATH_NONE and response is pre-built.
// If input is NOT smalltalk, kind == FASTPATH_NONE.
jash_fastpath_result_t jash_fastpath_classify(const char* input);

// Set user name for vocative stripping.
void jash_fastpath_set_user_name(const char* name);

#endif // JASH_CORE_FASTPATH_H
