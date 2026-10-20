// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/keymap — byte stream → key decoder (impl).
//
// Recognized CSI sequences (ESC [ ...):
//   ESC [ A     → UP        ESC [ B   → DOWN
//   ESC [ C     → RIGHT     ESC [ D   → LEFT
//   ESC [ H     → HOME      ESC [ F   → END
//   ESC [ 3 ~   → DELETE
//   ESC [ 5 ~   → PAGEUP    ESC [ 6 ~ → PAGEDOWN
//   ESC [ 200 ~ → PASTE_START
//   ESC [ 201 ~ → PASTE_END
//   ESC [ 1 ~   → HOME (legacy)
//   ESC [ 4 ~   → END  (legacy)
//
// Bare ESC (no follow-up within the SAME feed) is reported as JK_ESC
// only when the decoder times out — that's a higher-layer concern.
// For now, ESC alone stays pending; the layer above can flush with a
// jash_keydec_init() if the user paused mid-escape.

#include "keymap.h"

void jash_keydec_init(jash_keydec_t* d) {
    d->pending_len = 0;
}

/* Helper: clear pending buffer. */
static void clear_pending(jash_keydec_t* d) {
    d->pending_len = 0;
}

/* Recognize the final byte of a CSI sequence after ESC [ collected
 * args/params. Returns key code, or JK_UNKNOWN.
 *
 * `params` is the in-decoder buffer minus the leading ESC and [. */
static int decode_csi(const uint8_t* params, uint8_t plen) {
    if (plen == 0) return JK_UNKNOWN;
    uint8_t final = params[plen - 1];

    /* Single-byte CSI (ESC [ X): arrows / home / end. */
    if (plen == 1) {
        switch (final) {
        case 'A': return JK_UP;
        case 'B': return JK_DOWN;
        case 'C': return JK_RIGHT;
        case 'D': return JK_LEFT;
        case 'H': return JK_HOME;
        case 'F': return JK_END;
        default:  return JK_UNKNOWN;
        }
    }

    /* Multi-byte: number(s) followed by '~'. We only need decimal
     * numbers in 1–3 digits without ';' parameters. */
    if (final != '~') return JK_UNKNOWN;

    /* Parse the leading integer. */
    int v = 0;
    for (uint8_t i = 0; i + 1 < plen; i++) {
        if (params[i] < '0' || params[i] > '9') return JK_UNKNOWN;
        v = v * 10 + (params[i] - '0');
    }
    switch (v) {
    case 1:   return JK_HOME;      /* xterm legacy */
    case 3:   return JK_DELETE;
    case 4:   return JK_END;       /* xterm legacy */
    case 5:   return JK_PAGEUP;
    case 6:   return JK_PAGEDOWN;
    case 200: return JK_PASTE_START;
    case 201: return JK_PASTE_END;
    default:  return JK_UNKNOWN;
    }
}

int jash_keydec_feed(jash_keydec_t* d, uint8_t b) {
    /* No pending state: classify the byte standalone. */
    if (d->pending_len == 0) {
        if (b == 0x1B) {  /* ESC — start of possible CSI; buffer it */
            d->pending[d->pending_len++] = b;
            return 0;
        }
        if (b == 0x7F) return JK_BACKSPACE;   /* DEL → backspace */
        if (b == '\r' || b == '\n') return JK_ENTER;
        /* Anything 0x01..0x1A is a Ctrl-X key code; 0x20+ is a literal. */
        return (int)b;
    }

    /* Pending starts with ESC. Decide if we have '[' yet. */
    if (d->pending_len == 1) {
        if (b == '[') {
            d->pending[d->pending_len++] = b;
            return 0;
        }
        /* ESC followed by anything else — not a CSI we know about.
         * Emit the buffered ESC and feed `b` as a fresh byte. */
        clear_pending(d);
        if (b == 0x1B) {  /* immediate second ESC — restart pending */
            d->pending[d->pending_len++] = b;
            return JK_ESC;
        }
        /* Otherwise: we drop b for simplicity (caller can re-feed). */
        (void)b;
        return JK_ESC;
    }

    /* We have ESC [ already; collect bytes until a "final" byte. The
     * final byte for arrows is 0x40..0x7E excluding '['; for the
     * tilde-terminated form it's '~' specifically. */
    if (d->pending_len >= sizeof(d->pending)) {
        clear_pending(d);
        return JK_UNKNOWN;
    }
    d->pending[d->pending_len++] = b;

    if ((b >= '@' && b <= '~')) {
        /* The "final" byte. Decode using params (everything after the
         * '['). pending = ESC '[' params... */
        int key = decode_csi(d->pending + 2, d->pending_len - 2);
        clear_pending(d);
        return key;
    }
    /* Still collecting digits/separators. */
    return 0;
}
