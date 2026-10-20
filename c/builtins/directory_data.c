// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Directory state has one semantic implementation for text, JSON and JTV2.
#define _GNU_SOURCE
#include "shell_output.h"
#include <core/argv.h>
#include <core/env.h>
#include <core/timeline.h>
#include <core/frecency.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DIRECTORY_MAX 128
#define FIELD(name, value) {JDV_LITERAL(name), (value)}

static char* logical_path(const char* current, const char* target) {
    size_t a = strlen(current), b = strlen(target);
    if (a > SIZE_MAX - b - 3) { errno = EOVERFLOW; return NULL; }
    char* full = malloc(a + b + 3);
    if (!full) return NULL;
    if (target[0] == '/') memcpy(full, target, b + 1);
    else { memcpy(full, current, a); full[a] = '/'; memcpy(full + a + 1, target, b + 1); }
    size_t read = 0, used = 1;
    while (full[read]) {
        while (full[read] == '/') read++;
        if (!full[read]) break;
        size_t begin = read;
        while (full[read] && full[read] != '/') read++;
        size_t count = read - begin;
        if (count == 1 && full[begin] == '.') continue;
        if (count == 2 && full[begin] == '.' && full[begin + 1] == '.') {
            while (used > 1 && full[used - 1] != '/') used--;
            if (used > 1) used--;
            continue;
        }
        if (used > 1) full[used++] = '/';
        memmove(full + used, full + begin, count); used += count;
    }
    full[0] = '/'; full[used] = 0;
    return full;
}

/* Commit PWD/OLDPWD together; restore the original descriptor if the new cwd
 * cannot be represented by central state or the environment update fails. */
static int change_directory(jash_state_t* state, const char* target, int physical,
                             const char* encoded_stack) {
    char* logical = logical_path(state->cwd, target);
    if (!logical) return -1;
    if (!physical && strlen(logical) >= sizeof(state->cwd)) { free(logical); errno = ENAMETOOLONG; return -1; }
    int oldfd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (oldfd < 0) { free(logical); return -1; }
    if (chdir(physical ? target : logical)) { int saved = errno; close(oldfd); free(logical); errno = saved; return -1; }
    char* actual = getcwd(NULL, 0);
    const char* next = physical ? actual : logical;
    if (!actual || !next || strlen(next) >= sizeof(state->cwd)) {
        int saved = actual ? ENAMETOOLONG : errno;
        if (fchdir(oldfd)) saved = EIO;
        close(oldfd); free(actual); free(logical); errno = saved; return -1;
    }
    const char* keys[] = {"OLDPWD", "PWD", "_DIRSTACK"};
    const char* values[] = {state->cwd, next, encoded_stack};
    if (jash_env_apply_batch(state, keys, values, encoded_stack ? 3 : 2)) {
        int saved = ENOMEM;
        if (fchdir(oldfd)) saved = EIO;
        close(oldfd); free(actual); free(logical); errno = saved; return -1;
    }
    char description[JASH_CWD_MAX + 4];
    snprintf(description, sizeof description, "cd %s", next);
    jash_timeline_record(JASH_UNDO_CHDIR, description, state->cwd, NULL);
    memcpy(state->cwd, next, strlen(next) + 1);
    frecency_record(&state->frecency, state->cwd);
    frecency_save(&state->frecency);
    close(oldfd); free(actual); free(logical);
    return 0;
}

static int directory_error(jash_writer_t writer, void* context, const char* command,
                            int mode, const char* code, int status) {
    if (mode) return jash_discovery_error(writer, context, command, mode, code, status);
    jash_writef(jash_stderr_writer, NULL, "%s: %s\n", command, code);
    return status;
}

