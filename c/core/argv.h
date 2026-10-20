// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/argv — quote-aware argv tokenizer for builtin dispatch
//
// ADR-0036 W2 (class-kill for BUG-038/BUG-044): builtins historically
// received the rest-of-line as ONE joined string and re-split it themselves
// (naively, on whitespace), so a quoted multi-word argument like
// `printf '%s\n' 'a b c'` lost its quote boundary and was re-split into
// three words. This tokenizer splits ONCE, correctly, respecting quotes —
// the same algorithm `jash_quote_split` (parser/quotes.c) already used, but
// without that function's hard caps (8 tokens of 256 chars) which made it
// unusable for real argv (a long path, or more than 8 arguments).

#ifndef JASH_CORE_ARGV_H
#define JASH_CORE_ARGV_H

#include <stddef.h>

typedef struct {
    int    argc;
    char** argv;     /* NULL-terminated, argc entries */
    char*  backing;  /* all token storage; do not free argv[i] directly */
} jash_argv_t;

/* Tokenize `line` into `out`. Single quotes are fully literal; double quotes
 * allow \$ \" \\ \n escapes (\n as line-continuation, POSIX); backslash
 * outside quotes escapes the next char — same rules as jash_unquote
 * (parser/quotes.c), applied per-token so word boundaries survive.
 * Returns argc (>=0) on success, -1 on unterminated quote or allocation
 * failure. On any successful result, caller must jash_argv_free(out). */
int jash_argv_build(const char* line, jash_argv_t* out);

/* Free storage owned by `out` (safe on a zeroed or already-freed struct). */
void jash_argv_free(jash_argv_t* out);

/* Invocation-local storage for internal dispatch. These are fast-path sizes,
 * not input limits: either buffer independently falls back to heap storage.
 * Keep jash_argv_t unchanged for existing callers and binary consumers. */
typedef struct {
    char* argv[32];
    char backing[512];
} jash_argv_scratch_t;

/* Same tokenizer and result as build. Scratch must outlive the result and must
 * not be reused while it is live. Release every successful result with
 * jash_argv_free_into using the SAME scratch, never jash_argv_free. NULL scratch
 * is equivalent to the original heap-only API. */
int jash_argv_build_into(const char* line, jash_argv_t* out,
                         jash_argv_scratch_t* scratch);
void jash_argv_free_into(jash_argv_t* out, jash_argv_scratch_t* scratch);

/* Literal argv rendering for a legacy execution boundary. Caller frees it. */
char* jash_argv_join(int argc, char* const* argv);

#endif /* JASH_CORE_ARGV_H */
