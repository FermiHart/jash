// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — editor/snippet — multi-line snippet editor (Ctrl+E).

#include "snippet.h"
#include <readline/keymap.h>
#include <theme/theme.h>
#include <theme/io.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* Ctrl+S for save */
#define JK_CTRL_S 19

#define MAX_LINES 100
#define MAX_LINE_LEN 256

struct jash_editor_session {
    jash_state_t* state;
    char lines[MAX_LINES][MAX_LINE_LEN];
    int line_count;
    int cursor_line;
    int cursor_col;
    int scroll_offset;
};

jash_editor_session_t* jash_editor_session_create(jash_state_t* s) {
    jash_editor_session_t* es = (jash_editor_session_t*)malloc(sizeof(jash_editor_session_t));
    if (!es) return NULL;
    es->state = s;
    es->line_count = 0;
    es->cursor_line = 0;
    es->cursor_col = 0;
    es->scroll_offset = 0;
    return es;
}

void jash_editor_session_destroy(jash_editor_session_t* es) {
    if (es) free(es);
}

static void render_editor(jash_editor_session_t* es, jash_writer_t w, void* ctx) {
    const jash_theme_t* theme = jash_theme_current();
    
    /* Clear screen */
    jash_writes(w, ctx, "\033[2J\033[H");
    
    /* Header */
    jash_writes(w, ctx, theme->role_accent);
    jash_writes(w, ctx, "  JASH SNIPPET EDITOR\r\n");
    jash_writes(w, ctx, "\033[0m");
    jash_writes(w, ctx, theme->role_muted);
    jash_writes(w, ctx, "  Ctrl+S=Save  Ctrl+C/Esc=Cancel  Enter=New Line\r\n");
    jash_writes(w, ctx, "\033[90m────────────────────────────────────────\033[0m\r\n");
    
    /* Lines */
    int visible_lines = 20;
    int start = es->scroll_offset;
    int end = start + visible_lines;
    if (end > es->line_count) end = es->line_count;
    
    for (int i = start; i < end; i++) {
        /* Line number */
        jash_writes(w, ctx, theme->role_muted);
        char linenum[16];
        snprintf(linenum, sizeof(linenum), "%3d ", i + 1);
        jash_writes(w, ctx, linenum);
        jash_writes(w, ctx, "\033[0m");
        
        /* Line content */
        if (i == es->cursor_line) {
            jash_writes(w, ctx, theme->role_accent);
            jash_writes(w, ctx, "▸ ");
        } else {
            jash_writes(w, ctx, "  ");
        }
        jash_writes(w, ctx, es->lines[i]);
        jash_writes(w, ctx, "\r\n");
    }
    
    /* Footer */
    jash_writes(w, ctx, "\033[90m────────────────────────────────────────\033[0m\r\n");
    jash_writes(w, ctx, theme->role_muted);
    char status[64];
    snprintf(status, sizeof(status), "Line %d/%d  Col %d", 
             es->cursor_line + 1, es->line_count, es->cursor_col);
    jash_writes(w, ctx, status);
    jash_writes(w, ctx, "\033[0m");
}

