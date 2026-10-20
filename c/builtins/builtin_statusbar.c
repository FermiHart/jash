// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_statusbar — Living Dashboard control
//
// Usage:
//   statusbar                  Show current status
//   statusbar on               Enable status bar
//   statusbar off              Disable status bar
//   statusbar toggle           Toggle status bar
//   statusbar <mode>           Switch to mode (computer/dev/ai/focus/personal/debug)
//   statusbar auto on|off      Enable/disable auto-switching
//   statusbar help             Show help

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <ui/statusbar.h>
#include <string.h>

int jash_b_statusbar(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    if (!s || !w) return 1;

    /* Skip leading whitespace */
    while (args && (*args == ' ' || *args == '\t')) args++;

    /* No arguments: show current status */
    if (!args || !*args) {
        const char* mode_name = jash_statusbar_mode_name(jash_statusbar_get_mode());
        int enabled = jash_statusbar_enabled(s);
        int auto_sw = 0; /* TODO: expose auto_switch */
        jash_writef(w, ctx, "[statusbar] %s | %s | auto:%s\r\n",
                    enabled ? "on" : "off", mode_name, auto_sw ? "on" : "off");
        return 0;
    }

    /* statusbar help */
    if (args[0] == 'h' && args[1] == 'e' && args[2] == 'l' && args[3] == 'p') {
        jash_writes(w, ctx, "Usage:\r\n");
        jash_writes(w, ctx, "  statusbar                Show current status\r\n");
        jash_writes(w, ctx, "  statusbar on             Enable status bar\r\n");
        jash_writes(w, ctx, "  statusbar off            Disable status bar\r\n");
        jash_writes(w, ctx, "  statusbar toggle         Toggle status bar\r\n");
        jash_writes(w, ctx, "  statusbar computer       Computer mode (CPU/RAM/disk)\r\n");
        jash_writes(w, ctx, "  statusbar dev            Dev mode (git/project)\r\n");
        jash_writes(w, ctx, "  statusbar ai             AI mode (agent status)\r\n");
        jash_writes(w, ctx, "  statusbar focus          Focus mode (minimal)\r\n");
        jash_writes(w, ctx, "  statusbar personal       Personal mode (weather/news)\r\n");
        jash_writes(w, ctx, "  statusbar debug          Debug mode (internals)\r\n");
        jash_writes(w, ctx, "  statusbar auto on|off    Auto-switch modes\r\n");
        return 0;
    }

    /* statusbar on */
    if (args[0] == 'o' && args[1] == 'n' && (args[2] == '\0' || args[2] == ' ')) {
        jash_statusbar_set_enabled(s, 1);
        return 0;
    }

    /* statusbar off */
    if (args[0] == 'o' && args[1] == 'f' && args[2] == 'f' &&
        (args[3] == '\0' || args[3] == ' ')) {
        jash_statusbar_set_enabled(s, 0);
        return 0;
    }

    /* statusbar toggle */
    if (args[0] == 't' && args[1] == 'o' && args[2] == 'g' && args[3] == 'g' &&
        args[4] == 'l' && args[5] == 'e' && (args[6] == '\0' || args[6] == ' ')) {
        jash_statusbar_set_enabled(s, !jash_statusbar_enabled(s));
        return 0;
    }

    /* statusbar auto on|off */
    if (args[0] == 'a' && args[1] == 'u' && args[2] == 't' && args[3] == 'o' &&
        (args[4] == '\0' || args[4] == ' ')) {
        const char* sub = args + 4;
        while (*sub == ' ') sub++;
        if (strncmp(sub, "on", 2) == 0 && (sub[2] == '\0' || sub[2] == ' ')) {
            jash_statusbar_set_auto_switch(1);
            jash_writes(w, ctx, "auto-switch: on\r\n");
            return 0;
        }
        if (strncmp(sub, "off", 3) == 0 && (sub[3] == '\0' || sub[3] == ' ')) {
            jash_statusbar_set_auto_switch(0);
            jash_writes(w, ctx, "auto-switch: off\r\n");
            return 0;
        }
    }

    /* statusbar <mode> — check if it's a valid mode name */
    const char* mode_names[] = {"computer", "dev", "ai", "focus", "personal", "debug"};
    for (int i = 0; i < 6; i++) {
        if (strncmp(args, mode_names[i], strlen(mode_names[i])) == 0 &&
            (args[strlen(mode_names[i])] == '\0' || args[strlen(mode_names[i])] == ' ')) {
            jash_statusbar_set_mode((jash_sb_mode_t)i);
            jash_statusbar_mark_dirty();
            /* Force immediate render */
            jash_writes(w, ctx, "\0337");  /* Save cursor */
            jash_writes(w, ctx, "\033[1;1H");  /* Go to top line */
            jash_writes(w, ctx, "\033[2K");  /* Clear line */
            jash_writes(w, ctx, "\0338");  /* Restore cursor */
            return 0;
        }
    }

    /* Unknown command */
    jash_writes(w, ctx, "statusbar: unknown command '");
    jash_writes_sanitized(w, ctx, args);
    jash_writes(w, ctx, "'\r\n");
    jash_writes(w, ctx, "Try 'statusbar help'\r\n");
    return 1;
}
