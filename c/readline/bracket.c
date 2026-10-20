// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — bracket — bracket matching visual (impl).

#include "bracket.h"

static char match_char(char c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return 0;
    }
}

static int is_open(char c) {
    return c == '(' || c == '[' || c == '{';
}

static int is_close(char c) {
    return c == ')' || c == ']' || c == '}';
}

int jash_is_bracket(char c) {
    return is_open(c) || is_close(c);
}

int jash_bracket_match(const jash_line_t* L, uint32_t pos) {
    if (!L || pos >= L->len) return -1;
    char c = L->buf[pos];
    if (!jash_is_bracket(c)) return -1;

    char target = match_char(c);
    int depth = 0;

    if (is_open(c)) {
        /* Search forward */
        for (uint32_t i = pos; i < L->len; i++) {
            if (L->buf[i] == c) depth++;
            else if (L->buf[i] == target) depth--;
            if (depth == 0) return (int)i;
        }
    } else {
        /* Search backward */
        for (int i = (int)pos; i >= 0; i--) {
            if (L->buf[i] == c) depth++;
            else if (L->buf[i] == target) depth--;
            if (depth == 0) return i;
        }
    }

    return -1;
}
