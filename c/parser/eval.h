// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/eval — minimal command dispatch.
//
// Phase 2 grammar (legacy parity):
//   - Whitespace-split first token = command name
//   - Rest of line = args (passed as one string to the builtin)
//   - Alias expansion: if the first token is an alias, replace it
//   - Comments: '#' to end-of-line (only at line start or after ws)
//
// Phase 3 will replace this with a real tokenizer + AST + tree-walker.

#ifndef JASH_PARSER_EVAL_H
#define JASH_PARSER_EVAL_H

#include <theme/io.h>
#include <core/state.h>
#include "source_location.h"

/* Evaluate one command line. Updates s->last_status.code.
 * Returns the same exit code. Empty / comment-only lines return 0. */
int jash_eval_line(jash_state_t* s, jash_writer_t w, void* ctx, const char* line);
int jash_get_lineno(void);
int jash_eval_line_mapped(jash_state_t*, jash_writer_t, void*, const char*, const unsigned*, unsigned);
int jash_eval_line_from(jash_state_t*, jash_writer_t, void*, const char*, const char*);

/* Execute a resolved function in the current shell, including scope setup and
 * restoration. args must already be expanded, with quote boundaries intact;
 * only the body is evaluated, not the call line. name/fbody remain caller-owned. */
int jash_eval_function(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* name, const char* fbody, const char* args);

int jash_eval_function_at(jash_state_t*, jash_writer_t, void*, const char*, const char*, const char*, unsigned);

/* Quote-aware function argument parsing shared by direct, redirected and
 * pipeline calls. Values are heap-owned on success. */
int jash_parse_function_positionals(const char* args, char** values,
                                    int* out_count);

#endif /* JASH_PARSER_EVAL_H */
