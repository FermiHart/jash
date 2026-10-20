// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: logout — exit login shell

#include "builtin.h"

int jash_b_logout(jash_state_t* s, jash_writer_t w, void* ctx,
                  const char* args) {
    (void)args;
    if (!s->interactive) {
        jash_writes(w, ctx, "logout: not login shell\n");
        return 1;
    }
    s->last_status.code = 0;
    s->interactive = 0;
    return 0;
}
