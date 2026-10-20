// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — parser/builtin_redir.h — public interface for builtin redirection helpers.

#ifndef JASH_PARSER_BUILTIN_REDIR_H
#define JASH_PARSER_BUILTIN_REDIR_H

#include <core/state.h>
#include <builtins/builtin.h>
#include <theme/io.h>

void jash_stdin_stream_sync(int drop_buffered);
/* A typed streaming result must see write failures before its next effect.
 * Switch only the known redirect buffer to checked streaming; keep its fd. */
void jash_redir_stream_writer(jash_writer_t writer, void* context);
/* Before other redirect engines save/apply fds, pass ALL user targets and dup
 * sources together. Moves enclosing registered backups out of the way.
 * Returns 0 on success, positive on failure (do not execute the command). */
int jash_redir_reserve_fds(const int* operands, size_t count);
typedef struct jash_redir_snapshot jash_redir_snapshot_t;
/* Save targets and register their backups for the entire nested execution.
 * All targets and operands are excluded from backup allocation; inputs are
 * copied. begin returns 0 or a positive error, leaving *out NULL on failure.
 * end consumes the snapshot in LIFO order: restore=1 rolls back, 0 commits. */
int jash_redir_snapshot_begin(jash_redir_snapshot_t** out,
                              const int* targets, size_t target_count,
                              const int* operands, size_t operand_count);
int jash_redir_snapshot_end(jash_redir_snapshot_t* saved, int restore);
int jash_eval_builtin_with_redir(jash_state_t* s, jash_writer_t w, void* ctx,
                                 const jash_builtin_t* b, const char* args);
/* -1: actual command (normal exec path); 0: applied; positive: error. */
int jash_exec_apply_redirs_permanently(jash_state_t* s, const char* line);
int jash_eval_function_with_redir(jash_state_t* s, jash_writer_t w, void* ctx,
                                   const char* fname, const char* args);

#endif /* JASH_PARSER_BUILTIN_REDIR_H */
