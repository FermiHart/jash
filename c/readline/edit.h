// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/edit — mid-line editing buffer.
//
// A pure data-structure: fixed-capacity char buffer plus a cursor
// index. All operations are byte-bounded and safe under any input,
// including malformed UTF-8 (we treat the buffer as bytes; UTF-8
// awareness is deferred to render — a higher layer counts grapheme
// widths but storage is byte-flat).
//
// All operations return 1 on success (something happened) and 0 on
// no-op (e.g. backspace at column 0). This makes redraw decisions
// trivial in the layer above: "if the editor reports change, redraw".

#ifndef JASH_READLINE_EDIT_H
#define JASH_READLINE_EDIT_H

#include <stdint.h>
#include <stddef.h>

#define JASH_LINE_MAX  1024

typedef struct {
    char     buf[JASH_LINE_MAX];
    uint32_t len;        /* number of bytes in buf (NOT including NUL) */
    uint32_t cur;        /* cursor index, 0..len */
} jash_line_t;

/* Lifecycle. */
void jash_line_reset(jash_line_t* L);

/* Mutators. Return 1 on change, 0 on no-op. */
int jash_line_insert_byte(jash_line_t* L, char b);
int jash_line_backspace  (jash_line_t* L);     /* delete byte before cursor */
int jash_line_delete     (jash_line_t* L);     /* delete byte at cursor */
int jash_line_left       (jash_line_t* L);
int jash_line_right      (jash_line_t* L);
int jash_line_home       (jash_line_t* L);
int jash_line_end        (jash_line_t* L);
int jash_line_kill_eol   (jash_line_t* L);     /* Ctrl-K: cut to end */
int jash_line_kill_bol   (jash_line_t* L);     /* Ctrl-U: cut to start */
int jash_line_kill_word  (jash_line_t* L);     /* Ctrl-W: cut prev word */
int jash_line_clear      (jash_line_t* L);     /* like kill_bol but full */
int jash_line_replace    (jash_line_t* L, const char* s);

/* Yank (paste) the last kill buffer at cursor. Kill ops feed it. */
int jash_line_yank       (jash_line_t* L);

/* Vi normal-mode motion and editing. */
int jash_line_vi_word_fwd    (jash_line_t* L);   /* w: start of next word */
int jash_line_vi_word_back   (jash_line_t* L);   /* b: start of prev word */
int jash_line_vi_word_end    (jash_line_t* L);   /* e: end of current word */
int jash_line_vi_replace_char(jash_line_t* L, char c); /* r: replace char at cursor */

#endif /* JASH_READLINE_EDIT_H */