int jash_b_cd_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return directory_error(writer, context, "cd", mode, "conflicting_formats", 2);
    int physical = 0, ended = 0, dash = 0;
    const char* target = NULL;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!ended && !strcmp(arg, "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(arg)) continue;
        if (!ended && !strcmp(arg, "-L")) { physical = 0; continue; }
        if (!ended && !strcmp(arg, "-P")) { physical = 1; continue; }
        if (target || (!ended && arg[0] == '-' && arg[1])) return directory_error(writer, context, "cd", mode, "invalid_options", 2);
        target = arg; dash = !ended && !strcmp(arg, "-");
    }
    if (!target) target = jash_env_get(state, "HOME");
    else if (dash) target = jash_env_get(state, "OLDPWD");
    if (!target || !*target) return directory_error(writer, context, "cd", mode, dash ? "oldpwd_unset" : "home_unset", 1);
    /* Keep the interactive preview/frecency suggestion path available. The
     * structured and script paths always honor one literal argv operand. */
    if (!mode && state->interactive && !physical && !dash && argc <= 1)
        return jash_b_cd(state, writer, context, target);
    char* requested = strdup(target);
    char* before = strdup(state->cwd);
    if (!requested || !before) { free(requested); free(before); return directory_error(writer, context, "cd", mode, "allocation_failed", 125); }
    jash_discovery_rows_t rows;
    if (mode) {
        if (jash_discovery_rows_init(&rows)) { free(requested); free(before); return directory_error(writer, context, "cd", mode, "allocation_failed", 125); }
        int admission = jash_discovery_effects_begin(&rows, writer, context, "cd", mode, "jash.cwd.v1");
        if (admission) { jash_discovery_rows_free(&rows); free(requested); free(before); return admission; }
    }
    int status = change_directory(state, requested, physical, NULL) ? 1 : 0;
    int error = status ? errno : 0;
    if (!mode) {
        if (status) jash_writef(jash_stderr_writer, NULL, "cd: %s: %s\n", requested, strerror(error));
        else if (dash && jash_writef(writer, context, "%s\n", state->cwd) < 0) status = 125;
        free(requested); free(before); return status;
    }
    char* actual = getcwd(NULL, 0);
    jdv_record_input_t fields[] = {
        FIELD("requested", jash_discovery_path(&rows, requested)),
        FIELD("before", jash_discovery_path(&rows, before)),
        FIELD("cwd", jash_discovery_path(&rows, state->cwd)),
        FIELD("physical_cwd", jash_discovery_path(&rows, actual)),
        FIELD("logical", jdv_value_bool(!physical)),
        FIELD("changed", jdv_value_bool(!status && strcmp(before, state->cwd))),
        FIELD("errno", error ? jdv_value_i64(error) : jdv_value_nothing()),
        FIELD("error", jash_discovery_string(&rows, error ? strerror(error) : NULL))
    };
    jash_discovery_add(&rows, fields, 8);
    int result = jash_discovery_emit_schema(writer, context, "cd", mode, &rows, status, "jash.cwd.v1");
    jash_discovery_rows_free(&rows); free(actual); free(requested); free(before);
    return result;
}

typedef struct { char* paths[DIRECTORY_MAX]; size_t count; } stack_t;
static void stack_free(stack_t* stack) {
    for (size_t i = 0; i < stack->count; i++) free(stack->paths[i]);
    memset(stack, 0, sizeof(*stack));
}
static int stack_load(jash_state_t* state, stack_t* stack) {
    memset(stack, 0, sizeof(*stack));
    jash_argv_t parsed;
    if (jash_argv_build(jash_env_get(state, "_DIRSTACK"), &parsed) < 0) return -1;
    if (parsed.argc > DIRECTORY_MAX) { jash_argv_free(&parsed); return -1; }
    size_t count = parsed.argc ? (size_t)parsed.argc : 1u;
    for (size_t i = 0; i < count; i++) {
        stack->paths[i] = strdup(i ? parsed.argv[i] : state->cwd);
        if (!stack->paths[i]) { jash_argv_free(&parsed); stack_free(stack); return -1; }
        stack->count++;
    }
    jash_argv_free(&parsed); return 0;
}
static int index_value(const char* text, size_t count, size_t* result) {
    if (!text || (text[0] != '+' && text[0] != '-') || !text[1]) return -1;
    uint64_t value = 0;
    for (const unsigned char* p = (const unsigned char*)text + 1; *p; p++) {
        if (*p < '0' || *p > '9' || value > (UINT64_MAX - (*p - '0')) / 10) return -1;
        value = value * 10 + (*p - '0');
    }
    if (value >= count) return -1;
    *result = text[0] == '-' ? count - (size_t)value - 1 : (size_t)value;
    return 0;
}

