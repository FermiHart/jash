// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Structured external execution IR. This model deliberately has no shell text.

#ifndef JASH_CORE_EXECUTION_IR_H
#define JASH_CORE_EXECUTION_IR_H

#include <stddef.h>

#define JASH_EXECUTION_IR_VERSION       1u
#define JASH_EXEC_IR_MAX_STAGES         16u
#define JASH_EXEC_IR_MAX_ARGV           64u
#define JASH_EXEC_IR_MAX_TOTAL_ARGV     256u
#define JASH_EXEC_IR_MAX_REDIRS         3u
#define JASH_EXEC_IR_MAX_ARG_BYTES      16384u
#define JASH_EXEC_IR_MAX_PATH_BYTES     4096u
#define JASH_EXEC_IR_MAX_TOTAL_BYTES    65536u

typedef enum {
    JASH_EXEC_REDIR_READ = 1,
    JASH_EXEC_REDIR_WRITE_TRUNC = 2,
    JASH_EXEC_REDIR_WRITE_APPEND = 3,
} jash_execution_redirection_kind_t;

typedef struct {
    unsigned fd; /* Only the standard descriptors 0, 1, and 2 are accepted. */
    jash_execution_redirection_kind_t kind;
    const char* path;
} jash_execution_redirection_t;

typedef struct {
    size_t argc;
    const char* argv[JASH_EXEC_IR_MAX_ARGV];
    size_t redirection_count;
    jash_execution_redirection_t redirections[JASH_EXEC_IR_MAX_REDIRS];
} jash_execution_stage_t;

typedef struct {
    unsigned version;
    size_t stage_count;
    jash_execution_stage_t stages[JASH_EXEC_IR_MAX_STAGES];
} jash_execution_ir_t;

typedef enum {
    JASH_EXEC_IR_VALID = 0,
    JASH_EXEC_IR_NULL,
    JASH_EXEC_IR_BAD_VERSION,
    JASH_EXEC_IR_BAD_STAGE_COUNT,
    JASH_EXEC_IR_BAD_ARGC,
    JASH_EXEC_IR_NULL_ARG,
    JASH_EXEC_IR_EMPTY_COMMAND,
    JASH_EXEC_IR_ARG_TOO_LONG,
    JASH_EXEC_IR_TOO_MANY_TOTAL_ARGS,
    JASH_EXEC_IR_TOO_MANY_REDIRS,
    JASH_EXEC_IR_BAD_REDIR_FD,
    JASH_EXEC_IR_DUPLICATE_REDIR_FD,
    JASH_EXEC_IR_BAD_REDIR_KIND,
    JASH_EXEC_IR_BAD_REDIR_PATH,
    JASH_EXEC_IR_TOO_MANY_TOTAL_BYTES,
} jash_execution_ir_error_t;

void jash_execution_ir_init(jash_execution_ir_t* ir);

/* Validation reads only the supplied object and strings. It never opens a
 * path, resolves PATH, changes cwd, allocates, forks, or executes anything. */
int jash_execution_ir_validate(const jash_execution_ir_t* ir,
                               jash_execution_ir_error_t* error);

const char* jash_execution_ir_error_string(jash_execution_ir_error_t error);

#endif
