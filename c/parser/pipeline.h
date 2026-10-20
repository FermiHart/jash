// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/pipeline — multi-command pipe execution (header).

#ifndef JASH_PARSER_PIPELINE_H
#define JASH_PARSER_PIPELINE_H

#include <theme/io.h>
#include <core/state.h>

/* Execute a pipeline. `line` is the raw command line which may contain
 * zero or more '|' characters. Segments are split on unquoted '|',
 * each resolved via PATH, connected with pipe+dup2, and run in N
 * children. The parent waits for all; returns the exit code of the
 * LAST segment (bash convention).
 *
 * Also handles redirects (> < >>) within each segment.
 * Single-command lines (no pipe) are delegated to jash_exec_external
 * unless redirects are present. */
int jash_exec_pipeline(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* line);

/* Returns 1 if `line` contains an unquoted '|' character. */
int jash_line_has_pipe(const char* line);

/* Returns 1 if `line` contains an unquoted '>' or '<' (redirect). */
int jash_line_has_redir(const char* line);

/* Background fast path for `cmd &`: if `line` is a single external
 * command (no pipe, not a builtin/function/compound/alias, nothing
 * absorbable, PATH-resolvable), apply its assigns + redirects in place
 * and execve — this call does NOT return on success. Returns (does
 * nothing) for anything else so the caller falls back to the full
 * subshell eval. MUST be called in an already-forked child (it replaces
 * the process image when it fires). */
void jash_pipeline_bg_exec(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* line);

#endif
