// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_invoice.h — Token economics: count tokens saved/wasted by jash mediation
// F E R M I ∞ H A R T
//
// Tracks how much LLM context jash avoids by providing attested history,
// structured state, cached digests, guard blocks, and brain suggestions.

#ifndef JASH_CORE_INVOICE_H
#define JASH_CORE_INVOICE_H

#include <stdint.h>
#include <stddef.h>
#include <theme/io.h>

// ── Token economics categories ───────────────────────────────────────────────

typedef enum {
    JASH_INVOICE_RECALL = 0,   // past commands/state retrieved without search
    JASH_INVOICE_DIGEST,       // cached project briefing
    JASH_INVOICE_STATE,        // structured state mirror
    JASH_INVOICE_GUARD,        // blocked bypass or repeated search
    JASH_INVOICE_BRAIN,        // accepted next-command suggestion
    JASH_INVOICE_BYPASS,       // wasted tokens due to bypass (penalty)
    JASH_INVOICE_CATEGORY_COUNT
} jash_invoice_category_t;

// ── Stats ────────────────────────────────────────────────────────────────────

typedef struct {
    uint64_t saved[JASH_INVOICE_CATEGORY_COUNT];  // tokens saved per category
    uint64_t commands_predicted;
    uint64_t suggestions_offered;
    uint64_t suggestions_accepted;
    uint64_t guard_blocks;
    uint64_t naive_input_tokens;  // estimated if the agent did everything naively
    uint64_t jash_input_tokens;   // estimated with jash mediation
} jash_invoice_stats_t;

// ── Session (one invoice) ────────────────────────────────────────────────────

typedef struct {
    char     id[64];
    char     intent[256];
    uint64_t started_ns;
    uint64_t closed_ns;
    int      active;
    jash_invoice_stats_t delta;
} jash_invoice_session_t;

// ── Lifecycle ────────────────────────────────────────────────────────────────

int  jash_invoice_init(const char* data_dir);
/* Read observation without creating the store or scheduling cleanup writes.
 * Explicit later mutations still create/save the store normally. */
int  jash_invoice_init_readonly(const char* data_dir);
void jash_invoice_cleanup(void);
int  jash_invoice_save(void);
int  jash_invoice_load(void);

// ── Accounting ───────────────────────────────────────────────────────────────

// Add saved tokens for a category. Approximate counts are fine.
void jash_invoice_add_saved(jash_invoice_category_t cat, uint64_t tokens);

// Penalty: tokens wasted when an agent escapes to bash/sh/python/etc.
void jash_invoice_add_bypass_waste(uint64_t tokens);

// Record that the brain offered a suggestion.
void jash_invoice_suggestion_offered(void);

// Record that the user/agent accepted a brain suggestion.
void jash_invoice_suggestion_accepted(void);

// Record that the guard blocked or warned about a bypass.
void jash_invoice_guard_block(void);

// Estimate naive vs jash token cost for a command string.
void jash_invoice_account_command(const char* cmd, int is_jash_native);

// ── Sessions ─────────────────────────────────────────────────────────────────

int jash_invoice_begin(const char* intent);
int jash_invoice_close(jash_invoice_stats_t* out);
int jash_invoice_session_active(void);
const jash_invoice_session_t* jash_invoice_current(void);

// ── Reporting ────────────────────────────────────────────────────────────────

const jash_invoice_stats_t* jash_invoice_lifetime(void);
void jash_invoice_print_status(jash_writer_t w, void* ctx);
void jash_invoice_print_session(jash_writer_t w, void* ctx,
                                const jash_invoice_session_t* s);

#endif // JASH_CORE_INVOICE_H
