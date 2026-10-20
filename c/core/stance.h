// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/stance — operational modes that transform jash behavior.
//
// Each stance selects different context providers, adjusts token budgets,
// modifies journal auto-tag priority, and controls digest --session level.
//
// See Docs/ADR-0035-context-engine-stances-cargo.md

#ifndef JASH_STANCE_H
#define JASH_STANCE_H

#include <stdint.h>
#include <core/context_engine.h>
#include <core/digest_engine.h>

/* ── Stance Enum ───────────────────────────────────────────── */

typedef enum {
    JASH_STANCE_EXPLORE = 0,
    JASH_STANCE_BUILD,
    JASH_STANCE_DEBUG,
    JASH_STANCE_REVIEW,
    JASH_STANCE_DEPLOY,
    JASH_STANCE_COUNT
} jash_stance_t;

/* ── Stance Config ─────────────────────────────────────────── */

typedef struct {
    jash_stance_t          id;
    const char*            name;
    const char*            description;
    uint64_t               context_budget;                        /* max tokens */
    digest_session_level_t digest_level;                          /* brief/standard/deep */
    int                    journal_priority_tags[8];              /* tag prioritization hints */
    int                    n_priority_tags;
} jash_stance_cfg_t;

/* ── Public API ────────────────────────────────────────────── */

/* Lifecycle */
void stance_init(void);

/* Get / Set */
jash_stance_t              stance_get(void);
int                        stance_set(jash_stance_t id);
const char*                stance_name(jash_stance_t id);
const char*                stance_description(jash_stance_t id);

/* Config access */
const jash_stance_cfg_t*   stance_cfg(jash_stance_t id);
const jash_stance_cfg_t*   stance_cfg_current(void);

/* Apply — pushes stance config to context engine */
int                        stance_apply(void);

/* Lookup */
jash_stance_t              stance_from_name(const char* name);

#endif /* JASH_STANCE_H */
