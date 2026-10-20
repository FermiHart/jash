// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/hints — moment-of-relevance hint engine (ADR-0040 W4).
//
// The instruct/nudge adoption layer, generalized: instead of dumping the
// manual upfront, jash whispers the ONE relevant differentiator at the
// moment the agent just did something the hard (bash) way.
//
// Anti-spam contract:
//   - at most ONE hint per executed command (first matching rule wins)
//   - per-tag cooldown stamps under ~/.jash/nudges/ (shared with nudge.c)
//   - session-wide cap: JASH_HINT_BUDGET (default 5, 0 = off)
//   - only when recording (sigil >= LOG); JASH_NUDGE=0 kills everything
//   - never advertises a builtin that is absent from the differentiator
//     registry (c/core/differentiators) — no drift, single source of truth
// F E R M I ∞ H A R T
#ifndef JASH_CORE_HINTS_H
#define JASH_CORE_HINTS_H

#include <theme/io.h>

// Post-exec hook: `name`/`args` of the command that just ran, its exit code.
// Cheap and silent in the common case.
void jash_hint_post_exec(const char* name, const char* args, int rc,
                         jash_writer_t w, void* ctx);

// Dashboard/scorecard introspection (ADR-0040 W6): session budget spent/cap.
int jash_hint_budget_used(void);
int jash_hint_budget_cap(void);

#endif /* JASH_CORE_HINTS_H */
