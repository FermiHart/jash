// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — theme system — runtime-switchable color themes (impl).

#include "theme.h"
#include <core/env.h>
#include <core/jash_ai.h>
#include <string.h>

extern int g_jash_no_color; /* defined in theme/prompt.c (INV-889) */

/* NO_COLOR/TERM=dumb theme: every role is an empty string, so the
 * prompt renders as plain text with zero ANSI bytes (INV-889).
 * Selected automatically at init when the environment says no-color. */
static const jash_theme_t JASH_THEME_PLAIN = {
    .name = "plain", .desc = "No colors — for dumb terminals and Beyond",
    .role_user = "", .role_host = "", .role_path = "",
    .role_sigil = "", .role_accent = "", .role_ok = "",
    .role_warn = "", .role_err = "", .role_muted = "",
    .role_banner = "",
};

/* ---- MIDNIGHT (cool blues/cyans) ---- */
const jash_theme_t JASH_THEME_MIDNIGHT = {
    .name = "midnight", .desc = "Cool blues and cyans",
    .role_user = "\033[92m", .role_host = "\033[36m", .role_path = "\033[94m",
    .role_sigil = "\033[97m", .role_accent = "\033[95m",
    .role_ok = "\033[32m", .role_warn = "\033[33m", .role_err = "\033[91m",
    .role_muted = "\033[90m", .role_banner = "\033[96m",
    .syn_builtin = "\033[96m", .syn_command = "\033[94m",
    .syn_flag = "\033[33m", .syn_redirect = "\033[35m",
    .syn_string = "\033[32m", .syn_comment = "\033[90m",
    .syn_variable = "\033[95m", .syn_number = "\033[33m",
};

/* ---- SYNTHWAVE (pink/magenta/cyan) ---- */
const jash_theme_t JASH_THEME_SYNTHWAVE = {
    .name = "synthwave", .desc = "Pink, magenta and neon cyan",
    .role_user = "\033[96m", .role_host = "\033[35m", .role_path = "\033[94m",
    .role_sigil = "\033[97m", .role_accent = "\033[93m",
    .role_ok = "\033[92m", .role_warn = "\033[93m", .role_err = "\033[91m",
    .role_muted = "\033[90m", .role_banner = "\033[95m",
    .syn_builtin = "\033[95m", .syn_command = "\033[94m",
    .syn_flag = "\033[93m", .syn_redirect = "\033[35m",
    .syn_string = "\033[32m", .syn_comment = "\033[90m",
    .syn_variable = "\033[96m", .syn_number = "\033[93m",
};

/* ---- AMBER (warm orange/yellow) ---- */
const jash_theme_t JASH_THEME_AMBER = {
    .name = "amber", .desc = "Warm amber and gold",
    .role_user = "\033[33m", .role_host = "\033[93m", .role_path = "\033[33m",
    .role_sigil = "\033[97m", .role_accent = "\033[91m",
    .role_ok = "\033[93m", .role_warn = "\033[91m", .role_err = "\033[31m",
    .role_muted = "\033[33m", .role_banner = "\033[93m",
    .syn_builtin = "\033[93m", .syn_command = "\033[33m",
    .syn_flag = "\033[91m", .syn_redirect = "\033[35m",
    .syn_string = "\033[32m", .syn_comment = "\033[90m",
    .syn_variable = "\033[95m", .syn_number = "\033[93m",
};

/* ---- 1990 (green-on-black, retro VT100) ---- */
const jash_theme_t JASH_THEME_1990 = {
    .name = "1990", .desc = "Retro green phosphor terminal",
    .role_user = "\033[32m", .role_host = "\033[32m", .role_path = "\033[92m",
    .role_sigil = "\033[32m", .role_accent = "\033[92m",
    .role_ok = "\033[32m", .role_warn = "\033[93m", .role_err = "\033[91m",
    .role_muted = "\033[32m", .role_banner = "\033[92m",
    .syn_builtin = "\033[92m", .syn_command = "\033[32m",
    .syn_flag = "\033[93m", .syn_redirect = "\033[35m",
    .syn_string = "\033[32m", .syn_comment = "\033[90m",
    .syn_variable = "\033[92m", .syn_number = "\033[93m",
};

/* ---- DRACULA (popular dark theme) ---- */
const jash_theme_t JASH_THEME_DRACULA = {
    .name = "dracula", .desc = "Dracula color scheme",
    .role_user = "\033[92m", .role_host = "\033[95m", .role_path = "\033[94m",
    .role_sigil = "\033[97m", .role_accent = "\033[96m",
    .role_ok = "\033[92m", .role_warn = "\033[93m", .role_err = "\033[91m",
    .role_muted = "\033[90m", .role_banner = "\033[95m",
    .syn_builtin = "\033[96m", .syn_command = "\033[94m",
    .syn_flag = "\033[93m", .syn_redirect = "\033[35m",
    .syn_string = "\033[92m", .syn_comment = "\033[90m",
    .syn_variable = "\033[95m", .syn_number = "\033[93m",
};

/* ---- NORD (arctic, north-bluish) ---- */
const jash_theme_t JASH_THEME_NORD = {
    .name = "nord", .desc = "Nord arctic color palette",
    .role_user = "\033[94m", .role_host = "\033[36m", .role_path = "\033[96m",
    .role_sigil = "\033[97m", .role_accent = "\033[95m",
    .role_ok = "\033[32m", .role_warn = "\033[33m", .role_err = "\033[91m",
    .role_muted = "\033[90m", .role_banner = "\033[94m",
    .syn_builtin = "\033[36m", .syn_command = "\033[94m",
    .syn_flag = "\033[93m", .syn_redirect = "\033[35m",
    .syn_string = "\033[92m", .syn_comment = "\033[90m",
    .syn_variable = "\033[95m", .syn_number = "\033[93m",
};

