// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — completion/smart_suggestions — keystroke-driven learning from history.

#ifndef JASH_COMPLETION_SMART_SUGGESTIONS_H
#define JASH_COMPLETION_SMART_SUGGESTIONS_H

#include <core/state.h>

typedef struct jash_ss_engine jash_ss_engine_t;

/* Create smart suggestions engine */
jash_ss_engine_t* jash_ss_engine_create(jash_state_t* s);

/* Destroy engine */
void jash_ss_engine_destroy(jash_ss_engine_t* e);

/* Record a command execution */
void jash_ss_history_record(jash_ss_engine_t* e, const char* cmd, 
                            int exit_code, uint64_t duration_ns);

/* Get suggestion for current input. Returns NULL if no suggestion. */
const char* jash_ss_get_suggestion(jash_ss_engine_t* e, const char* input);

/* Persist engine state to disk */
void jash_ss_engine_persist(jash_ss_engine_t* e);

/* Load persisted engine state from disk */
void jash_ss_engine_load(jash_ss_engine_t* e);

#endif /* JASH_COMPLETION_SMART_SUGGESTIONS_H */