static int stack_output(jash_state_t* state, jash_writer_t writer, void* context,
                         const char* command, int mode, const stack_t* stack,
                         int verbose, int per_line, int full, size_t first,
                         size_t count, int changed, const char* before,
                         jash_discovery_rows_t* admitted, const char* error) {
    if (mode) {
        jash_discovery_rows_t local;
        jash_discovery_rows_t* rows = admitted ? admitted : &local;
        if (!admitted && jash_discovery_rows_init(rows)) return directory_error(writer, context, command, mode, "allocation_failed", 125);
        for (size_t i = first; i < first + count; i++) {
            jdv_record_input_t fields[] = {
                FIELD("index", jdv_value_u64(i)), FIELD("path", jash_discovery_path(rows, stack->paths[i])),
                FIELD("current", jdv_value_bool(i == 0)), FIELD("stack_changed", jdv_value_bool(changed)),
                FIELD("cwd_changed", jdv_value_bool(strcmp(before, state->cwd) != 0)),
                FIELD("previous_cwd", jash_discovery_path(rows, before)),
                FIELD("error", jash_discovery_string(rows, error))
            };
            jash_discovery_add(rows, fields, 7);
        }
        int rc = jash_discovery_emit_schema(writer, context, command, mode, rows, error ? 1 : 0, "jash.directory-stack.v1");
        if (!admitted) jash_discovery_rows_free(rows);
        return rc;
    }
    const char* home = jash_env_get(state, "HOME");
    for (size_t i = first; i < first + count; i++) {
        if (i > first && jash_writes(writer, context, per_line || verbose ? "\n" : " ") < 0) return 125;
        if (verbose && jash_writef(writer, context, "%zu  ", i) < 0) return 125;
        const char* path = stack->paths[i];
        if (!full && home && *home && !strncmp(path, home, strlen(home)) && (!path[strlen(home)] || path[strlen(home)] == '/')) {
            if (jash_writes(writer, context, "~") < 0) return 125;
            path += strlen(home);
        }
        if (jash_writes(writer, context, path) < 0) return 125;
    }
    return jash_writes(writer, context, "\n") < 0 ? 125 : 0;
}

