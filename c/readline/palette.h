// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — palette — VS Code-style command palette (Ctrl+P).

#ifndef JASH_READLINE_PALETTE_H
#define JASH_READLINE_PALETTE_H

#include "edit.h"
#include "loop.h"
#include <core/state.h>
#include "render.h"

int jash_palette_open(jash_state_t* s,
                      jash_reader_t r, void* rctx,
                      jash_writer_t w, void* wctx,
                      jash_line_t* L);

#endif /* JASH_READLINE_PALETTE_H */
