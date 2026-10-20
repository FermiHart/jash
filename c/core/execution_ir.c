// SPDX-License-Identifier: LicenseRef-SPL-1.0

#include "execution_ir.h"

#include <string.h>

static int ir_fail(jash_execution_ir_error_t value,
                   jash_execution_ir_error_t* error) {
    if (error) *error = value;
    return -1;
}

void jash_execution_ir_init(jash_execution_ir_t* ir) {
    if (!ir) return;
    memset(ir, 0, sizeof(*ir));
    ir->version = JASH_EXECUTION_IR_VERSION;
}

int jash_execution_ir_validate(const jash_execution_ir_t* ir,
                               jash_execution_ir_error_t* error) {
    if (error) *error = JASH_EXEC_IR_VALID;
    if (!ir) return ir_fail(JASH_EXEC_IR_NULL, error);
    if (ir->version != JASH_EXECUTION_IR_VERSION)
        return ir_fail(JASH_EXEC_IR_BAD_VERSION, error);
    if (ir->stage_count == 0 || ir->stage_count > JASH_EXEC_IR_MAX_STAGES)
        return ir_fail(JASH_EXEC_IR_BAD_STAGE_COUNT, error);

    size_t total_argv = 0;
    size_t total_bytes = 0;
    for (size_t i = 0; i < ir->stage_count; i++) {
        const jash_execution_stage_t* stage = &ir->stages[i];
        if (stage->argc == 0 || stage->argc > JASH_EXEC_IR_MAX_ARGV)
            return ir_fail(JASH_EXEC_IR_BAD_ARGC, error);
        if (total_argv > JASH_EXEC_IR_MAX_TOTAL_ARGV - stage->argc)
            return ir_fail(JASH_EXEC_IR_TOO_MANY_TOTAL_ARGS, error);
        total_argv += stage->argc;

        for (size_t a = 0; a < stage->argc; a++) {
            if (!stage->argv[a]) return ir_fail(JASH_EXEC_IR_NULL_ARG, error);
            size_t length = strnlen(stage->argv[a], JASH_EXEC_IR_MAX_ARG_BYTES + 1u);
            if (length > JASH_EXEC_IR_MAX_ARG_BYTES)
                return ir_fail(JASH_EXEC_IR_ARG_TOO_LONG, error);
            if (a == 0 && length == 0)
                return ir_fail(JASH_EXEC_IR_EMPTY_COMMAND, error);
            if (total_bytes > JASH_EXEC_IR_MAX_TOTAL_BYTES - length - 1u)
                return ir_fail(JASH_EXEC_IR_TOO_MANY_TOTAL_BYTES, error);
            total_bytes += length + 1u;
        }

        if (stage->redirection_count > JASH_EXEC_IR_MAX_REDIRS)
            return ir_fail(JASH_EXEC_IR_TOO_MANY_REDIRS, error);
        unsigned used_fds = 0;
        for (size_t r = 0; r < stage->redirection_count; r++) {
            const jash_execution_redirection_t* redir = &stage->redirections[r];
            if (redir->fd > 2u)
                return ir_fail(JASH_EXEC_IR_BAD_REDIR_FD, error);
            unsigned bit = 1u << redir->fd;
            if (used_fds & bit)
                return ir_fail(JASH_EXEC_IR_DUPLICATE_REDIR_FD, error);
            used_fds |= bit;
            if (redir->kind != JASH_EXEC_REDIR_READ &&
                redir->kind != JASH_EXEC_REDIR_WRITE_TRUNC &&
                redir->kind != JASH_EXEC_REDIR_WRITE_APPEND)
                return ir_fail(JASH_EXEC_IR_BAD_REDIR_KIND, error);
            if (!redir->path || !redir->path[0])
                return ir_fail(JASH_EXEC_IR_BAD_REDIR_PATH, error);
            size_t length = strnlen(redir->path, JASH_EXEC_IR_MAX_PATH_BYTES + 1u);
            if (length > JASH_EXEC_IR_MAX_PATH_BYTES)
                return ir_fail(JASH_EXEC_IR_BAD_REDIR_PATH, error);
            if (total_bytes > JASH_EXEC_IR_MAX_TOTAL_BYTES - length - 1u)
                return ir_fail(JASH_EXEC_IR_TOO_MANY_TOTAL_BYTES, error);
            total_bytes += length + 1u;
        }
    }
    return 0;
}

const char* jash_execution_ir_error_string(jash_execution_ir_error_t error) {
    switch (error) {
    case JASH_EXEC_IR_VALID: return "valid";
    case JASH_EXEC_IR_NULL: return "IR is null";
    case JASH_EXEC_IR_BAD_VERSION: return "unsupported IR version";
    case JASH_EXEC_IR_BAD_STAGE_COUNT: return "invalid pipeline stage count";
    case JASH_EXEC_IR_BAD_ARGC: return "invalid argv count";
    case JASH_EXEC_IR_NULL_ARG: return "argv contains a null item";
    case JASH_EXEC_IR_EMPTY_COMMAND: return "command name is empty";
    case JASH_EXEC_IR_ARG_TOO_LONG: return "argv item exceeds its byte limit";
    case JASH_EXEC_IR_TOO_MANY_TOTAL_ARGS: return "pipeline has too many argv items";
    case JASH_EXEC_IR_TOO_MANY_REDIRS: return "stage has too many redirections";
    case JASH_EXEC_IR_BAD_REDIR_FD: return "redirection fd is outside 0..2";
    case JASH_EXEC_IR_DUPLICATE_REDIR_FD: return "stage redirects an fd twice";
    case JASH_EXEC_IR_BAD_REDIR_KIND: return "invalid redirection kind";
    case JASH_EXEC_IR_BAD_REDIR_PATH: return "invalid redirection path";
    case JASH_EXEC_IR_TOO_MANY_TOTAL_BYTES: return "IR exceeds its total byte limit";
    }
    return "unknown IR validation error";
}
