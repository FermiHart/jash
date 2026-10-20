// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Builtin: theme — switch color themes at runtime.

#include "builtin.h"
#include <theme/theme.h>
#include <theme/io.h>
#include <core/env.h>
#include <string.h>

int jash_b_theme(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;

    /* Trim trailing whitespace */
    char theme_name[64];
    int len = 0;
    while (p[len] && len < 63) len++;
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t')) len--;
    
    /* Copy trimmed name */
    for (int i = 0; i < len; i++) theme_name[i] = p[i];
    theme_name[len] = '\0';
    p = theme_name;

    if (!*p || strcmp(p, "--list") == 0) {
        jash_writes(w, ctx, "\033[1;96mAvailable themes:\033[0m\r\n");
        char names[8][32];
        int count = jash_theme_list(names, 8);
        const jash_theme_t* cur = jash_theme_current();
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], cur->name) == 0) {
                jash_writef(w, ctx, "  \033[1;92m* %s\033[0m — %s\r\n", names[i], cur->desc);
            } else {
                jash_writef(w, ctx, "    %s\r\n", names[i]);
            }
        }
        jash_writes(w, ctx, "\r\n\033[90mUsage: theme NAME\033[0m\r\n");
        return 0;
    }

    if (strcmp(p, "--help") == 0 || strcmp(p, "-h") == 0) {
        jash_writes(w, ctx, "theme — switch color themes\r\n\r\n");
        jash_writes(w, ctx, "Usage:\r\n");
        jash_writes(w, ctx, "  theme           List available themes\r\n");
        jash_writes(w, ctx, "  theme NAME      Switch to theme NAME\r\n");
        jash_writes(w, ctx, "  theme --current Show current theme\r\n");
        return 0;
    }

    if (strcmp(p, "--current") == 0) {
        const jash_theme_t* cur = jash_theme_current();
        jash_writef(w, ctx, "Current theme: \033[1m%s\033[0m — %s\r\n", cur->name, cur->desc);
        return 0;
    }

    if (jash_theme_set(p) == 0) {
        jash_env_set(s, "JASH_THEME", p);
        const jash_theme_t* cur = jash_theme_current();
        jash_writef(w, ctx, "Theme switched to \033[1;92m%s\033[0m — %s\r\n", cur->name, cur->desc);
        return 0;
    }

    jash_writef(w, ctx, "theme: unknown theme '%s'\r\n", p);
    jash_writes(w, ctx, "Run 'theme --list' to see available themes.\r\n");
    return 1;
}
