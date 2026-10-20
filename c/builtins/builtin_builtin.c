// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: builtin — force execution of a builtin, bypassing functions

#include "builtin.h"
#include <parser/quotes.h>
#include <string.h>

int jash_b_builtin_cmd(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* args) {
    if (!args || !*args) {
        jash_writes(jash_stderr_writer, 0, "builtin: usage: builtin COMMAND [ARGS...]\n");
        return 2;
    }

    /* Extract command name */
    while (*args == ' ' || *args == '\t') args++;
    char raw[JASH_CMDWORD_MAX], cmd[64];
    int word_error = 0;
    args = jash_first_word(args, raw, sizeof(raw), 1, &word_error);

    if (word_error || jash_unquote(raw, cmd, sizeof(cmd)) < 0 || !cmd[0]) {
        jash_writes(jash_stderr_writer, 0, "builtin: usage: builtin COMMAND [ARGS...]\n");
        return 2;
    }

    /* Skip whitespace to get remaining args */
    while (*args == ' ' || *args == '\t') args++;

    /* Look up the builtin directly in the registry */
    const jash_builtin_t* b = jash_builtin_lookup(cmd);
    if (!b) {
        jash_writef(jash_stderr_writer, 0, "builtin: %s: not a builtin\n", cmd);
        return 1;
    }

    return jash_builtin_dispatch(b, s, w, ctx, args);
}
