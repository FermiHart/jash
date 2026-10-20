// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/render — paint the prompt + line + cursor.
//
// Two strategies:
//   FULL  — write prompt, line, then ESC[<n>D to back the cursor up
//           by (len - cur) cells. Cheap, works under any TTY.
//   DIFF  — track previous render and only emit changes. Phase 3.
//
// For now: FULL. The line buffer is ≤ 1 KB; redraw cost is negligible.

#ifndef JASH_READLINE_RENDER_H
#define JASH_READLINE_RENDER_H

#include "edit.h"
#include <theme/io.h>
#include <core/state.h>

/* Paint the prompt + current line, position cursor logically.
 * Emits "\r ESC[K" first to clear stale chars from prior renders. */
void jash_render_line(jash_writer_t w, void* ctx,
                      const jash_state_t* s, const jash_line_t* L);

void jash_render_line_suggest(jash_writer_t w, void* ctx,
                              const jash_state_t* s, const jash_line_t* L,
                              const char* suggestion);

/* Clear the entire current line (e.g. after Enter, before output). */
void jash_render_newline(jash_writer_t w, void* ctx);

#endif /* JASH_READLINE_RENDER_H */
