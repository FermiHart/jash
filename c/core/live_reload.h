// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/live_reload — execve self while preserving state (header).

#ifndef JASH_CORE_LIVE_RELOAD_H_
#define JASH_CORE_LIVE_RELOAD_H_

#include "state.h"

/* Re-execute jash, preserving shell state (env, history, aliases, cwd).
 * Does not return on success; returns error code on failure. */
int jash_live_reload(jash_state_t* s);

#endif
