// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash interactive UX (jux) helpers — autosuggest, fuzzy history, abbreviations.

#ifndef JASH_READLINE_JUX_H
#define JASH_READLINE_JUX_H

#include "edit.h"
#include <core/state.h>

void jash_jux_load_config(jash_state_t* s);
int  jash_jux_autosuggest(jash_state_t* s, const jash_line_t* L,
                              char* out, unsigned cap);
int  jash_jux_accept_suggestion(jash_line_t* L, const char* suggestion);
int  jash_jux_expand_abbr(jash_state_t* s, jash_line_t* L);
const char* jash_jux_fuzzy_history(jash_state_t* s, const char* query,
                                      int previous_index);

#endif /* JASH_READLINE_JUX_H */
