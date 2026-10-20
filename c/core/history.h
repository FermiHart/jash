// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// History header

#ifndef JASH_HISTORY_H
#define JASH_HISTORY_H

#include <stdint.h>
#include <stddef.h>

// Initialize history (load from file)
void history_init(void);

// Save history to file
void history_save(void);

// Add command to history
void history_add(const char* cmd);

// Navigate history (up/down arrows)
const char* history_prev(void);
const char* history_next(void);

// Search history (Ctrl+R style)
const char* history_search(const char* prefix);

// Fuzzy search (for autosuggestion)
const char* history_fuzzy_search(const char* partial);

// Get history count
int history_count(void);

// Get command at index
const char* history_get(int index);

// Obsidian integration
void obsidian_log_command(const char* cmd);
void obsidian_analyze_patterns(void);

// Cleanup
void history_cleanup(void);

// Wrapper functions for compatibility with existing code
struct jash_state;
typedef struct jash_state jash_state_t;

void jash_history_clear(jash_state_t* s);
uint32_t jash_history_count(jash_state_t* s);
const char* jash_history_at(jash_state_t* s, uint32_t index);
void jash_history_load(jash_state_t* s, const char* path);
void jash_history_add(jash_state_t* s, const char* cmd);
void jash_history_push(jash_state_t* s, const char* cmd);
void jash_history_save(jash_state_t* s, const char* path);
int jash_history_expand(jash_state_t* s, const char* line, char* buf, size_t buflen);

#endif // JASH_HISTORY_H