char* jash_editor_session_run(jash_editor_session_t* es,
                              jash_reader_t r, void* rctx,
                              jash_writer_t w, void* wctx,
                              const char* initial) {
    if (!es || !r || !w) return NULL;
    
    /* Initialize with content */
    es->line_count = 0;
    es->cursor_line = 0;
    es->cursor_col = 0;
    es->scroll_offset = 0;
    
    if (initial && *initial) {
        const char* p = initial;
        while (*p && es->line_count < MAX_LINES) {
            int len = 0;
            while (p[len] && p[len] != '\n' && len < MAX_LINE_LEN - 1) len++;
            memcpy(es->lines[es->line_count], p, len);
            es->lines[es->line_count][len] = '\0';
            es->line_count++;
            p += len;
            if (*p == '\n') p++;
        }
    }
    
    if (es->line_count == 0) {
        es->lines[0][0] = '\0';
        es->line_count = 1;
    }
    
    /* Editor loop */
    jash_keydec_t dec;
    jash_keydec_init(&dec);
    
    for (;;) {
        render_editor(es, w, wctx);
        
        char b;
        if (r(rctx, &b) <= 0) break;
        int key = jash_keydec_feed(&dec, (uint8_t)b);
        if (key == 0) continue;
        
        /* Ctrl+S: Save */
        if (key == JK_CTRL_S) {
            /* Build result */
            size_t total = 0;
            for (int i = 0; i < es->line_count; i++) {
                total += strlen(es->lines[i]) + 1;
            }
            char* result = (char*)malloc(total + 1);
            if (!result) return NULL;
            
            size_t pos = 0;
            for (int i = 0; i < es->line_count; i++) {
                size_t len = strlen(es->lines[i]);
                memcpy(result + pos, es->lines[i], len);
                pos += len;
                if (i < es->line_count - 1) {
                    result[pos++] = '\n';
                }
            }
            result[pos] = '\0';
            
            /* Clear screen */
            jash_writes(w, wctx, "\033[2J\033[H");
            return result;
        }
        
        /* Ctrl+C or Esc: Cancel */
        if (key == JK_CTRL_C || key == JK_ESC) {
            jash_writes(w, wctx, "\033[2J\033[H");
            return NULL;
        }
        
        /* Enter: New line */
        if (key == JK_ENTER) {
            if (es->line_count < MAX_LINES) {
                /* Shift lines down */
                for (int i = es->line_count; i > es->cursor_line; i--) {
                    memcpy(es->lines[i], es->lines[i-1], MAX_LINE_LEN);
                }
                /* Split current line */
                int col = es->cursor_col;
                int rest_len = strlen(es->lines[es->cursor_line]) - col;
                if (rest_len > 0) {
                    memcpy(es->lines[es->cursor_line + 1], 
                           es->lines[es->cursor_line] + col, rest_len);
                    es->lines[es->cursor_line + 1][rest_len] = '\0';
                    es->lines[es->cursor_line][col] = '\0';
                } else {
                    es->lines[es->cursor_line + 1][0] = '\0';
                }
                es->line_count++;
                es->cursor_line++;
                es->cursor_col = 0;
            }
            continue;
        }
        
        /* Backspace */
        if (key == JK_BACKSPACE) {
            if (es->cursor_col > 0) {
                /* Delete char before cursor */
                int len = strlen(es->lines[es->cursor_line]);
                memmove(es->lines[es->cursor_line] + es->cursor_col - 1,
                        es->lines[es->cursor_line] + es->cursor_col,
                        len - es->cursor_col);
                es->lines[es->cursor_line][len - 1] = '\0';
                es->cursor_col--;
            } else if (es->cursor_line > 0) {
                /* Join with previous line */
                int prev_len = strlen(es->lines[es->cursor_line - 1]);
                int cur_len = strlen(es->lines[es->cursor_line]);
                if (prev_len + cur_len < MAX_LINE_LEN) {
                    memcpy(es->lines[es->cursor_line - 1] + prev_len,
                           es->lines[es->cursor_line], cur_len);
                    es->cursor_col = prev_len;
                    es->cursor_line--;
                    /* Shift lines up */
                    for (int i = es->cursor_line; i < es->line_count - 1; i++) {
                        memcpy(es->lines[i], es->lines[i+1], MAX_LINE_LEN);
                    }
                    es->line_count--;
                }
            }
            continue;
        }
        
        /* Arrow keys */
        if (key == JK_UP) {
            if (es->cursor_line > 0) es->cursor_line--;
            continue;
        }
        if (key == JK_DOWN) {
            if (es->cursor_line < es->line_count - 1) es->cursor_line++;
            continue;
        }
        if (key == JK_LEFT) {
            if (es->cursor_col > 0) es->cursor_col--;
            continue;
        }
        if (key == JK_RIGHT) {
            int len = strlen(es->lines[es->cursor_line]);
            if (es->cursor_col < len) es->cursor_col++;
            continue;
        }
        
        /* Printable character */
        if (key >= 0x20 && key < 0x7F) {
            int len = strlen(es->lines[es->cursor_line]);
            if (len < MAX_LINE_LEN - 1) {
                memmove(es->lines[es->cursor_line] + es->cursor_col + 1,
                        es->lines[es->cursor_line] + es->cursor_col,
                        len - es->cursor_col);
                es->lines[es->cursor_line][es->cursor_col] = (char)key;
                es->cursor_col++;
            }
            continue;
        }
    }
    
    return NULL;
}
