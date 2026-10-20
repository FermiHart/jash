// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: readline/edit — line buffer manipulation (impl).

#include "edit.h"

/* The kill ring is a single-slot buffer shared by all jash_line_t
 * instances in the same process. (Bash uses one per shell too —
 * yank-pop with a ring is a feature we deliberately skip; manifesto
 * §11 minimalism.)
 *
 * Lives in BSS as a static. This is the ONLY file-static in jash 1.0;
 * it is a process-wide UX clipboard and conceptually distinct from
 * jash_state_t (the shell state). If we ever truly need multiple
 * shell instances in one process (REPL-in-REPL?) this becomes a state
 * field. */
static char     g_kill_buf[JASH_LINE_MAX];
static uint32_t g_kill_len = 0;

static void kill_set(const char* src, uint32_t n) {
    if (n > JASH_LINE_MAX) n = JASH_LINE_MAX;
    for (uint32_t i = 0; i < n; i++) g_kill_buf[i] = src[i];
    g_kill_len = n;
}

void jash_line_reset(jash_line_t* L) {
    L->len = 0;
    L->cur = 0;
    L->buf[0] = '\0';
}

int jash_line_insert_byte(jash_line_t* L, char b) {
    if (L->len + 1 >= JASH_LINE_MAX) return 0;
    /* Shift right from cursor. */
    for (uint32_t i = L->len; i > L->cur; i--) L->buf[i] = L->buf[i-1];
    L->buf[L->cur] = b;
    L->len++;
    L->cur++;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_backspace(jash_line_t* L) {
    if (L->cur == 0) return 0;
    for (uint32_t i = L->cur - 1; i + 1 < L->len; i++) L->buf[i] = L->buf[i+1];
    L->len--;
    L->cur--;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_delete(jash_line_t* L) {
    if (L->cur >= L->len) return 0;
    for (uint32_t i = L->cur; i + 1 < L->len; i++) L->buf[i] = L->buf[i+1];
    L->len--;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_left(jash_line_t* L)  { if (L->cur == 0)      return 0; L->cur--; return 1; }
int jash_line_right(jash_line_t* L) { if (L->cur >= L->len) return 0; L->cur++; return 1; }
int jash_line_home(jash_line_t* L)  { if (L->cur == 0)      return 0; L->cur = 0; return 1; }
int jash_line_end(jash_line_t* L)   { if (L->cur == L->len) return 0; L->cur = L->len; return 1; }

int jash_line_kill_eol(jash_line_t* L) {
    if (L->cur == L->len) return 0;
    kill_set(L->buf + L->cur, L->len - L->cur);
    L->len = L->cur;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_kill_bol(jash_line_t* L) {
    if (L->cur == 0) return 0;
    kill_set(L->buf, L->cur);
    uint32_t tail = L->len - L->cur;
    for (uint32_t i = 0; i < tail; i++) L->buf[i] = L->buf[L->cur + i];
    L->len = tail;
    L->cur = 0;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_clear(jash_line_t* L) {
    if (L->len == 0) return 0;
    kill_set(L->buf, L->len);
    jash_line_reset(L);
    return 1;
}

int jash_line_kill_word(jash_line_t* L) {
    if (L->cur == 0) return 0;
    /* Skip trailing whitespace, then the word before. */
    uint32_t end = L->cur;
    uint32_t i = end;
    while (i > 0 && (L->buf[i-1] == ' ' || L->buf[i-1] == '\t')) i--;
    while (i > 0 && L->buf[i-1] != ' ' && L->buf[i-1] != '\t') i--;
    if (i == end) return 0;
    /* [i, end) is the word + ws to kill. */
    kill_set(L->buf + i, end - i);
    uint32_t tail = L->len - end;
    for (uint32_t k = 0; k < tail; k++) L->buf[i + k] = L->buf[end + k];
    L->len -= (end - i);
    L->cur = i;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_yank(jash_line_t* L) {
    if (g_kill_len == 0) return 0;
    /* Insert g_kill_buf[0..g_kill_len) at cursor. */
    if (L->len + g_kill_len >= JASH_LINE_MAX) return 0;
    /* Shift tail right by g_kill_len. */
    for (uint32_t i = L->len; i > L->cur; i--) {
        L->buf[i + g_kill_len - 1] = L->buf[i - 1];
    }
    for (uint32_t i = 0; i < g_kill_len; i++) L->buf[L->cur + i] = g_kill_buf[i];
    L->len += g_kill_len;
    L->cur += g_kill_len;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_replace(jash_line_t* L, const char* s) {
    jash_line_reset(L);
    if (!s) return 1;
    while (*s && L->len + 1 < JASH_LINE_MAX) {
        L->buf[L->len++] = *s++;
    }
    L->cur = L->len;
    L->buf[L->len] = '\0';
    return 1;
}

int jash_line_vi_word_fwd(jash_line_t* L) {
    if (L->cur >= L->len) return 0;
    uint32_t p = L->cur;
    while (p < L->len && L->buf[p] != ' ' && L->buf[p] != '\t') p++;
    while (p < L->len && (L->buf[p] == ' ' || L->buf[p] == '\t')) p++;
    if (p == L->cur) return 0;
    L->cur = p;
    return 1;
}

int jash_line_vi_word_back(jash_line_t* L) {
    if (L->cur == 0) return 0;
    uint32_t p = L->cur;
    while (p > 0 && (L->buf[p-1] == ' ' || L->buf[p-1] == '\t')) p--;
    while (p > 0 && L->buf[p-1] != ' ' && L->buf[p-1] != '\t') p--;
    if (p == L->cur) return 0;
    L->cur = p;
    return 1;
}

int jash_line_vi_word_end(jash_line_t* L) {
    if (L->cur + 1 >= L->len) return 0;
    uint32_t p = L->cur + 1;
    while (p < L->len && (L->buf[p] == ' ' || L->buf[p] == '\t')) p++;
    if (p >= L->len) return 0;
    while (p + 1 < L->len && L->buf[p+1] != ' ' && L->buf[p+1] != '\t') p++;
    if (p == L->cur) return 0;
    L->cur = p;
    return 1;
}

int jash_line_vi_replace_char(jash_line_t* L, char c) {
    if (L->cur >= L->len) return 0;
    L->buf[L->cur] = c;
    return 1;
}
