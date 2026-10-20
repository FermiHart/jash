// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: theme/prompt — render the interactive prompt.
//
// Format (default):
//   <user>@<host>:<cwd>[<exit>] $
//
// where the [<exit>] segment shows the last status code only when
// non-zero. Each role uses J_ROLE_* tokens, so themes recolor the
// whole prompt with no string edits.

#ifndef JASH_THEME_PROMPT_H
#define JASH_THEME_PROMPT_H

#include "io.h"
#include <core/state.h>

/* Render the interactive prompt to writer w/ctx. Reads user/host/cwd
 * and last_status from the state. */
void jash_prompt_render(jash_writer_t w, void* ctx, const jash_state_t* s);
void jash_prompt_set_compact_redraw(int compact);

/* Low-frequency prompt sigil spinner. Enabled by default; disable with
 * JASH_PROMPT_SPINNER=0/off/no. */
int  jash_prompt_spinner_enabled(const jash_state_t* s);
void jash_prompt_spinner_advance(const jash_state_t* s);

/* The static host string. On Josh this is always "fermihart" until
 * mDNS / config supplies otherwise. */
extern const char* const JASH_HOSTNAME;

#endif /* JASH_THEME_PROMPT_H */
