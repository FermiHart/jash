// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Force the optional task/children source absent even on a kernel that has
 * it. The real supervisor, proc reader and pidfd signalling run unchanged. */
#define _GNU_SOURCE
#define JASH_AGENT_PROCESS_TEST_NO_CHILDREN 1
#include "../c/core/agent_process.c"

static const char* test_proc_root;
static const char* test_denied_stat;

int __real_open(const char* path, int flags, ...);
int __real_openat(int fd, const char* path, int flags, ...);
int __wrap_open(const char* path, int flags, ...);
int __wrap_openat(int fd, const char* path, int flags, ...);

int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    if (test_proc_root && !strcmp(path, "/proc")) path = test_proc_root;
    return __real_open(path, flags, mode);
}
int __wrap_openat(int fd, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    if (test_proc_root && test_denied_stat && !strcmp(path, test_denied_stat)) {
        errno = EACCES; return -1;
    }
    return __real_openat(fd, path, flags, mode);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __func__, __LINE__, #condition, errno); \
    return -1; \
} } while (0)

static int put_stat(int root, const char* directory, const void* text, size_t size) {
    if (mkdirat(root, directory, 0700) && errno != EEXIST) return -1;
    char path[128];
    int length = snprintf(path, sizeof(path), "%s/stat", directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return -1;
    int fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int status = write_all(fd, text, size);
    if (close(fd)) status = -1;
    return status;
}

static int fake_source_contract(const char* root) {
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 0);
    char self[256], child[256], other[256];
    int self_size = snprintf(self, sizeof(self), "%ld (self) R %ld 1 1 0 0\n",
                             (long)getpid(), (long)getppid());
    int child_size = snprintf(child, sizeof(child), "101 (a ) tricky\ncomm) S %ld 1 1 0 0\n", (long)getpid());
    int other_size = snprintf(other, sizeof(other), "102 (other) S %ld 1 1 0 0\n", (long)getppid());
    CHECK(self_size > 0 && child_size > 0 && other_size > 0);
    CHECK(!put_stat(directory, "self", self, (size_t)self_size));
    CHECK(!put_stat(directory, "101", child, (size_t)child_size));
    CHECK(!put_stat(directory, "102", other, (size_t)other_size));
    test_proc_root = root;
    pid_t children[8] = {0}; size_t count = 99;
    CHECK(!read_direct_children(children, 8, &count));
    CHECK(count == 1 && children[0] == 101);
    errno = 0;
    CHECK(read_direct_children(children, 0, &count) == -1 && errno == EOVERFLOW && count == 0);
    test_denied_stat = "101/stat";
    CHECK(read_direct_children(children, 8, &count) == -1 && errno == EACCES && count == 0);
    test_denied_stat = NULL;

    const char* malformed[] = {
        "999 (wrong identity) S 1 1 1 0 0\n",
        "101 (unterminated) S 1 1 1 0 0",
        "101 (bad state) 0 1 1 1 0 0\n",
        "101 (bad parent) S x 1 1 0 0\n",
        "101 (overflow parent) S 999999999999999999999 1 1 0 0\n",
        "101 (no state)\n"
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        CHECK(!put_stat(directory, "101", malformed[i], strlen(malformed[i])));
        CHECK(read_direct_children(children, 8, &count) == -1 && count == 0);
    }
    char embedded[] = "101 (nul) S 1 1\0 1 0 0\n";
    CHECK(!put_stat(directory, "101", embedded, sizeof(embedded) - 1u));
    CHECK(read_direct_children(children, 8, &count) == -1 && count == 0);
    CHECK(!put_stat(directory, "101", child, (size_t)child_size));
    const char wrong_self[] = "1 (foreign namespace) S 0 1 1 0 0\n";
    CHECK(!put_stat(directory, "self", wrong_self, sizeof(wrong_self) - 1u));
    CHECK(read_direct_children(children, 8, &count) == -1 && errno == EIO && count == 0);

    /* A permanent source failure reached from an indeterminate cleanup state
     * must be returned, with its final facts, instead of spinning in destroy. */
    jash_agent_process_t* process = calloc(1, sizeof(*process));
    CHECK(process != NULL);
    process->pty_master_fd = process->stdin_writer_fd = process->stdout_fd = -1;
    process->status_fd = process->completion_fd = -1;
    process->launcher_reaped = process->pgid_quiescent = 1;
    process->result.state = JASH_AGENT_PROCESS_INDETERMINATE;
    process->result.indeterminate = 1;
    jash_agent_process_result_t final;
    int64_t start = monotonic_ms(); errno = 0;
    CHECK(jash_agent_process_destroy_checked(process, &final) == -1 && errno == EIO);
    CHECK(monotonic_ms() - start < 1000);
    CHECK(final.indeterminate && !final.quiescent && !final.descendants_quiescent);
    CHECK(final.cleanup_error_number == EIO && final.state == JASH_AGENT_PROCESS_INDETERMINATE);
    test_proc_root = NULL;
    CHECK(close(directory) == 0);
    return 0;
}

