// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_spinner — toggle the idle prompt spinner
//
// Usage:
//   spinner                  Show current status
//   spinner on               Enable the idle spinner
//   spinner off              Disable the idle spinner
//   spinner toggle           Toggle it
//   spinner help             Show help
//
// The spinner itself (jash_prompt_spinner_enabled/_advance, theme/prompt.c)
// already reads JASH_PROMPT_SPINNER fresh on every check — no caching, so
// this builtin only needs to set the env var. Some people don't like
// animated prompts; this is the off switch without editing dotfiles.
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <theme/prompt.h>
#include <string.h>

int jash_b_spinner(jash_state_t* s, jash_writer_t w, void* ctx,
                   const char* args) {
    if (!s || !w) return 1;

    while (args && (*args == ' ' || *args == '\t')) args++;

    if (!args || !*args) {
        jash_writef(w, ctx, "[spinner] %s\r\n",
                    jash_prompt_spinner_enabled(s) ? "on" : "off");
        return 0;
    }

    if (args[0] == 'h' && args[1] == 'e' && args[2] == 'l' && args[3] == 'p') {
        jash_writes(w, ctx, "Usage:\r\n");
        jash_writes(w, ctx, "  spinner                Show current status\r\n");
        jash_writes(w, ctx, "  spinner on             Enable the idle spinner\r\n");
        jash_writes(w, ctx, "  spinner off            Disable the idle spinner\r\n");
        jash_writes(w, ctx, "  spinner toggle         Toggle it\r\n");
        return 0;
    }

    if (args[0] == 'o' && args[1] == 'n' && (args[2] == '\0' || args[2] == ' ')) {
        jash_env_set(s, "JASH_PROMPT_SPINNER", "1");
        return 0;
    }

    if (args[0] == 'o' && args[1] == 'f' && args[2] == 'f' &&
        (args[3] == '\0' || args[3] == ' ')) {
        jash_env_set(s, "JASH_PROMPT_SPINNER", "0");
        return 0;
    }

    if (args[0] == 't' && args[1] == 'o' && args[2] == 'g' && args[3] == 'g' &&
        args[4] == 'l' && args[5] == 'e' && (args[6] == '\0' || args[6] == ' ')) {
        jash_env_set(s, "JASH_PROMPT_SPINNER",
                     jash_prompt_spinner_enabled(s) ? "0" : "1");
        return 0;
    }

    jash_writes(w, ctx, "spinner: unknown argument (try 'spinner help')\r\n");
    return 1;
}
