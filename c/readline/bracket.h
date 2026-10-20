// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — bracket — bracket matching visual.
//
// When cursor is on (, ), [, ], {, }, highlight the matching pair.
// This is what most editors do — but no shell does it inline.

#ifndef JASH_READLINE_BRACKET_H
#define JASH_READLINE_BRACKET_H

#include "edit.h"

/* Find the matching bracket position for L->buf[pos].
 * Returns the index of the matching bracket, or -1 if none.
 * Handles nesting. */
int jash_bracket_match(const jash_line_t* L, uint32_t pos);

/* Check if a position is a bracket character */
int jash_is_bracket(char c);

#endif /* JASH_READLINE_BRACKET_H */
