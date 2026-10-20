// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — theme/colors — ANSI escape tokens.
//
// J_ROLE_* macros provide compile-time default colors.
// The runtime theme system (theme/theme.h) overrides these via
// jash_theme_current() pointers. Files that use runtime themes
// should use theme->role_* instead of J_ROLE_*.
//
// J_ROLE_* is kept for backward compatibility in builtins, parsers,
// and other code that hasn't migrated to runtime themes yet.

#ifndef JASH_THEME_COLORS_H
#define JASH_THEME_COLORS_H

#define J_ESC          "\033["

#define J_RESET        J_ESC "0m"
#define J_BOLD         J_ESC "1m"
#define J_DIM          J_ESC "2m"
#define J_ITAL         J_ESC "3m"
#define J_UL           J_ESC "4m"
#define J_BLINK        J_ESC "5m"
#define J_INV          J_ESC "7m"

#define J_BLACK        J_ESC "30m"
#define J_RED          J_ESC "31m"
#define J_GREEN        J_ESC "32m"
#define J_YELLOW       J_ESC "33m"
#define J_BLUE         J_ESC "34m"
#define J_MAGENTA      J_ESC "35m"
#define J_CYAN         J_ESC "36m"
#define J_WHITE        J_ESC "37m"

#define J_BBLACK       J_ESC "90m"
#define J_BRED         J_ESC "91m"
#define J_BGREEN       J_ESC "92m"
#define J_BYELLOW      J_ESC "93m"
#define J_BBLUE        J_ESC "94m"
#define J_BMAGENTA     J_ESC "95m"
#define J_BCYAN        J_ESC "96m"
#define J_BWHITE       J_ESC "97m"

#define J_BG_BLACK     J_ESC "40m"
#define J_BG_RED       J_ESC "41m"
#define J_BG_GREEN     J_ESC "42m"
#define J_BG_YELLOW    J_ESC "43m"
#define J_BG_BLUE      J_ESC "44m"
#define J_BG_MAGENTA   J_ESC "45m"
#define J_BG_CYAN      J_ESC "46m"
#define J_BG_WHITE     J_ESC "47m"

#define J_RGB_FMT      J_ESC "38;2;%d;%d;%dm"
#define J_BG_RGB_FMT   J_ESC "48;2;%d;%d;%dm"

#define J_CLEAR_SCREEN  J_ESC "2J"
#define J_CURSOR_HOME   J_ESC "H"
#define J_HIDE_CURSOR   J_ESC "?25l"
#define J_SHOW_CURSOR   J_ESC "?25h"

/* Semantic roles — compile-time defaults (midnight theme).
 * Runtime themes override these via jash_theme_current()->role_*. */
#define J_ROLE_PROMPT   J_BCYAN
#define J_ROLE_USER     J_BGREEN
#define J_ROLE_HOST     J_CYAN
#define J_ROLE_PATH     J_BBLUE
#define J_ROLE_SIGIL    J_BWHITE
#define J_ROLE_ACCENT   J_BMAGENTA
#define J_ROLE_OK       J_GREEN
#define J_ROLE_WARN     J_YELLOW
#define J_ROLE_ERR      J_BRED
#define J_ROLE_MUTED    J_BBLACK
#define J_ROLE_BANNER   J_BCYAN
#define J_ROLE_LICENSE  J_BMAGENTA

#endif /* JASH_THEME_COLORS_H */
