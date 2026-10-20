// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/arithmetic — arithmetic expansion $((expr))

#ifndef JASH_PARSER_ARITHMETIC_H
#define JASH_PARSER_ARITHMETIC_H

/* Evaluate arithmetic expression and return result.
 * Supports: +, -, *, /, %, parentheses, variables ($VAR)
 * Example: "5 + 3 * 2" → 11
 * Returns: result, or 0 on error */
long jash_eval_arithmetic(const char* expr, void* state);

/* Check if string contains $((  ... )) pattern */
int jash_has_arithmetic(const char* str);

/* Expand $((expr)) in a string, replacing with result.
 * Returns: pointer to static buffer with expanded string */
const char* jash_expand_arithmetic(const char* str, void* state);
/* Caller already found arithmetic expansion in this exact input. Skips only
 * the presence scan; like the checked entry, does not clear the error channel. */
const char* jash_expand_arithmetic_known(const char* str, void* state);

/* Division-by-zero error channel (bash parity). Clear before an expansion pass,
 * then check after: if set, a `/0` or `%0` occurred — emit an error and rc 1. */
void jash_arith_clear_error(void);
int  jash_arith_error(void);

#endif
