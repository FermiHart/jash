// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Cross-domain inverses. Receipts describe observed child execution, not a
 * guarantee about a third-party program's durable state. */
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int started, status, signal_number, error;
    pid_t pid;
} revert_result_t;

static revert_result_t revert_run(char* const* argv, int structured) {
    revert_result_t result = {.status = 125};
    int acknowledgment[2];
    if (pipe(acknowledgment)) { result.error = errno; return result; }
    if (fcntl(acknowledgment[1], F_SETFD, FD_CLOEXEC) < 0) {
        result.error = errno; close(acknowledgment[0]); close(acknowledgment[1]); return result;
    }
    result.pid = fork();
    if (result.pid < 0) {
        result.error = errno; close(acknowledgment[0]); close(acknowledgment[1]); return result;
    }
    if (!result.pid) {
        close(acknowledgment[0]);
        if (!structured || dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) execvp(argv[0], argv);
        int error = errno; size_t used = 0;
        while (used < sizeof error) {
            ssize_t n = write(acknowledgment[1], (char*)&error + used, sizeof error - used);
            if (n > 0) used += (size_t)n;
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        _exit(127);
    }
    close(acknowledgment[1]);
    int error = 0; size_t used = 0;
    while (used < sizeof error) {
        ssize_t n = read(acknowledgment[0], (char*)&error + used, sizeof error - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { if (n < 0) result.error = errno; break; }
    }
    /* A close error cannot undo an observed exec acknowledgment. Keep this
     * observation separate from cleanup errors in the parent. */
    result.started = !used && !result.error;
    if (close(acknowledgment[0]) && !result.error) result.error = errno;
    if (used == sizeof error) result.error = error;
    else if (used && !result.error) result.error = EIO;
    int status = 0; pid_t waited;
    do { waited = waitpid(result.pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) { if (!result.error) result.error = errno; return result; }
    if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { result.signal_number = WTERMSIG(status); result.status = 128 + result.signal_number; }
    if (result.error && !result.status) result.status = 125;
    return result;
}

static int revert_receipt(jash_discovery_rows_t* rows, size_t step,
                           char* const* argv, revert_result_t result) {
    jdv_value_t args[8]; size_t count = 0;
    for (; argv[count]; count++) args[count] = jash_discovery_string(rows, argv[count]);
    jdv_value_t arguments = jdv_value_nothing();
    if (jdv_value_list(&rows->arena, args, count, &arguments)) rows->error = 1;
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("step"), jdv_value_u64(step)},
        {JDV_LITERAL("argv"), arguments},
        {JDV_LITERAL("started"), jdv_value_bool(result.started)},
        {JDV_LITERAL("pid"), result.pid > 0 ? jdv_value_i64(result.pid) : jdv_value_nothing()},
        {JDV_LITERAL("exit_code"), jdv_value_i64(result.status)},
        {JDV_LITERAL("signal"), result.signal_number ? jdv_value_i64(result.signal_number) : jdv_value_nothing()},
        {JDV_LITERAL("errno"), result.error ? jdv_value_i64(result.error) : jdv_value_nothing()},
        {JDV_LITERAL("error"), jash_discovery_string(rows, result.error ? strerror(result.error) : NULL)},
        {JDV_LITERAL("observation"), jash_discovery_string(rows, "child_execution")},
        {JDV_LITERAL("domain_state_verified"), jdv_value_bool(0)},
        {JDV_LITERAL("effects_may_have_applied"), jdv_value_bool(result.pid > 0)},
        {JDV_LITERAL("child_output"), jash_discovery_string(rows, "stderr")}
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

int jash_b_revert_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    (void)state;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "revert", mode, "conflicting_formats", 2);
    char* operands[4]; int count = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (count == 4 || !*argv[i]) goto invalid;
        operands[count++] = argv[i];
    }
    char* first[8] = {0}; char* second[8] = {0};
    if (count >= 2 && !strcmp(operands[0], "git")) {
        first[0] = "git";
        if (count == 2 && !strcmp(operands[1], "commit")) {
            first[1] = "reset"; first[2] = "--soft"; first[3] = "HEAD~1";
        } else if (count == 2 && !strcmp(operands[1], "add")) first[1] = "reset";
        else if (count == 3 && !strcmp(operands[1], "rm")) {
            first[1] = "checkout"; first[2] = "HEAD"; first[3] = "--"; first[4] = operands[2];
            second[0] = "git"; second[1] = "reset"; second[2] = "HEAD"; second[3] = "--"; second[4] = operands[2];
        } else if (count == 4 && !strcmp(operands[1], "mv")) {
            first[1] = "mv"; first[2] = "--"; first[3] = operands[3]; first[4] = operands[2];
        } else goto invalid;
    } else if (count == 2 && !strcmp(operands[0], "docker")) {
        first[0] = "docker"; first[1] = "stop"; first[2] = "--"; first[3] = operands[1];
        second[0] = "docker"; second[1] = "rm"; second[2] = "--"; second[3] = operands[1];
    } else goto invalid;
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return 125;
    int status = mode ? jash_discovery_effects_begin(&rows, writer, context, "revert", mode, "jash.revert.action.v1") : 0;
    for (size_t step = 0; step < 2 && !status; step++) {
        char** command = step ? second : first;
        if (!command[0]) break;
        if (!mode) jash_writef(writer, context, "[revert] %s %s\n", command[0], command[1]);
        revert_result_t result = revert_run(command, mode != 0);
        int delivered = mode ? revert_receipt(&rows, step + 1, command, result) : 0;
        if (!mode && result.status) jash_writef(writer, context, "revert: %s failed with rc %d%s%s\n", command[0], result.status,
            result.error ? ": " : "", result.error ? strerror(result.error) : "");
        status = delivered ? delivered : result.status;
    }
    if (mode) status = jash_discovery_effects_end(&rows, status);
    jash_discovery_rows_free(&rows); return status;
invalid:
    if (mode) return jash_discovery_error(writer, context, "revert", mode, "invalid_arguments", 2);
    jash_writes(writer, context, "Usage: revert [--json|--data] git commit|add|rm FILE|mv SRC DST; revert [--json|--data] docker ID\n");
    return 2;
}

int jash_b_revert(jash_state_t* state, jash_writer_t writer, void* context, const char* args) {
    jash_argv_t parsed = {0};
    if (jash_argv_build(args ? args : "", &parsed) < 0) return 2;
    int result = jash_b_revert_argv(state, writer, context, parsed.argc, parsed.argv);
    jash_argv_free(&parsed); return result;
}
