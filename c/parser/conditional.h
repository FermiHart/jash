// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Parser: [[ ... ]] conditional expressions

#ifndef JASH_PARSER_CONDITIONAL_H
#define JASH_PARSER_CONDITIONAL_H

#include <core/state.h>

/* jash_parse_conditional: evaluate [[ expr ]]
 * Returns 1 (true) or 0 (false)
 *
 * Operators:
 *   -z "str"     — string is empty
 *   -n "str"     — string is non-empty
 *   -e "file"    — file exists
 *   -f "file"    — is regular file
 *   -d "dir"     — is directory
 *   str == pat   — string matches pattern
 *   str != pat   — string does not match
 *   str =~ pat   — POSIX extended regex; sets BASH_REMATCH[0..N]
 *   -eq, -ne, -lt, -gt, -le, -ge for numeric
 *   && (and), || (or)
 */
int jash_parse_conditional(const char* expr, jash_state_t* s);

#endif /* JASH_PARSER_CONDITIONAL_H */
