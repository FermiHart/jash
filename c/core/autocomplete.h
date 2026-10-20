// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Autocomplete header

#ifndef JASH_AUTOCOMPLETE_H
#define JASH_AUTOCOMPLETE_H

// Complete file/directory paths
int autocomplete_path(const char* partial, char** matches, int max_matches);

// Complete command names
int autocomplete_command(const char* partial, char** matches, int max_matches);

// Complete flags for commands
int autocomplete_flag(const char* cmd, const char* partial, char** matches, int max_matches);

// Main autocomplete function
int autocomplete(const char* line, int cursor_pos, char** matches, int max_matches);

// Show completions
void autocomplete_show(char** matches, int nmatches);

// Free completions
void autocomplete_free(char** matches, int nmatches);

#endif // JASH_AUTOCOMPLETE_H
