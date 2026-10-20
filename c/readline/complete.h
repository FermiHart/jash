// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/complete — TAB completion (header).

#ifndef JASH_READLINE_COMPLETE_H
#define JASH_READLINE_COMPLETE_H

#include <core/state.h>
#include "render.h"
#include "edit.h"

/* Called when the user presses TAB. Mutates L (insertion / expansion)
 * and may emit listing output via w(wctx).
 * Returns 1 if a completion was applied or a pager opened, 0 if nothing matched. */
int jash_complete_tab(jash_state_t* s, jash_writer_t w, void* wctx,
                      jash_line_t* L);

/* Second-TAB command HUD: list candidates selected by command semantics without
 * mutating the current line. */
void jash_complete_command_hud(jash_state_t* s, jash_writer_t w, void* wctx,
                               const jash_line_t* L);

/* Completion pager. When active, Tab/arrows move selection,
 * Enter accepts, Esc cancels. Returns 1 when the key was consumed. */
int jash_complete_pager_key(jash_state_t* s, jash_writer_t w, void* wctx,
                            jash_line_t* L, int key);
int jash_complete_pager_active(void);

/* Reset the cycling state (call when line content changes outside of TAB). */
void jash_complete_reset_cycle(void);

#endif
