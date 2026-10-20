// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_ANALYSIS_OUTPUT_H
#define JASH_BUILTINS_ANALYSIS_OUTPUT_H
#include "builtin.h"
int jash_b_lint_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_explain_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_sense_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_debug_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_impact_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_contract_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_expect_argv(jash_state_t*, jash_writer_t, void*, int, char**);
const char* jash_expect_effect_argv(int, char* const*);
int jash_b_assert_structured_argv(jash_state_t*, jash_writer_t, void*, int, char**);
const char* jash_assert_effect_argv(int, char* const*);
int jash_b_speculate_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_context_argv(jash_state_t*, jash_writer_t, void*, int, char**);
#endif
