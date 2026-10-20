// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Bounded capture of the command being evaluated, never of a human report.
 * A draining child bounds retained memory while the evaluator stays in the
 * current shell. No temporary output file grows with command output. */
#ifndef JASH_BUILTINS_ANALYSIS_CAPTURE_H
#define JASH_BUILTINS_ANALYSIS_CAPTURE_H
#include "eval_output.h"
#include "semantic_value.h"
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#define ANALYSIS_CAPTURE_MAX (1024u * 1024u)
typedef struct {
    jash_eval_observation_t execution;
    unsigned char* bytes[2];
    size_t size[2];
    uint64_t seen[2];
    int truncated[2], complete, error;
} analysis_capture_t;
typedef struct {
    uint64_t seen[2], kept[2];
    int error, boundary_limit;
} analysis_capture_header_t;

static inline int analysis_capture_pipe(int pair[2]) {
    if (pipe(pair)) return -1;
    for (int i = 0; i < 2; i++) if (fcntl(pair[i], F_SETFD, FD_CLOEXEC)) {
        int error = errno; close(pair[0]); close(pair[1]); pair[0] = pair[1] = -1; errno = error; return -1;
    }
    return 0;
}
static inline int analysis_capture_transfer(int fd, void* data, size_t size, int writing) {
    unsigned char* at = data;
    while (size) {
        ssize_t n = writing ? write(fd, at, size) : read(fd, at, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        at += n; size -= (size_t)n;
    }
    return 0;
}
static inline void analysis_capture_collect(int input[2], int control, int result,
        unsigned char* buffers[2], size_t limit) {
    analysis_capture_header_t header = {0}; int live[2] = {1,1}, done = 0;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(input[i], F_GETFL);
        if (flags < 0 || fcntl(input[i], F_SETFL, flags | O_NONBLOCK)) header.error = errno;
    }
    while (!header.error && !done && (live[0] || live[1])) {
        struct pollfd descriptors[3] = {{live[0] ? input[0] : -1, POLLIN,0},
            {live[1] ? input[1] : -1,POLLIN,0},{control,POLLIN,0}};
        int ready;
        do { ready = poll(descriptors, 3, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) { header.error = errno; break; }
        done = !!(descriptors[2].revents & (POLLIN | POLLHUP | POLLERR));
        for (int i = 0; i < 2 && !header.error; i++) {
            if (!live[i]) continue;
            if (!done && !(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            unsigned iterations = 0;
            for (;;) {
                unsigned char chunk[8192]; ssize_t n = read(input[i], chunk, sizeof(chunk));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n < 0) { header.error = errno; break; }
                if (!n) { live[i] = 0; break; }
                if ((uint64_t)n > UINT64_MAX - header.seen[i]) { header.error = EOVERFLOW; break; }
                header.seen[i] += (uint64_t)n;
                size_t keep = limit - (size_t)header.kept[i];
                if (keep > (size_t)n) keep = (size_t)n;
                memcpy(buffers[i] + header.kept[i], chunk, keep); header.kept[i] += keep;
                /* A background child can keep writing after eval returns.
                 * Bound the final drain and declare that boundary incomplete. */
                if (++iterations == 64u) { if (done) header.boundary_limit = 1; break; }
            }
        }
    }
    for (int i = 0; i < 2; i++) if (close(input[i]) && !header.error) header.error = errno;
    if (close(control) && !header.error) header.error = errno;
    int failed = analysis_capture_transfer(result, &header, sizeof(header), 1);
    for (int i = 0; i < 2 && !failed; i++)
        failed = analysis_capture_transfer(result, buffers[i], (size_t)header.kept[i], 1);
    if (close(result)) failed = 1;
    _exit(failed ? 1 : 0);
}

static inline int analysis_capture_exec(char* const argv[], int output, int diagnostic,
                                        jash_eval_observation_t* observed) {
    *observed = (jash_eval_observation_t){0}; int failure[2] = {-1,-1};
    if (analysis_capture_pipe(failure)) { observed->adapter_error = errno; return -1; }
    pid_t child = fork();
    if (child < 0) {
        observed->adapter_error = errno; close(failure[0]); close(failure[1]); return -1;
    }
    if (!child) {
        close(failure[0]);
        if (dup2(output, STDOUT_FILENO) >= 0 && dup2(diagnostic, STDERR_FILENO) >= 0) {
            if (output > STDERR_FILENO) close(output);
            if (diagnostic > STDERR_FILENO && diagnostic != output) close(diagnostic);
            execvp(argv[0], argv);
        }
        int error = errno;
        (void)analysis_capture_transfer(failure[1], &error, sizeof(error), 1);
        _exit(error == ENOENT ? 127 : 126);
    }
    observed->attempted = 1; close(failure[1]); int exec_error = 0; ssize_t bytes;
    do { bytes = read(failure[0], &exec_error, sizeof(exec_error)); } while (bytes < 0 && errno == EINTR);
    if (bytes < 0) observed->adapter_error = errno;
    else if (bytes && bytes != (ssize_t)sizeof(exec_error)) observed->adapter_error = EIO;
    else if (bytes) observed->adapter_error = exec_error;
    if (close(failure[0]) && !observed->adapter_error) observed->adapter_error = errno;
    int status = 0; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) { if (!observed->adapter_error) observed->adapter_error = errno; }
    else observed->command_status = WIFEXITED(status) ? WEXITSTATUS(status) :
        WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    return observed->adapter_error ? -1 : 0;
}

static inline int analysis_capture_run_mode(jash_state_t* state, const char* command,
        char* const argv[], size_t limit, int merged, analysis_capture_t* capture) {
    memset(capture, 0, sizeof(*capture));
    if (!limit || limit > ANALYSIS_CAPTURE_MAX) { capture->error = EINVAL; return -1; }
    int output[2][2] = {{-1,-1},{-1,-1}}, control[2] = {-1,-1}, result[2] = {-1,-1};
    pid_t collector = -1; int error = 0;
    for (int i = 0; i < 2; i++) {
        capture->bytes[i] = malloc(limit + 1u);
        if (!capture->bytes[i]) { error = ENOMEM; goto cleanup; }
        if (analysis_capture_pipe(output[i])) { error = errno; goto cleanup; }
    }
    if (analysis_capture_pipe(control) || analysis_capture_pipe(result)) { error = errno; goto cleanup; }
    collector = fork();
    if (collector < 0) { error = errno; goto cleanup; }
    if (!collector) {
        (void)signal(SIGPIPE, SIG_IGN);
        close(output[0][1]); close(output[1][1]); close(control[1]); close(result[0]);
        int inputs[] = {output[0][0],output[1][0]};
        analysis_capture_collect(inputs, control[0], result[1], capture->bytes, limit);
    }
    close(output[0][0]); output[0][0] = -1;
    close(output[1][0]); output[1][0] = -1;
    close(control[0]); control[0] = -1; close(result[1]); result[1] = -1;
    int diagnostic = output[merged ? 0 : 1][1];
    int evaluation = argv ? analysis_capture_exec(argv, output[0][1], diagnostic, &capture->execution) :
        jash_eval_with_output(state, command, output[0][1], diagnostic, &capture->execution);
    if (evaluation)
        error = capture->execution.adapter_error ? capture->execution.adapter_error : EIO;
    /* An explicit byte also works when a shell background subshell inherited
     * the write descriptor without exec: EOF alone would wait for that job.
     * Keep our output writers open until this byte is sent. Otherwise the
     * collector may observe both EOFs, close control, and make a completed
     * capture spuriously fail with EPIPE. */
    unsigned char boundary = 1;
    struct sigaction ignore = {0}, previous = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    int signal_saved = !sigaction(SIGPIPE, &ignore, &previous);
    if (!signal_saved) { if (!error) error = errno; }
    else {
        if (analysis_capture_transfer(control[1], &boundary, 1, 1) && !error) error = errno;
        if (sigaction(SIGPIPE, &previous, NULL) && !error) error = errno;
    }
    if (close(control[1]) && !error) error = errno;
    control[1] = -1;
    for (int i = 0; i < 2; i++) {
        if (close(output[i][1]) && !error) error = errno;
        output[i][1] = -1;
    }
    analysis_capture_header_t header = {0};
    if (analysis_capture_transfer(result[0], &header, sizeof(header), 0)) { if (!error) error = errno; }
    else if (header.kept[0] > limit || header.kept[1] > limit ||
             header.kept[0] > header.seen[0] || header.kept[1] > header.seen[1]) { if (!error) error = EIO; }
    else {
        for (int i = 0; i < 2; i++) {
            if (analysis_capture_transfer(result[0], capture->bytes[i], (size_t)header.kept[i], 0)) {
                if (!error) error = errno;
                break;
            }
            capture->size[i] = (size_t)header.kept[i]; capture->bytes[i][capture->size[i]] = 0;
            capture->seen[i] = header.seen[i];
            capture->truncated[i] = header.seen[i] > header.kept[i] || header.boundary_limit;
        }
        if (header.error && !error) error = header.error;
    }
cleanup:
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++)
        if (output[i][j] >= 0 && close(output[i][j]) && !error) error = errno;
    for (int i = 0; i < 2; i++) {
        if (control[i] >= 0 && close(control[i]) && !error) error = errno;
        if (result[i] >= 0 && close(result[i]) && !error) error = errno;
    }
    if (collector > 0) {
        int status = 0; pid_t waited;
        do { waited = waitpid(collector, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) { if (!error) error = EIO; }
    }
    if (!error && (capture->truncated[0] || capture->truncated[1])) error = EOVERFLOW;
    capture->error = error; capture->complete = !error;
    if (error) { errno = error; return -1; }
    return 0;
}

static inline int analysis_capture_run(jash_state_t* state, const char* command,
                                       size_t limit, analysis_capture_t* capture) {
    return analysis_capture_run_mode(state, command, NULL, limit, 0, capture);
}
static inline int analysis_capture_argv(char* const argv[], size_t limit, analysis_capture_t* capture) {
    return analysis_capture_run_mode(NULL, NULL, argv, limit, 0, capture);
}
/* Both descriptors write to the same pipe: preserve observed kernel ordering
 * for predicates whose documented subject is combined command output. */
static inline int analysis_capture_merged(jash_state_t* state, const char* command,
                                          size_t limit, analysis_capture_t* capture) {
    return analysis_capture_run_mode(state, command, NULL, limit, 1, capture);
}

static inline void analysis_capture_free(analysis_capture_t* capture) {
    for (int i = 0; i < 2; i++) { free(capture->bytes[i]); capture->bytes[i] = NULL; }
}

static inline jdv_value_t analysis_capture_value(semantic_value_t* sem, const analysis_capture_t* capture) {
    jdv_record_input_t fields[] = {
        semantic_field("attempted", jdv_value_bool(capture->execution.attempted)),
        semantic_field("command_status", capture->execution.attempted ? jdv_value_i64(capture->execution.command_status) : jdv_value_nothing()),
        semantic_field("stdout", semantic_bytes(sem, capture->bytes[0], capture->size[0])),
        semantic_field("stderr", semantic_bytes(sem, capture->bytes[1], capture->size[1])),
        semantic_field("stdout_bytes_seen", jdv_value_filesize(capture->seen[0])),
        semantic_field("stderr_bytes_seen", jdv_value_filesize(capture->seen[1])),
        semantic_field("stdout_truncated", jdv_value_bool(capture->truncated[0])),
        semantic_field("stderr_truncated", jdv_value_bool(capture->truncated[1])),
        semantic_field("complete", jdv_value_bool(capture->complete)),
        semantic_field("capture_boundary", semantic_symbol(sem, "evaluation_return_background_output_not_waited")),
        semantic_field("adapter_errno", capture->error ? jdv_value_i64(capture->error) : jdv_value_nothing()),
        semantic_field("writer_errno", capture->execution.writer_error ? jdv_value_i64(capture->execution.writer_error) : jdv_value_nothing()),
        semantic_field("restore_errno", capture->execution.restore_error ? jdv_value_i64(capture->execution.restore_error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}
#endif
