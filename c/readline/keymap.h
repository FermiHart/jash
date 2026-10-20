// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/keymap — input key constants + decoder.
//
// readline operates on logical KEYS, not bytes. The decoder reads a
// stream of bytes (typically from sys_read on stdin) and emits
// jash_key_t values. This separation lets us test ALL editor logic
// without touching the TTY — just feed it a byte stream and assert
// the cursor/buffer state.
//
// We deliberately implement only the keys jash actually uses. Bash
// has dozens more. Per manifesto §11, when in doubt, leave it out.

#ifndef JASH_READLINE_KEYMAP_H
#define JASH_READLINE_KEYMAP_H

#include <stdint.h>

/* Logical key codes. Values above 0x100 are special (non-byte) so a
 * caller can tell a literal byte from a navigation key. */
typedef enum {
    /* Sub-0x100 reserved for literal printable bytes (0x20–0x7E)
     * and a few control codes that double as named keys (Tab, Enter). */
    JK_NONE     = 0x000,
    JK_BACKSPACE = 0x008,   /* also DEL 0x7F — both normalize here */
    JK_TAB      = 0x009,
    JK_ENTER    = 0x00A,    /* normalized from \r and \n */
    JK_ESC      = 0x01B,    /* bare ESC (no CSI) */
    JK_CTRL_A   = 0x001,
    JK_CTRL_B   = 0x002,
    JK_CTRL_C   = 0x003,
    JK_CTRL_D   = 0x004,
    JK_CTRL_E   = 0x005,
    JK_CTRL_F   = 0x006,
    JK_CTRL_H   = 0x008,    /* same as backspace */
    JK_CTRL_K   = 0x00B,
    JK_CTRL_L   = 0x00C,
    JK_CTRL_N   = 0x00E,
    JK_CTRL_P   = 0x010,
    JK_CTRL_R   = 0x012,
    JK_CTRL_U   = 0x015,
    JK_CTRL_W   = 0x017,
    JK_CTRL_Y   = 0x019,

    /* Arrow keys / nav — emitted by ESC [ <X>. */
    JK_UP       = 0x100,
    JK_DOWN     = 0x101,
    JK_RIGHT    = 0x102,
    JK_LEFT     = 0x103,
    JK_HOME     = 0x104,
    JK_END      = 0x105,
    JK_DELETE   = 0x106,
    JK_PAGEUP   = 0x107,
    JK_PAGEDOWN = 0x108,

    /* Bracketed-paste markers (the surrounding ESC [ 200 ~ / 201 ~). */
    JK_PASTE_START = 0x200,
    JK_PASTE_END   = 0x201,

    /* Unrecognized / partial escape sequence — caller should usually
     * ignore. */
    JK_UNKNOWN  = 0xFFF,
} jash_key_t;

/* Decoder is stream-aware: it can buffer a partial escape sequence
 * across read boundaries. */
typedef struct {
    /* Pending bytes from an unfinished sequence. */
    uint8_t  pending[8];
    uint8_t  pending_len;
} jash_keydec_t;

void jash_keydec_init(jash_keydec_t* d);

/* Feed one byte. Returns:
 *   > 0  jash_key_t value — emit this key
 *   = 0  byte buffered (incomplete sequence); call again with more
 *   < 0  decoder error (caller may discard pending state) */
int jash_keydec_feed(jash_keydec_t* d, uint8_t byte);

#endif /* JASH_READLINE_KEYMAP_H */
