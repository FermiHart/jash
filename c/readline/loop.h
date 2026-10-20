// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/loop — the read-edit-render loop.
//
// One call to jash_read_line() returns ONE finished command line
// (terminated by Enter). The function:
//   - prints the prompt
//   - reads bytes through (reader_fn, ctx_in)
//   - decodes keys, mutates the line buffer
//   - paints through (writer_fn, ctx_out) after every change
//   - navigates history with Up/Down
//   - handles Ctrl-C (discard line) and Ctrl-D (return EOF on empty)
//
// Return values mirror conventional readline:
//   0   ok — `out` filled with NUL-terminated line (no trailing \n)
//  -1   EOF (Ctrl-D on empty line — caller should exit)
//  -2   I/O error from the reader

#ifndef JASH_READLINE_LOOP_H
#define JASH_READLINE_LOOP_H

#include "edit.h"
#include <theme/io.h>
#include <core/state.h>

/* Reader callback: fill 1 byte into *out. Returns 1 on byte read,
 * 0 on EOF, negative on error. */
typedef int (*jash_reader_t)(void* ctx, char* out);

int jash_read_line(jash_state_t* s,
                   jash_reader_t r, void* rctx,
                   jash_writer_t w, void* wctx,
                   char* out, uint32_t out_cap);

#endif /* JASH_READLINE_LOOP_H */
