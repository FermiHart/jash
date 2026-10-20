// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/expand — word expansions performed AFTER cmdsub /
// arithmetic / parameter expansion but BEFORE command dispatch:
//
//   tilde  : ~        -> $HOME
//            ~/path   -> $HOME/path
//   brace  : {a,b,c}  -> a b c
//            pre{a,b} -> prea preb
//            {1..5}   -> 1 2 3 4 5
//   glob   : *.c ?    -> matching filenames in the cwd / dir prefix
//            [abc]    -> character class
//
// These are line-level rewrites: jash_expand_word_layer borrows the unchanged
// input or returns a rewritten line in caller-owned expansion buffers.

#ifndef JASH_PARSER_EXPAND_H
#define JASH_PARSER_EXPAND_H

#include <core/state.h>
#include <core/limits.h>

/* ADR-0036 W4: scratch buffers for the expansion pipeline used to be
 * function-local `static char` arrays — correct for a single top-level
 * call (each stage's own recursion, where present, already copies its
 * input to a private buffer before reusing its output buffer — see
 * jash_expand_braces), but shared across EVERY call for the lifetime of
 * the process. That's ~104KB of permanent BSS for buffers that only ever
 * need to live for one expansion, and it's the same class of hazard that
 * made the var_cache/g_result_stack bugs possible: any future caller
 * pattern that holds a pointer across two independent top-level calls
 * silently aliases them. Caller-owned (stack) storage threaded explicitly
 * through the pipeline removes both problems at once. */
typedef struct {
    char tilde_out[4096];
    char brace_inbuf[JASH_LINE_CAP_STACK];  /* JINV-030 */
    char brace_out[JASH_LINE_CAP_STACK];  /* JINV-030 */
    char glob_out[JASH_LINE_CAP_STACK];  /* JINV-030 */
    char ansi_out[4096];
    /* BUG-167: jash_expand_glob() doesn't take jash_state_t* — populated by
     * jash_expand_word_layer from s->shell_opts before calling it, so the
     * glob stage can honor shopt nullglob without a signature change. */
    int nullglob;
    int glob_error;
    char glob_error_msg[160];
} jash_expand_ctx_t;

/* Run tilde -> brace -> glob expansion on `line`. Returns a pointer into
 * `ctx`'s buffers with the rewritten line, or `line` unchanged if no
 * expansion applied. `ctx` need only live as long as the returned pointer
 * is in use — typically a single stack-allocated instance per top-level
 * expansion call. */
const char* jash_expand_word_layer(jash_state_t* s, const char* line, jash_expand_ctx_t* ctx);

/* JINV-041: til no VALOR de uma atribuicao (inicio e apos cada ':').
 * O caminho de atribuicao simples recebe o valor ja separado, sem o
 * `NAME=` que jash_expand_tilde usa para reconhecer o contexto. */
const char* jash_expand_tilde_value(jash_state_t* s, const char* val,
                                    jash_expand_ctx_t* ctx);


/* Individual stages — exposed for testing / reuse. Each returns a pointer
 * into `ctx` (distinct field per stage) or the input unchanged. */
const char* jash_expand_tilde(jash_state_t* s, const char* line, jash_expand_ctx_t* ctx);
const char* jash_expand_braces(const char* line, jash_expand_ctx_t* ctx);
const char* jash_expand_glob(const char* line, jash_expand_ctx_t* ctx);
int jash_glob_error(void);
const char* jash_glob_error_msg(void);
void jash_glob_clear_error(void);

/* ANSI-C $'...' string expansion: converts $'\n', $'\t', $'\x41', etc.
 * Returns a pointer into `ctx` with $'...' sequences replaced, or NULL if none. */
const char* jash_expand_ansi_c(const char* line, jash_expand_ctx_t* ctx);

#endif /* JASH_PARSER_EXPAND_H */