static int stack_command(jash_state_t* state, jash_writer_t writer, void* context,
                          const char* command, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return directory_error(writer, context, command, mode, "conflicting_formats", 2);
    int dirs = !strcmp(command, "dirs"), push = !strcmp(command, "pushd");
    int ended = 0, no_cd = 0, clear = 0, verbose = 0, full = 0, per_line = 0;
    const char* operand = NULL;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!ended && !strcmp(arg, "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(arg)) continue;
        if (!ended && !dirs && !strcmp(arg, "-n")) { no_cd = 1; continue; }
        if (!ended && dirs && arg[0] == '-' && arg[1] && (arg[1] < '0' || arg[1] > '9')) {
            for (const char* p = arg + 1; *p; p++) {
                if (*p == 'c') clear = 1; else if (*p == 'v') verbose = 1;
                else if (*p == 'l') full = 1; else if (*p == 'p') per_line = 1;
                else return directory_error(writer, context, command, mode, "invalid_options", 2);
            }
            continue;
        }
        if (operand) return directory_error(writer, context, command, mode, "too_many_operands", 2);
        operand = arg;
    }
    if (clear && operand) return directory_error(writer, context, command, mode, "invalid_options", 2);
    stack_t stack;
    if (stack_load(state, &stack)) return directory_error(writer, context, command, mode, "invalid_or_oversized_stack", 1);
    char* before = strdup(state->cwd);
    if (!before) { stack_free(&stack); return directory_error(writer, context, command, mode, "allocation_failed", 125); }
    int changed = !dirs || clear, status = 0;
    size_t chosen = 0, first = 0, count = stack.count;
    const char* error = NULL;
    if (dirs) {
        if (clear) { for (size_t i = 1; i < stack.count; i++) free(stack.paths[i]); stack.count = 1; count = 1; }
        else if (operand) {
            if (index_value(operand, stack.count, &first)) error = "invalid_stack_index";
            count = 1;
        }
    } else if (push) {
        if (no_cd && !operand) changed = 0;
        else if (operand && (ended || (operand[0] != '+' && operand[0] != '-'))) {
            if (stack.count == DIRECTORY_MAX) error = "directory_stack_limit";
            else {
                char* path = logical_path(state->cwd, operand);
                if (!path) error = "allocation_failed";
                else {
                    size_t at = no_cd ? 1 : 0;
                    memmove(stack.paths + at + 1, stack.paths + at, (stack.count - at) * sizeof(*stack.paths));
                    stack.paths[at] = path; stack.count++;
                }
            }
        } else {
            if (operand) { if (index_value(operand, stack.count, &chosen)) error = "invalid_stack_index"; }
            else if (stack.count < 2) error = "directory_stack_empty";
            else chosen = 1;
            if (!error) {
                if (!operand) { char* swap = stack.paths[0]; stack.paths[0] = stack.paths[1]; stack.paths[1] = swap; }
                else { char* rotated[DIRECTORY_MAX]; for (size_t i = 0; i < stack.count; i++) rotated[i] = stack.paths[(i + chosen) % stack.count]; memcpy(stack.paths, rotated, stack.count * sizeof(*rotated)); }
            }
        }
    } else {
        if (operand && index_value(operand, stack.count, &chosen)) error = "invalid_stack_index";
        if (stack.count <= 1) error = "directory_stack_empty";
        if (!error) {
            if (no_cd && chosen == 0) chosen = 1;
            free(stack.paths[chosen]);
            memmove(stack.paths + chosen, stack.paths + chosen + 1, (stack.count - chosen - 1) * sizeof(*stack.paths));
            stack.count--;
        }
    }
    if (!error && no_cd && strcmp(stack.paths[0], state->cwd)) {
        char* current = strdup(state->cwd);
        if (!current) error = "allocation_failed";
        else { free(stack.paths[0]); stack.paths[0] = current; }
    }
    char* encoded = !error && changed ? jash_argv_join((int)stack.count, stack.paths) : NULL;
    if (!error && changed && !encoded) error = "allocation_failed";
    jash_discovery_rows_t rows;
    int admitted = 0;
    if (!error && mode && changed) {
        if (jash_discovery_rows_init(&rows)) error = "allocation_failed";
        else {
            int admission = jash_discovery_effects_begin(&rows, writer, context, command, mode, "jash.directory-stack.v1");
            if (admission) {
                jash_discovery_rows_free(&rows); free(encoded); free(before); stack_free(&stack); return admission;
            }
            admitted = 1;
        }
    }
    /* Retain old stack data until both cwd and environment writes succeed. */
    int changing_cwd = !dirs && !no_cd && (push || chosen == 0);
    if (!error && changing_cwd && change_directory(state, stack.paths[0], 0, encoded)) error = strerror(errno);
    if (!error && changed && !changing_cwd && jash_env_set(state, "_DIRSTACK", encoded)) error = "stack_update_failed";
    if (error && admitted) {
        stack_free(&stack);
        if (stack_load(state, &stack)) rows.error = 1;
        status = stack_output(state, writer, context, command, mode, &stack, verbose, per_line, full, 0, stack.count, 0, before, &rows, error);
    }
    else if (error) status = directory_error(writer, context, command, mode, error, 1);
    else if (!((dirs && clear && !mode) || (push && no_cd && !operand && !mode))) {
        if (!dirs) { first = 0; count = stack.count; }
        status = stack_output(state, writer, context, command, mode, &stack, verbose, per_line, full, first, count, changed, before, admitted ? &rows : NULL, NULL);
    }
    if (admitted) jash_discovery_rows_free(&rows);
    free(encoded); free(before); stack_free(&stack);
    return status;
}

int jash_b_pushd_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) { return stack_command(s, w, ctx, "pushd", argc, argv); }
int jash_b_popd_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) { return stack_command(s, w, ctx, "popd", argc, argv); }
int jash_b_dirs_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) { return stack_command(s, w, ctx, "dirs", argc, argv); }
