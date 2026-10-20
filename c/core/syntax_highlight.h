// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Syntax Highlight header

#ifndef JASH_SYNTAX_HIGHLIGHT_H
#define JASH_SYNTAX_HIGHLIGHT_H

// Highlight a line with ANSI colors
// Returns newly allocated string - caller must free with syntax_highlight_free()
char* syntax_highlight(const char* input);

// Free highlighted string
void syntax_highlight_free(char* highlighted);

#endif // JASH_SYNTAX_HIGHLIGHT_H
