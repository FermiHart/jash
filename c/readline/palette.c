// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — palette — VS Code-style command palette (impl).

#include "palette.h"
#include "keymap.h"
#include "jux.h"
#include "autopair.h"
#include <builtins/builtin.h>
#include <core/history.h>
#include <theme/theme.h>
#include <theme/io.h>
#include <string.h>

#define PAL_MAX_CMDS 128
#define PAL_MAX_NAME 64
#define PAL_MAX_DESC 96

typedef struct {
    char name[PAL_MAX_NAME];
    char desc[PAL_MAX_DESC];
} pal_cmd_t;

static void pal_load_commands(jash_state_t* s, pal_cmd_t* cmds, int* count) {
    int n = 0;

    static const char* builtins[][2] = {
        {"theme", "Switch color theme (midnight/synthwave/amber/1990/dracula/nord/solarized/monokai)"},
        {"clear", "Clear the terminal screen"},
        {"help", "Show available commands"},
        {"version", "Show jash version"},
        {"history", "Show command history"},
        {"jobs", "List background jobs"},
        {"exit", "Exit the shell"},
        {"export", "Export environment variable"},
        {"alias", "Create or list aliases"},
        {"unalias", "Remove an alias"},
        {"source", "Execute commands from a file"},
        {"eval", "Evaluate arguments as a command"},
        {"cd", "Change directory"},
        {"pwd", "Print working directory"},
        {"echo", "Print arguments"},
        {"printf", "Formatted output"},
        {"read", "Read a line from input"},
        {"set", "Set shell options or positional params"},
        {"shopt", "Shell options"},
        {"trap", "Set signal handlers"},
        {"type", "Show command type"},
        {"env", "Show environment"},
        {"unset", "Remove variable"},
        {"pushd", "Push directory to stack"},
        {"popd", "Pop directory from stack"},
        {"dirs", "Show directory stack"},
        {"declare", "Declare variables"},
        {"readonly", "Make variable readonly"},
        {"local", "Declare local variable"},
        {"let", "Evaluate arithmetic expression"},
        {"test", "Test condition"},
        {"shift", "Shift positional parameters"},
        {"getopts", "Parse positional parameters"},
        {"mapfile", "Read lines into array"},
        {"exec", "Replace shell with command"},
        {"builtin", "Execute builtin command"},
        {"enable", "Enable/disable builtins"},
        {"hash", "Cache command paths"},
        {"logout", "Exit login shell"},
        {"wait", "Wait for background jobs"},
        {"fg", "Bring job to foreground"},
        {"bg", "Resume job in background"},
        {"disown", "Remove job from table"},
        {"suspend", "Suspend the shell"},
        {"abbr", "Manage abbreviations"},
        {"juxconfig", "Configure interactive UX"},
        {"calc", "Inline calculator"},
        {"jash_state", "Save/restore shell state"},
        {"select", "Select from a list"},
        {"command", "Execute command (bypass shell)"},
        {"break", "Break out of loop"},
        {"continue", "Continue to next iteration"},
        {"return", "Return from function"},
        {"ulimit", "Set resource limits"},
        {"umask", "Set file creation mask"},
    };
    int nb = (int)(sizeof(builtins) / sizeof(builtins[0]));
    for (int i = 0; i < nb && n < PAL_MAX_CMDS; i++) {
        int j = 0;
        while (builtins[i][0][j] && j < PAL_MAX_NAME - 1) { cmds[n].name[j] = builtins[i][0][j]; j++; }
        cmds[n].name[j] = '\0';
        j = 0;
        while (builtins[i][1][j] && j < PAL_MAX_DESC - 1) { cmds[n].desc[j] = builtins[i][1][j]; j++; }
        cmds[n].desc[j] = '\0';
        n++;
    }

    /* Recent history entries */
    uint32_t hc = jash_history_count(s);
    int hist_start = hc > 10 ? (int)hc - 10 : 0;
    for (int i = hist_start; i < (int)hc && n < PAL_MAX_CMDS; i++) {
        const char* h = jash_history_at(s, (uint32_t)i);
        if (!h || !*h) continue;
        int k = 0;
        while (h[k] && k < PAL_MAX_NAME - 1) { cmds[n].name[k] = h[k]; k++; }
        cmds[n].name[k] = '\0';
        k = 0;
        const char* desc = "history entry";
        while (desc[k]) { cmds[n].desc[k] = desc[k]; k++; }
        cmds[n].desc[k] = '\0';
        n++;
    }

    *count = n;
}

static int fuzzy_match(const char* text, const char* query) {
    if (!query || !*query) return 1;
    const char* t = text;
    const char* q = query;
    while (*q && *t) {
        char a = *t, b = *q;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a == b) q++;
        t++;
    }
    return *q == 0;
}