/* ---- SOLARIZED ---- */
const jash_theme_t JASH_THEME_SOLARIZED = {
    .name = "solarized", .desc = "Solarized color scheme",
    .role_user = "\033[32m", .role_host = "\033[36m", .role_path = "\033[34m",
    .role_sigil = "\033[37m", .role_accent = "\033[33m",
    .role_ok = "\033[32m", .role_warn = "\033[33m", .role_err = "\033[31m",
    .role_muted = "\033[90m", .role_banner = "\033[36m",
    .syn_builtin = "\033[36m", .syn_command = "\033[34m",
    .syn_flag = "\033[33m", .syn_redirect = "\033[35m",
    .syn_string = "\033[32m", .syn_comment = "\033[90m",
    .syn_variable = "\033[35m", .syn_number = "\033[33m",
};

/* ---- MONOKAI ---- */
const jash_theme_t JASH_THEME_MONOKAI = {
    .name = "monokai", .desc = "Monokai classic",
    .role_user = "\033[92m", .role_host = "\033[95m", .role_path = "\033[93m",
    .role_sigil = "\033[97m", .role_accent = "\033[96m",
    .role_ok = "\033[92m", .role_warn = "\033[93m", .role_err = "\033[91m",
    .role_muted = "\033[90m", .role_banner = "\033[95m",
    .syn_builtin = "\033[96m", .syn_command = "\033[92m",
    .syn_flag = "\033[93m", .syn_redirect = "\033[91m",
    .syn_string = "\033[93m", .syn_comment = "\033[90m",
    .syn_variable = "\033[95m", .syn_number = "\033[93m",
};

/* ---- AI_BORDEAUX (deep wine red — active when AI mode is on) ---- */
const jash_theme_t JASH_THEME_AI_BORDEAUX = {
    .name = "ai-bordeaux", .desc = "Deep wine red for AI mode",
    .role_user = "\033[38;5;124m",   /* Dark wine red */
    .role_host = "\033[38;5;160m",   /* Bright red */
    .role_path = "\033[38;5;196m",   /* Bright crimson */
    .role_sigil = "\033[38;5;203m",  /* Light pink-red */
    .role_accent = "\033[38;5;132m", /* Muted burgundy */
    .role_ok = "\033[38;5;168m",     /* Rose */
    .role_warn = "\033[38;5;215m",  /* Orange-gold */
    .role_err = "\033[38;5;196m",   /* Bright red */
    .role_muted = "\033[38;5;95m",  /* Dark muted red */
    .role_banner = "\033[38;5;160m",
    .syn_builtin = "\033[38;5;203m",
    .syn_command = "\033[38;5;168m",
    .syn_flag = "\033[38;5;215m",
    .syn_redirect = "\033[38;5;132m",
    .syn_string = "\033[38;5;168m",
    .syn_comment = "\033[38;5;95m",
    .syn_variable = "\033[38;5;203m",
    .syn_number = "\033[38;5;215m",
};

static const jash_theme_t* g_current = &JASH_THEME_MIDNIGHT;

static const jash_theme_t* all_themes[JASH_THEME_COUNT];

static void init_theme_table(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;
    all_themes[0] = &JASH_THEME_MIDNIGHT;
    all_themes[1] = &JASH_THEME_SYNTHWAVE;
    all_themes[2] = &JASH_THEME_AMBER;
    all_themes[3] = &JASH_THEME_1990;
    all_themes[4] = &JASH_THEME_DRACULA;
    all_themes[5] = &JASH_THEME_NORD;
    all_themes[6] = &JASH_THEME_SOLARIZED;
    all_themes[7] = &JASH_THEME_MONOKAI;
}

const jash_theme_t* jash_theme_current(void) {
    /* AI mode override: switch to bordeaux theme */
    if (jash_ai_active()) {
        return &JASH_THEME_AI_BORDEAUX;
    }
    return g_current;
}

int jash_theme_set(const char* name) {
    if (!name) return -1;
    init_theme_table();
    for (int i = 0; i < JASH_THEME_COUNT; i++) {
        if (strcmp(all_themes[i]->name, name) == 0) {
            g_current = all_themes[i];
            return 0;
        }
    }
    return -1;
}

int jash_theme_list(char out[][32], int max) {
    init_theme_table();
    int n = JASH_THEME_COUNT < max ? JASH_THEME_COUNT : max;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (all_themes[i]->name[j] && j < 30) {
            out[i][j] = all_themes[i]->name[j];
            j++;
        }
        out[i][j] = '\0';
    }
    return n;
}

void jash_theme_init(jash_state_t* s) {
    init_theme_table();
    const char* t = jash_env_get(s, "JASH_THEME");
    if (t && jash_theme_set(t) == 0) return;
    /* INV-889: TERM=dumb + NO_COLOR select the plain theme so the
     * prompt emits zero ANSI bytes. Under JASH_TARGET_BEYOND, env.c
     * sets both; the serial console shows raw text, not a terminal. */
    const char* no_color = jash_env_get(s, "NO_COLOR");
    const char* term = jash_env_get(s, "TERM");
    if ((no_color && no_color[0]) || (term && strcmp(term, "dumb") == 0)) {
        g_current = &JASH_THEME_PLAIN;
        g_jash_no_color = 1;
        return;
    }
    g_current = &JASH_THEME_MIDNIGHT;
}
