// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Idle Sigil Defender easter egg: Bear protects the hash chain.

#ifndef JASH_EASTER_BEAR_EGG_H
#define JASH_EASTER_BEAR_EGG_H

#include <core/state.h>
#include <readline/loop.h>
#include <theme/io.h>

int  jash_bear_egg_enabled(jash_state_t* s);
long jash_bear_egg_idle_ms(jash_state_t* s);
void jash_bear_egg_render_ghost(jash_state_t* s, jash_writer_t w, void* ctx);
void jash_bear_egg_run(jash_state_t* s, jash_reader_t r, void* rctx,
                       jash_writer_t w, void* wctx);

#endif /* JASH_EASTER_BEAR_EGG_H */
