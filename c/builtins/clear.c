// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/clear — clear the screen.

#include "builtin.h"
#include <theme/colors.h>
#include <ui/statusbar.h>
#include <core/jash_ai.h>
#ifdef JASH_BETHOS
#include <osif/bethos/surface.h>
#endif

int jash_b_clear(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)args;
#ifdef JASH_BETHOS
    if (jash_bethos_surface_clear() == 0)
        return 0;
#endif
    if (jash_statusbar_region_active()) {
        /* Keep the reserved top statusbar line sovereign; clear only the
         * scrollable command area below it. */
        jash_writes(w, ctx, "\033[2;1H\033[J");
    } else {
        jash_writes(w, ctx, J_CLEAR_SCREEN J_CURSOR_HOME);
    }
    
    /* Force statusbar redraw to preserve AI MODE indicator */
    jash_statusbar_mark_dirty();
    jash_statusbar_render(w, ctx, s);
    
    /* Redraw AI MODE indicator if active and statusbar is off */
    if (jash_ai_active() && !jash_statusbar_region_active()) {
        render_ai_mode_indicator();
    }
    
    return 0;
}
