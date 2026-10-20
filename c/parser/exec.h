// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/exec — external command execution.
//
// When the dispatcher sees a name that:
//   - is not an alias
//   - is not a built-in
//   - DOES exist somewhere along $PATH
// it ends up here. We split argv on whitespace, build a NULL-terminated
// argv[] (and envp[] from the env table), fork, execve, and wait.
//
// Phase 2 is plain (no pipes yet — that lives in parser/pipeline.c).

#ifndef JASH_PARSER_EXEC_H
#define JASH_PARSER_EXEC_H

#include <theme/io.h>
#include <core/state.h>

/* Resolve `name` against $PATH. Writes the absolute path into out
 * (cap bytes). Returns:
 *   1   found, out filled
 *   0   not found
 *  -1   out too small */
int jash_path_lookup(const jash_state_t* s, const char* name,
                     char* out, unsigned long cap);

/* Restore PATH cache from disk (~/.jash/path_cache.dat). Called at
 * startup from jash_state_init. */
void jash_path_cache_restore(jash_state_t* s);

/* Persist only PATH-cache records dirtied in this process. Called centrally
 * from jash_state_cleanup(), so every normal exit path gets one batched flush. */
void jash_path_cache_flush(jash_state_t* s);

/* Mark a cached resolution unusable after exec/spawn proves the path stale. */
void jash_path_cache_invalidate(jash_state_t* s, const char* name);

/* Run an external command. `cmd` is the bareword (no path), `args`
 * is the rest-of-line (may be empty). Looks up via PATH, forks,
 * execve's, waits. Returns the child exit code (or 127 if not
 * found, or negative on syscall failure). */
int jash_exec_external(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* cmd, const char* args);

/* Remember which file fd 1 pointed at when the shell started. Called once
 * from jash_state_init — it is what lets the output governor tell the agent's
 * window apart from a pipe or a `> file`. */
void jash_exec_session_stdout_record(void);

/* 1 when fd 1 STILL points at the destination recorded at startup (dev+ino),
 * i.e. output is going to the session's own stdout. 0 when someone redirected
 * it (pipe segment, `> file`). Returns 1 if nothing was ever recorded, so an
 * embedder that skips the hook keeps the old behaviour. */
int jash_exec_stdout_is_session(void);

#endif