int jash_palette_open(jash_state_t* s,
                      jash_reader_t r, void* rctx,
                      jash_writer_t w, void* wctx,
                      jash_line_t* L) {
    pal_cmd_t cmds[PAL_MAX_CMDS];
    int cmd_count = 0;
    pal_load_commands(s, cmds, &cmd_count);

    char query[PAL_MAX_NAME];
    int qlen = 0;
    int selected = 0;

    char saved_line[JASH_LINE_MAX];
    uint32_t saved_len = L->len;
    uint32_t saved_cur = L->cur;
    memcpy(saved_line, L->buf, L->len);

    jash_keydec_t dec;
    jash_keydec_init(&dec);

    for (;;) {
        int filtered[PAL_MAX_CMDS];
        int fcount = 0;
        for (int i = 0; i < cmd_count; i++) {
            if (fuzzy_match(cmds[i].name, query)) {
                filtered[fcount++] = i;
            }
        }
        if (selected >= fcount) selected = fcount - 1;
        if (selected < 0) selected = 0;

        jash_writes(w, wctx, "\r\033[K");
        jash_writes(w, wctx, "\033[1;96m  \xe2\x96\x81\xe2\x96\x81\xe2\x96\x81 JASH COMMAND PALETTE \xe2\x96\x81\xe2\x96\x81\xe2\x96\x81\033[0m\r\n");
        jash_writes(w, wctx, "  \033[90m>\033[0m ");
        jash_writes_sanitized(w, wctx, query);
        jash_writes(w, wctx, "\033[90m_\033[0m\r\n");
        jash_writes(w, wctx, "\033[90m  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\033[0m\r\n");

        int max_show = 8;
        int start = 0;
        if (fcount > max_show) {
            start = selected - max_show / 2;
            if (start < 0) start = 0;
            if (start + max_show > fcount) start = fcount - max_show;
        }
        for (int i = start; i < fcount && i < start + max_show; i++) {
            if (i == selected) {
                jash_writes(w, wctx, "\033[48;2;60;60;80m");
            }
            jash_writes(w, wctx, "  ");
            jash_writes_sanitized(w, wctx, cmds[filtered[i]].name);
            jash_writes(w, wctx, "\033[0m");
            jash_writes(w, wctx, "\033[90m");
            unsigned nlen = 0;
            { const char* tmp = cmds[filtered[i]].name; while (*tmp) { nlen++; tmp++; } }
            int pad = 36 - (int)nlen;
            if (pad > 0) {
                /* one write for the whole padding run, not one per space */
                char padbuf[37];
                memset(padbuf, ' ', (size_t)pad);
                padbuf[pad] = '\0';
                jash_writes(w, wctx, padbuf);
            }
            jash_writes_sanitized(w, wctx, cmds[filtered[i]].desc);
            jash_writes(w, wctx, "\033[0m");
            jash_writes(w, wctx, "\r\n");
        }
        if (fcount > max_show) {
            jash_writes(w, wctx, "\033[90m  ... and more\033[0m\r\n");
        }

        jash_writes(w, wctx, "\033[90m  Esc=cancel  Enter=select  Up/Down=navigate  Tab=autocomplete\033[0m");

        char b;
        if (r(rctx, &b) <= 0) break;
        int key = jash_keydec_feed(&dec, (uint8_t)b);
        if (key == 0) continue;
        if (key == JK_ESC || key == JK_CTRL_C) {
            L->len = saved_len;
            L->cur = saved_cur;
            memcpy(L->buf, saved_line, saved_len);
            L->buf[L->len] = 0;
            return 0;
        }
        if (key == JK_ENTER) {
            if (fcount > 0 && selected >= 0) {
                L->len = 0;
                L->cur = 0;
                const char* sel = cmds[filtered[selected]].name;
                while (*sel && L->len + 1 < JASH_LINE_MAX)
                    L->buf[L->len++] = *sel++;
                L->cur = L->len;
                L->buf[L->len] = 0;
                return 1;
            }
            return 0;
        }
        if (key == JK_UP || key == JK_CTRL_P) {
            if (selected > 0) selected--;
            continue;
        }
        if (key == JK_DOWN || key == JK_CTRL_N) {
            if (selected < fcount - 1) selected++;
            continue;
        }
        if (key == JK_BACKSPACE || key == 0x7F) {
            if (qlen > 0) { qlen--; query[qlen] = 0; selected = 0; }
            continue;
        }
        if (key == JK_TAB) {
            if (fcount > 0) {
                selected = (selected + 1) % fcount;
            }
            continue;
        }
        if (key >= 0x20 && key < 0x7F && qlen < PAL_MAX_NAME - 1) {
            query[qlen++] = (char)key;
            query[qlen] = 0;
            selected = 0;
        }
    }
    return 0;
}
