// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — autopair — auto-close brackets, quotes, parentheses.
//
// When user types (, [, {, ", ' — auto-insert closing pair.
// When user types closing pair that matches — skip over it.
// Smart: don't pair inside quotes, don't pair after backslash.

#ifndef JASH_READLINE_AUTOPAIR_H
#define JASH_READLINE_AUTOPAIR_H

#include "edit.h"

/* Initialize autopair state (call once at startup) */
void jash_autopair_init(void);

/* Enable/disable autopairing (default: enabled) */
void jash_autopair_set_enabled(int enabled);

/* Process a character through autopair.
 * Returns:
 *   0 = no autopair action (normal insert)
 *   1 = autopair inserted closing char (caller should NOT insert)
 *   2 = autopair skipped over existing closing char (cursor moved)
 *   3 = autopair removed matching pair (backspace case)
 * The line buffer is modified in-place. */
int jash_autopair_feed(jash_line_t* L, char c);

/* Handle backspace with autopair — if cursor is between matching pair,
 * delete both. Returns 1 if pair was deleted, 0 otherwise. */
int jash_autopair_backspace(jash_line_t* L);

#endif /* JASH_READLINE_AUTOPAIR_H */
