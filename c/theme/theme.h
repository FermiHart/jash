// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — theme system — runtime-switchable color themes.
//
// Fish has no built-in theme system (needs external config).
// Jash ships 4 themes, switchable at runtime with `theme NAME`.
// Colors update instantly — no restart needed.

#ifndef JASH_THEME_THEME_H
#define JASH_THEME_THEME_H

#include <core/state.h>

typedef struct {
    const char* name;
    const char* desc;
    /* ANSI escape codes for each semantic role */
    const char* role_user;
    const char* role_host;
    const char* role_path;
    const char* role_sigil;
    const char* role_accent;
    const char* role_ok;
    const char* role_warn;
    const char* role_err;
    const char* role_muted;
    const char* role_banner;
    /* Syntax highlighting */
    const char* syn_builtin;
    const char* syn_command;
    const char* syn_flag;
    const char* syn_redirect;
    const char* syn_string;
    const char* syn_comment;
    const char* syn_variable;
    const char* syn_number;
} jash_theme_t;

/* Available themes */
extern const jash_theme_t JASH_THEME_MIDNIGHT;
extern const jash_theme_t JASH_THEME_SYNTHWAVE;
extern const jash_theme_t JASH_THEME_AMBER;
extern const jash_theme_t JASH_THEME_1990;
extern const jash_theme_t JASH_THEME_DRACULA;
extern const jash_theme_t JASH_THEME_NORD;
extern const jash_theme_t JASH_THEME_SOLARIZED;
extern const jash_theme_t JASH_THEME_MONOKAI;
extern const jash_theme_t JASH_THEME_AI_BORDEAUX;

#define JASH_THEME_COUNT 8

/* Get current theme (returns pointer to static theme struct) */
const jash_theme_t* jash_theme_current(void);

/* Set current theme by name. Returns 0 on success, -1 if not found. */
int jash_theme_set(const char* name);

/* List all available theme names (writes to out, returns count) */
int jash_theme_list(char out[][32], int max);

/* Initialize theme from JASH_THEME env var */
void jash_theme_init(jash_state_t* s);

#endif /* JASH_THEME_THEME_H */
