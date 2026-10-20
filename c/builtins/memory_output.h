// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_MEMORY_OUTPUT_H
#define JASH_BUILTINS_MEMORY_OUTPUT_H
#include "builtin.h"
int jash_b_kv_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_intent_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_resume_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_goal_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_why_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_blamefile_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_journal_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_recall_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_handoff_argv(jash_state_t*, jash_writer_t, void*, int, char**);
#endif