static int actual_children_and_identity(void) {
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { for (;;) pause(); }
    pid_t children[4096]; size_t count = 0;
    int observed = read_direct_children(children, 4096, &count);
    int found = 0;
    if (!observed) for (size_t i = 0; i < count; i++) if (children[i] == child) found = 1;
    int forced = 0, killed = kill_pid_safely(child, &forced), saved = errno;
    if (killed) (void)kill(child, SIGKILL);
    int status = 0; CHECK(waitpid(child, &status, 0) == child);
    errno = saved;
    CHECK(!observed && found && !killed && forced);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    int control[2], report[2]; CHECK(!make_pipe(control) && !make_pipe(report));
    child = fork(); CHECK(child >= 0);
    if (!child) {
        close(control[1]); close(report[0]);
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(70);
        if (!grandchild) { close(control[0]); close(report[1]); for (;;) pause(); }
        if (write_all(report[1], &grandchild, sizeof(grandchild))) _exit(71);
        char done; (void)read_exact(control[0], &done, 1);
        (void)kill(grandchild, SIGKILL);
        while (waitpid(grandchild, NULL, 0) < 0 && errno == EINTR) {}
        _exit(0);
    }
    close(control[0]); close(report[1]);
    pid_t foreign = 0; CHECK(!read_exact(report[0], &foreign, sizeof(foreign)) && foreign > 0);
    forced = 0; int foreign_result = kill_pid_safely(foreign, &forced);
    int still_alive = kill(foreign, 0) == 0;
    CHECK(!write_all(control[1], "G", 1));
    close(control[1]); close(report[0]);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(!foreign_result && !forced && still_alive && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}

static int successful_leader_is_contained(const char* root) {
    char out[4096], err[4096], error[256];
    CHECK(snprintf(out, sizeof(out), "%s/out", root) > 0);
    CHECK(snprintf(err, sizeof(err), "%s/err", root) > 0);
    int cwd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); CHECK(cwd >= 0);
    jash_execution_ir_t ir; jash_execution_ir_init(&ir);
    ir.stage_count = 1; ir.stages[0].argc = 3;
    ir.stages[0].argv[0] = "/bin/sh"; ir.stages[0].argv[1] = "-c";
    ir.stages[0].argv[2] = "(sleep 1; touch escaped) & exit 0";
    jash_agent_process_options_t options; jash_agent_process_options_init(&options);
    options.cwd_fd = cwd; options.stdout_path = out; options.stderr_path = err;
    options.background = options.spool_limit_only = 1;
    jash_agent_process_t* process = NULL;
    int64_t start = monotonic_ms();
    CHECK(!jash_agent_process_start(&ir, &options, &process, error, sizeof(error)));
    CHECK(!jash_agent_process_wait(process, 3000));
    jash_agent_process_result_t final;
    CHECK(!jash_agent_process_destroy_checked(process, &final));
    CHECK(monotonic_ms() - start < 3000);
    CHECK(final.state == JASH_AGENT_PROCESS_DONE && final.terminal_status_valid && !final.exit_code);
    CHECK(final.quiescent && final.descendants_quiescent && final.descendants_forced);
    CHECK(!final.indeterminate && !final.cleanup_error_number);
    CHECK(kill(-final.pgid, 0) == -1 && errno == ESRCH);
    CHECK(faccessat(cwd, "escaped", F_OK, 0) == -1 && errno == ENOENT);
    CHECK(close(cwd) == 0);
    return 0;
}

int main(void) {
    char root[] = "/tmp/jash-process-children-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(!fake_source_contract(root));
    CHECK(!actual_children_and_identity());
    CHECK(!successful_leader_is_contained(root));
    const char* names[] = {"self/stat", "101/stat", "102/stat", "out", "err"};
    int directory = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); CHECK(directory >= 0);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) CHECK(!unlinkat(directory, names[i], 0));
    CHECK(!unlinkat(directory, "self", AT_REMOVEDIR));
    CHECK(!unlinkat(directory, "101", AT_REMOVEDIR));
    CHECK(!unlinkat(directory, "102", AT_REMOVEDIR));
    CHECK(!close(directory) && !rmdir(root));
    puts("agent process children fallback, identity and checked cleanup: ok");
    return 0;
}
