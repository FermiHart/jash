// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Execute the existing evaluator in the current shell with an explicit output
 * destination. The caller owns capture/drain limits and reports output as
 * bytes; this helper does not reinterpret it as semantic data. */
#ifndef JASH_BUILTINS_EVAL_OUTPUT_H
#define JASH_BUILTINS_EVAL_OUTPUT_H
#include <parser/eval.h>
#include <parser/builtin_redir.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    int attempted;
    int command_status;
    int adapter_error;
    int writer_error;
    int restore_error;
} jash_eval_observation_t;

static inline long eval_output_writer(void* context, const char* bytes, size_t size) {
    jash_eval_observation_t* observed = context;
    if (observed->writer_error) { errno = observed->writer_error; return -1; }
    if (size > LONG_MAX) { observed->writer_error = EOVERFLOW; errno = EOVERFLOW; return -1; }
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(STDOUT_FILENO, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { observed->writer_error = count ? errno : EIO; errno = observed->writer_error; return -1; }
        offset += (size_t)count;
    }
    return (long)size;
}

/* stderr_fd == -1 keeps stderr. Descriptor destinations are borrowed; they
 * are pinned before dup2, so crossed stdout/stderr destinations remain exact.
 * Shell-controlled replacement/termination may not return: callers must not
 * fabricate completion for such an incomplete native stream. */
static inline int jash_eval_with_output(jash_state_t* state, const char* command,
        int stdout_fd, int stderr_fd, jash_eval_observation_t* observed) {
    if (!observed) { errno = EINVAL; return -1; }
    *observed = (jash_eval_observation_t){0};
    if (!state || !command || stdout_fd < 0 || stderr_fd < -1) {
        observed->adapter_error = EINVAL; errno = EINVAL; return -1;
    }
    if (fflush(stdout) || (stderr_fd >= 0 && fflush(stderr))) {
        observed->adapter_error = errno ? errno : EIO; return -1;
    }
    int output = fcntl(stdout_fd, F_DUPFD_CLOEXEC, 3);
    int diagnostic = stderr_fd >= 0 ? fcntl(stderr_fd, F_DUPFD_CLOEXEC, 3) : -1;
    if (output < 0 || (stderr_fd >= 0 && diagnostic < 0)) {
        observed->adapter_error = errno;
        if (output >= 0) close(output);
        if (diagnostic >= 0) close(diagnostic);
        errno = observed->adapter_error; return -1;
    }
    int targets[2] = {STDOUT_FILENO, STDERR_FILENO};
    int operands[2] = {output, diagnostic};
    jash_redir_snapshot_t* saved = NULL;
    size_t count = stderr_fd >= 0 ? 2u : 1u;
    if (jash_redir_snapshot_begin(&saved, targets, count, operands, count))
        observed->adapter_error = errno ? errno : EIO;
    if (!observed->adapter_error && (dup2(output, STDOUT_FILENO) < 0 ||
        (stderr_fd >= 0 && dup2(diagnostic, STDERR_FILENO) < 0))) observed->adapter_error = errno;
    if (close(output) && !observed->adapter_error) observed->adapter_error = errno;
    if (diagnostic >= 0 && close(diagnostic) && !observed->adapter_error) observed->adapter_error = errno;
    if (!observed->adapter_error) {
        observed->attempted = 1;
        observed->command_status = jash_eval_line(state, eval_output_writer, observed, command);
        if (fflush(stdout) || (stderr_fd >= 0 && fflush(stderr))) observed->adapter_error = errno ? errno : EIO;
        if (observed->writer_error && !observed->adapter_error) observed->adapter_error = observed->writer_error;
    }
    if (saved && jash_redir_snapshot_end(saved, 1)) {
        observed->restore_error = errno ? errno : EIO;
        if (!observed->adapter_error) observed->adapter_error = observed->restore_error;
    }
    clearerr(stdout);
    if (stderr_fd >= 0) clearerr(stderr);
    if (observed->adapter_error) { errno = observed->adapter_error; return -1; }
    return 0;
}
#endif
