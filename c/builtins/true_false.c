// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/true_false — POSIX no-op exits.

#include "builtin.h"

int jash_b_true (jash_state_t* s, jash_writer_t w, void* ctx, const char* a) {
    (void)s; (void)w; (void)ctx; (void)a; return 0;
}
int jash_b_false(jash_state_t* s, jash_writer_t w, void* ctx, const char* a) {
    (void)s; (void)w; (void)ctx; (void)a; return 1;
}
int jash_b_colon(jash_state_t* s, jash_writer_t w, void* ctx, const char* a) {
    (void)s; (void)w; (void)ctx; (void)a; return 0;
}
