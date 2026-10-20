// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — render — line painter with theme-aware syntax highlighting (impl).

#include "render.h"
#include <theme/prompt.h>
#include <theme/theme.h>
#include <core/env.h>
#include <builtins/builtin.h>
#include <string.h>

static void render_syntax(jash_writer_t w, void* ctx, const jash_line_t* L) {
    if (!L->len) return;
    const jash_theme_t* theme = jash_theme_current();
    uint32_t i = 0;
    int first_word = 1;
    while (i < L->len) {
        char c = L->buf[i];
        if (c == '#') {
            jash_writes(w, ctx, theme->syn_comment);
            w(ctx, L->buf + i, L->len - i);
            jash_writes(w, ctx, "\033[0m");
            return;
        }
        if (c == ' ' || c == '\t') { w(ctx, &c, 1); i++; continue; }

        /* Variable expansion ($VAR, ${VAR}) */
        if (c == '$') {
            jash_writes(w, ctx, theme->syn_variable);
            uint32_t start = i;
            i++;
            if (i < L->len && L->buf[i] == '{') {
                i++;
                while (i < L->len && L->buf[i] != '}') i++;
                if (i < L->len) i++;
            } else {
                while (i < L->len && ((L->buf[i] >= 'a' && L->buf[i] <= 'z') ||
                       (L->buf[i] >= 'A' && L->buf[i] <= 'Z') ||
                       (L->buf[i] >= '0' && L->buf[i] <= '9') ||
                       L->buf[i] == '_')) i++;
            }
            w(ctx, L->buf + start, i - start);
            jash_writes(w, ctx, "\033[0m");
            first_word = 0;
            continue;
        }

        /* Numbers */
        if (c >= '0' && c <= '9') {
            jash_writes(w, ctx, theme->syn_number);
            uint32_t start = i;
            while (i < L->len && ((L->buf[i] >= '0' && L->buf[i] <= '9') ||
                   L->buf[i] == '.' || L->buf[i] == 'x')) i++;
            w(ctx, L->buf + start, i - start);
            jash_writes(w, ctx, "\033[0m");
            first_word = 0;
            continue;
        }

        /* Quoted strings */
        char quote = 0;
        if (c == '"' || c == '\'' || c == '`') quote = c;
        if (quote) {
            uint32_t start = i;
            i++;
            while (i < L->len && L->buf[i] != quote) {
                if (L->buf[i] == '\\' && i + 1 < L->len) i += 2;
                else i++;
            }
            if (i < L->len) i++;
            jash_writes(w, ctx, theme->syn_string);
            w(ctx, L->buf + start, i - start);
            jash_writes(w, ctx, "\033[0m");
            first_word = 0;
            continue;
        }

        /* Redirects */
        if (c == '>' || c == '<' || (c == '|' && i + 1 < L->len && L->buf[i + 1] == '|') ||
            (c == '&' && i + 1 < L->len && L->buf[i + 1] == '>')) {
            jash_writes(w, ctx, theme->syn_redirect);
            uint32_t start = i;
            while (i < L->len && (L->buf[i] == '>' || L->buf[i] == '<' ||
                   L->buf[i] == '|' || L->buf[i] == '&' || L->buf[i] == ' ')) i++;
            w(ctx, L->buf + start, i - start);
            jash_writes(w, ctx, "\033[0m");
            continue;
        }

        /* Flags */
        if (c == '-' && i + 1 < L->len && L->buf[i + 1] != ' ') {
            jash_writes(w, ctx, theme->syn_flag);
            uint32_t start = i;
            i++;
            while (i < L->len && L->buf[i] != ' ' && L->buf[i] != '\t') i++;
            w(ctx, L->buf + start, i - start);
            jash_writes(w, ctx, "\033[0m");
            first_word = 0;
            continue;
        }

        /* Words (commands / arguments) */
        uint32_t start = i;
        while (i < L->len && L->buf[i] != ' ' && L->buf[i] != '\t') i++;
        uint32_t len = i - start;
        char word[128];
        uint32_t wl = len < sizeof(word) - 1 ? len : sizeof(word) - 1;
        for (uint32_t k = 0; k < wl; k++) word[k] = L->buf[start + k];
        word[wl] = 0;

        const char* color = NULL;
        if (first_word && jash_builtin_lookup(word)) color = theme->syn_builtin;
        else if (first_word) color = theme->syn_command;

        if (color) jash_writes(w, ctx, color);
        w(ctx, L->buf + start, len);
        if (color) jash_writes(w, ctx, "\033[0m");
        first_word = 0;
    }
}

void jash_render_newline(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "\r\n");
}

void jash_render_line(jash_writer_t w, void* ctx,
                      const jash_state_t* s, const jash_line_t* L) {
    jash_render_line_suggest(w, ctx, s, L, NULL);
}

void jash_render_line_suggest(jash_writer_t w, void* ctx,
                              const jash_state_t* s, const jash_line_t* L,
                              const char* suggestion) {
    if (!w || !s || !L) return;

    /* Clear from start of line + erase to EOL. */
    jash_writes(w, ctx, "\r\033[K");

    /* Prompt. */
    jash_prompt_render(w, ctx, s);

    /* Line content with theme-aware contextual syntax highlighting. */
    render_syntax(w, ctx, L);

    if (suggestion && *suggestion && L->cur == L->len) {
        const char* color = jash_env_get(s, "JASH_AUTOSUGGEST_COLOR");
        int color_ok = color && color[0] == '\033' && color[1] == '[';
        jash_writes(w, ctx, color_ok ? color : "\033[90m");
        jash_writes_sanitized(w, ctx, suggestion);
        jash_writes(w, ctx, "\033[0m");
        jash_writef(w, ctx, "\033[%uD", (unsigned)strlen(suggestion));
    }

    if (L->cur < L->len) {
        unsigned back = L->len - L->cur;
        jash_writef(w, ctx, "\033[%uD", back);
    }
}
