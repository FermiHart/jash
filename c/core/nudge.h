// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/nudge — proactive, opt-out tips that DRIVE adoption of the
// differentials at the moment of need (the "instruct/nudge" adoption layer).
// F E R M I ∞ H A R T
#ifndef JASH_CORE_NUDGE_H
#define JASH_CORE_NUDGE_H

#include <theme/io.h>

// Called when a command fails (rc != 0). Suggests recall/expect — once per
// rate-limit window, only when recording is on (agent context), and never when
// JASH_NUDGE=0. Cheap and silent in the common case.
// (Since ADR-0040 W4 this is a thin wrapper over the hint engine — see
//  core/hints.h — which adds moment-of-relevance rules + a session budget.)
void jash_nudge_failure(jash_writer_t w, void* ctx);

// Cooldown stamps shared by nudge + hint engine: returns 1 (and stamps
// ~/.jash/nudges/<tag>) when `tag` is outside its cooldown window, else 0.
int jash_nudge_due(const char* tag, int cooldown);

#endif /* JASH_CORE_NUDGE_H */
