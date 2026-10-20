// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — autopair — auto-close brackets, quotes, parentheses (impl).

#include "autopair.h"
#include <string.h>

static int g_autopair_enabled = 1;

static char pair_of(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        case '`': return '`';
        default: return 0;
    }
}

static int is_opening(char c) {
    return c == '(' || c == '[' || c == '{' ||
           c == '"' || c == '\'' || c == '`';
}

static int is_closing(char c) {
    return c == ')' || c == ']' || c == '}' ||
           c == '"' || c == '\'' || c == '`';
}

static int in_string_context(const jash_line_t* L) {
    char q = 0;
    for (uint32_t i = 0; i < L->cur; i++) {
        if (L->buf[i] == '\\') { i++; continue; }
        if (q) { if (L->buf[i] == q) q = 0; }
        else if (L->buf[i] == '"' || L->buf[i] == '\'' || L->buf[i] == '`')
            q = L->buf[i];
    }
    return q != 0;
}

void jash_autopair_init(void) {
    g_autopair_enabled = 1;
}

void jash_autopair_set_enabled(int enabled) {
    g_autopair_enabled = enabled;
}

int jash_autopair_feed(jash_line_t* L, char c) {
    if (!g_autopair_enabled || !L) return 0;

    int in_str = in_string_context(L);

    if (is_opening(c)) {
        char close = pair_of(c);

        if ((c == '"' || c == '\'' || c == '`') && in_str) {
            if (L->cur < L->len && L->buf[L->cur] == c) {
                L->cur++;
                return 2;
            }
            return 0;
        }

        if (L->len + 2 >= JASH_LINE_MAX) return 0;

        /* Shift buffer right by 2 to make room for the pair.
         * Must include the character at cursor position. */
        uint32_t i = L->len;
        while (i > L->cur) {
            i--;
            L->buf[i + 2] = L->buf[i];
        }
        L->buf[L->cur] = c;
        L->buf[L->cur + 1] = close;
        L->len += 2;
        L->cur++;
        return 1;
    }

    if (is_closing(c) && L->cur < L->len && L->buf[L->cur] == c) {
        L->cur++;
        return 2;
    }

    return 0;
}

int jash_autopair_backspace(jash_line_t* L) {
    if (!g_autopair_enabled || !L || L->cur == 0) return 0;

    char before = L->buf[L->cur - 1];
    char after = (L->cur < L->len) ? L->buf[L->cur] : 0;

    char close = pair_of(before);
    if (close && close == after) {
        for (uint32_t i = L->cur + 1; i < L->len; i++)
            L->buf[i - 2] = L->buf[i];
        L->len -= 2;
        L->cur--;
        L->buf[L->len] = 0;
        return 3;
    }

    return 0;
}
