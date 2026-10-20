// SPDX-License-Identifier: LicenseRef-SPL-1.0

#include <core/agent_process.h>
#include <core/execution_ir.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t got_winch;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s (errno=%d: %s)\n", \
                __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return -1; \
    } \
} while (0)

#define CHILD_CHECK(condition) do { if (!(condition)) return 70; } while (0)

static void on_winch(int signo) {
    (void)signo;
    got_winch = 1;
}

static void sleep_ms(unsigned ms) {
    struct timespec delay = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int child_mode(int argc, char** argv) {
    if (argc < 2) return -1;
    if (strcmp(argv[1], "--emit") == 0) {
        if (argc != 3) return 64;
        dprintf(STDOUT_FILENO, "%s", argv[2]);
        dprintf(STDERR_FILENO, "EMIT_ERR");
        return 0;
    }
    if (strcmp(argv[1], "--sleep-ms") == 0) {
        if (argc != 3) return 64;
        sleep_ms((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (strcmp(argv[1], "--pid-exit") == 0) {
        if (argc != 3) return 64;
        dprintf(STDOUT_FILENO, "%ld\n", (long)getpid());
        return (int)strtoul(argv[2], NULL, 10);
    }
    if (strcmp(argv[1], "--write-bytes") == 0) {
        if (argc != 3) return 64;
        size_t remaining = (size_t)strtoull(argv[2], NULL, 10);
        char buffer[4096];
        memset(buffer, 'x', sizeof buffer);
        while (remaining) {
            size_t amount = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t written = write(STDOUT_FILENO, buffer, amount);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return 74;
            remaining -= (size_t)written;
        }
        return 0;
    }
    if (strcmp(argv[1], "--raise-fsize-and-write") == 0) {
        if (argc != 4) return 64;
        rlim_t expected = (rlim_t)strtoull(argv[2], NULL, 10);
        size_t remaining = (size_t)strtoull(argv[3], NULL, 10);
        struct rlimit current;
        CHILD_CHECK(getrlimit(RLIMIT_FSIZE, &current) == 0);
        CHILD_CHECK(current.rlim_cur == expected);
        CHILD_CHECK(current.rlim_max == expected);
        struct rlimit raised = {
            .rlim_cur = expected + 4096,
            .rlim_max = expected + 4096,
        };
        CHILD_CHECK(setrlimit(RLIMIT_FSIZE, &raised) == -1);
        dprintf(STDERR_FILENO, "RLIMIT_LOCKED\n");
        char buffer[4096];
        memset(buffer, 'x', sizeof buffer);
        while (remaining) {
            size_t amount = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t written = write(STDOUT_FILENO, buffer, amount);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return 74;
            remaining -= (size_t)written;
        }
        return 0;
    }
    if (strcmp(argv[1], "--ignore-xfsz-and-write") == 0) {
        if (argc != 3) return 64;
        signal(SIGXFSZ, SIG_IGN);
        size_t remaining = (size_t)strtoull(argv[2], NULL, 10);
        char buffer[4096];
        memset(buffer, 'x', sizeof buffer);
        while (remaining) {
            size_t amount = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t written = write(STDOUT_FILENO, buffer, amount);
            if (written < 0 && errno == EINTR) continue;
            if (written < 0 && errno == EFBIG) return 0;
            if (written <= 0) return 74;
            remaining -= (size_t)written;
        }
        return 75;
    }
    if (strcmp(argv[1], "--raise-signal") == 0) {
        if (argc != 3) return 64;
        raise((int)strtoul(argv[2], NULL, 10));
        return 71;
    }
    if (strcmp(argv[1], "--spawn-descendant") == 0) {
        if (argc != 2) return 64;
        signal(SIGTERM, SIG_IGN);
        pid_t descendant = fork();
        CHILD_CHECK(descendant >= 0);
        if (descendant == 0) {
            signal(SIGTERM, SIG_IGN);
            for (;;) pause();
        }
        dprintf(STDOUT_FILENO, "%ld\n", (long)descendant);
        for (;;) pause();
    }
    if (strcmp(argv[1], "--spawn-detached") == 0) {
        if (argc != 3) return 64;
        pid_t descendant = fork();
        CHILD_CHECK(descendant >= 0);
        if (descendant == 0) {
            CHILD_CHECK(setsid() >= 0);
            signal(SIGTERM, SIG_IGN);
            for (;;) pause();
        }
        dprintf(STDOUT_FILENO, "%ld\n", (long)descendant);
        sleep_ms((unsigned)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (strcmp(argv[1], "--copy-err") == 0) {
        char buffer[256];
        ssize_t count;
        while ((count = read(STDIN_FILENO, buffer, sizeof buffer)) > 0)
            CHILD_CHECK(write(STDOUT_FILENO, buffer, (size_t)count) == count);
        CHILD_CHECK(count == 0);
        dprintf(STDERR_FILENO, "COPY_ERR");
        return 0;
    }
    if (strcmp(argv[1], "--close-stdin") == 0) {
        CHILD_CHECK(close(STDIN_FILENO) == 0);
        dprintf(STDOUT_FILENO, "STDIN_CLOSED\n");
        sleep_ms(30000);
        return 0;
    }
    if (strcmp(argv[1], "--context-probe") == 0) {
        if (argc != 3) return 64;
        char cwd[4096];
        unsigned char input[4];
        size_t received = 0;
        while (received < sizeof input) {
            ssize_t count = read(STDIN_FILENO, input + received,
                                 sizeof input - received);
            if (count > 0) received += (size_t)count;
            else if (count < 0 && errno == EINTR) continue;
            else return 74;
        }
        static const unsigned char expected[] = {'A', 0, 0xff, 'Z'};
        CHILD_CHECK(getcwd(cwd, sizeof cwd) != NULL);
        CHILD_CHECK(strcmp(cwd, argv[2]) == 0);
        CHILD_CHECK(getenv("W68_ENV") != NULL);
        CHILD_CHECK(strcmp(getenv("W68_ENV"), "CONTEXT") == 0);
        CHILD_CHECK(memcmp(input, expected, sizeof expected) == 0);
        dprintf(STDOUT_FILENO, "CONTEXT_OK");
        return 0;
    }
    if (strcmp(argv[1], "--fd-closed") == 0) {
        if (argc != 3) return 64;
        int fd = atoi(argv[2]);
        errno = 0;
        CHILD_CHECK(fd >= 3);
        CHILD_CHECK(fcntl(fd, F_GETFD) == -1);
        CHILD_CHECK(errno == EBADF);
        CHILD_CHECK(fcntl(STDIN_FILENO, F_GETFD) >= 0);
        CHILD_CHECK(fcntl(STDOUT_FILENO, F_GETFD) >= 0);
        CHILD_CHECK(fcntl(STDERR_FILENO, F_GETFD) >= 0);
        dprintf(STDOUT_FILENO, "FD_CLOSED");
        return 0;
    }
    if (strcmp(argv[1], "--cwd-marker") == 0) {
        if (argc != 3) return 64;
        char marker[32] = {0};
        int fd = open("marker", O_RDONLY);
        CHILD_CHECK(fd >= 0);
        CHILD_CHECK(read(fd, marker, sizeof marker - 1u) > 0);
        CHILD_CHECK(close(fd) == 0);
        CHILD_CHECK(strcmp(marker, argv[2]) == 0);
        dprintf(STDOUT_FILENO, "CWD_PINNED");
        return 0;
    }
    if (strcmp(argv[1], "--ignore-term") == 0) {
        signal(SIGTERM, SIG_IGN);
        dprintf(STDOUT_FILENO, "IGNORE_READY\n");
        for (;;) pause();
    }
    if (strcmp(argv[1], "--pty-probe") == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = on_winch;
        sigemptyset(&action.sa_mask);
        CHILD_CHECK(sigaction(SIGWINCH, &action, NULL) == 0);

        struct winsize ws;
        CHILD_CHECK(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0);
        dprintf(STDOUT_FILENO,
                "PTY_READY in=%d out=%d err=%d rows=%u cols=%u\n",
                isatty(STDIN_FILENO), isatty(STDOUT_FILENO),
                isatty(STDERR_FILENO), ws.ws_row, ws.ws_col);
        dprintf(STDERR_FILENO, "PTY_ERR\n");
        while (!got_winch) pause();
        CHILD_CHECK(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0);
        dprintf(STDOUT_FILENO, "PTY_RESIZED rows=%u cols=%u\n",
                ws.ws_row, ws.ws_col);
        return 0;
    }
    if (strcmp(argv[1], "--pty-copy") == 0) {
        if (argc != 3) return 64;
        size_t remaining = (size_t)strtoull(argv[2], NULL, 10);
        struct termios settings;
        CHILD_CHECK(tcgetattr(STDIN_FILENO, &settings) == 0);
        settings.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        settings.c_oflag &= (tcflag_t)~OPOST;
        settings.c_cflag |= CS8;
        settings.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
        settings.c_cc[VMIN] = 1;
        settings.c_cc[VTIME] = 0;
        CHILD_CHECK(tcsetattr(STDIN_FILENO, TCSANOW, &settings) == 0);
        dprintf(STDOUT_FILENO, "PTY_COPY_READY\n");
        unsigned char buffer[1024];
        while (remaining) {
            size_t wanted = remaining < sizeof buffer ? remaining : sizeof buffer;
            ssize_t count = read(STDIN_FILENO, buffer, wanted);
            if (count < 0 && errno == EINTR) continue;
            CHILD_CHECK(count > 0);
            size_t offset = 0;
            while (offset < (size_t)count) {
                ssize_t written = write(STDOUT_FILENO, buffer + offset,
                                        (size_t)count - offset);
                if (written < 0 && errno == EINTR) continue;
                CHILD_CHECK(written > 0);
                offset += (size_t)written;
            }
            remaining -= (size_t)count;
        }
        return 0;
    }
    return -1;
}

static int read_text(const char* path, char* out, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n < 0) return -1;
    out[n] = '\0';
    return (int)n;
}

static int wait_for_text(jash_agent_process_t* process, const char* path,
                         const char* needle, unsigned timeout_ms) {
    char text[4096];
    for (unsigned elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        int polled = jash_agent_process_poll(process, 10);
        if (polled < 0) return -1;
        if (read_text(path, text, sizeof text) >= 0 && strstr(text, needle))
            return 0;
        if (polled == 1) break;
    }
    return -1;
}

static int fd_count(void) {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    while (readdir(dir)) count++;
    closedir(dir);
    return count;
}

static int cleanup_root(const char* root) {
    DIR* dir = opendir(root);
    if (!dir) return -1;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char path[4096];
        int length = snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof path || unlink(path) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(root);
}

static void one_command(jash_execution_ir_t* ir, const char* command,
                        size_t argc, const char* const* argv) {
    jash_execution_ir_init(ir);
    ir->stage_count = 1;
    ir->stages[0].argc = argc;
    for (size_t i = 0; i < argc; i++) ir->stages[0].argv[i] = argv[i];
    (void)command;
}

static int no_zombie(const jash_agent_process_result_t* result) {
    errno = 0;
    CHECK(waitpid(result->launcher_pid, NULL, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    return 0;
}

static int process_is_gone(pid_t pid) {
    errno = 0;
    return kill(pid, 0) == -1 && errno == ESRCH;
}

static void kill_and_reap_test_escape(pid_t pid) {
    if (pid <= 0 || process_is_gone(pid)) return;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static int test_validation_is_effect_free(const char* root,
                                          const char* self) {
    char victim[4096];
    snprintf(victim, sizeof victim, "%s/validation-victim", root);
    const char* argv[] = {self, "--emit", "ok"};
    jash_execution_ir_t ir;
    jash_execution_ir_error_t error;
    one_command(&ir, self, 3, argv);
    ir.stages[0].redirection_count = 1;
    ir.stages[0].redirections[0] = (jash_execution_redirection_t) {
        .fd = 1, .kind = JASH_EXEC_REDIR_WRITE_TRUNC, .path = victim,
    };

    CHECK(jash_execution_ir_validate(&ir, &error) == 0);
    CHECK(access(victim, F_OK) != 0 && errno == ENOENT);

    ir.version = 2;
    CHECK(jash_execution_ir_validate(&ir, &error) != 0);
    CHECK(error == JASH_EXEC_IR_BAD_VERSION);
    CHECK(access(victim, F_OK) != 0 && errno == ENOENT);
    ir.version = JASH_EXECUTION_IR_VERSION;

    ir.stages[0].redirection_count = 2;
    ir.stages[0].redirections[1] = ir.stages[0].redirections[0];
    CHECK(jash_execution_ir_validate(&ir, &error) != 0);
    CHECK(error == JASH_EXEC_IR_DUPLICATE_REDIR_FD);
    CHECK(access(victim, F_OK) != 0 && errno == ENOENT);

    ir.stages[0].redirection_count = 1;
    ir.stages[0].redirections[0].fd = 3;
    CHECK(jash_execution_ir_validate(&ir, &error) != 0);
    CHECK(error == JASH_EXEC_IR_BAD_REDIR_FD);
    CHECK(access(victim, F_OK) != 0 && errno == ENOENT);
    return 0;
}

static int test_literal_pipeline_and_spools(const char* root,
                                            const char* self) {
    char out[4096], err[4096], text[4096], error[256];
    snprintf(out, sizeof out, "%s/literal.out", root);
    snprintf(err, sizeof err, "%s/literal.err", root);
    const char literal[] = "literal ; $(touch should-not-run) * $HOME\n";

    jash_execution_ir_t ir;
    jash_execution_ir_init(&ir);
    ir.stage_count = 2;
    ir.stages[0].argc = 3;
    ir.stages[0].argv[0] = self;
    ir.stages[0].argv[1] = "--emit";
    ir.stages[0].argv[2] = literal;
    ir.stages[1].argc = 1;
    ir.stages[1].argv[0] = "/bin/cat";

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result && result->state == JASH_AGENT_PROCESS_DONE);
    CHECK(result->exit_code == 0 && result->term_signal == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strcmp(text, literal) == 0);
    CHECK(read_text(err, text, sizeof text) >= 0);
    CHECK(strcmp(text, "EMIT_ERR") == 0);
    struct stat spool_metadata;
    CHECK(stat(out, &spool_metadata) == 0);
    CHECK(S_ISREG(spool_metadata.st_mode));
    CHECK(spool_metadata.st_uid == geteuid());
    CHECK(spool_metadata.st_nlink == 1);
    CHECK((spool_metadata.st_mode & 0777) == 0600);
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_cwd_env_and_binary_stdin(const char* root, const char* self) {
    char out[4096], err[4096], input[4096], text[128], before[4096], after[4096];
    char error[256];
    snprintf(out, sizeof out, "%s/context.out", root);
    snprintf(err, sizeof err, "%s/context.err", root);
    snprintf(input, sizeof input, "%s/context.in", root);
    CHECK(getcwd(before, sizeof before) != NULL);
    int input_fd = open(input, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(input_fd >= 0);
    static const unsigned char payload[] = {'A', 0, 0xff, 'Z'};
    CHECK(write(input_fd, payload, sizeof payload) == (ssize_t)sizeof payload);
    CHECK(lseek(input_fd, 0, SEEK_SET) == 0);

    const char* argv[] = {self, "--context-probe", root};
    const char* envp[] = {"PATH=/bin:/usr/bin", "W68_ENV=CONTEXT", NULL};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    int cwd_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd_fd >= 0);
    options.cwd_fd = cwd_fd;
    options.envp = envp;
    options.stdin_fd = input_fd;
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(close(cwd_fd) == 0);
    CHECK(close(input_fd) == 0);
    CHECK(jash_agent_process_get_result(process)->exit_code == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strcmp(text, "CONTEXT_OK") == 0);
    CHECK(getcwd(after, sizeof after) != NULL);
    CHECK(strcmp(before, after) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_cwd_is_inode_pinned(const char* root, const char* self) {
    char original[4096], renamed[4096];
    char original_marker[sizeof original + sizeof "/marker"];
    char replacement_marker[sizeof original + sizeof "/marker"];
    char out[4096], err[4096], text[128], error[256];
    snprintf(original, sizeof original, "%s/cwd-original", root);
    snprintf(renamed, sizeof renamed, "%s/cwd-renamed", root);
    snprintf(original_marker, sizeof original_marker, "%s/marker", original);
    snprintf(replacement_marker, sizeof replacement_marker, "%s/marker", original);
    snprintf(out, sizeof out, "%s/cwd-pinned.out", root);
    snprintf(err, sizeof err, "%s/cwd-pinned.err", root);
    CHECK(mkdir(original, 0700) == 0);
    int marker_fd = open(original_marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(marker_fd >= 0);
    CHECK(write(marker_fd, "original", 8) == 8);
    CHECK(close(marker_fd) == 0);
    int cwd_fd = open(original, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(cwd_fd >= 0);

    CHECK(rename(original, renamed) == 0);
    CHECK(mkdir(original, 0700) == 0);
    marker_fd = open(replacement_marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(marker_fd >= 0);
    CHECK(write(marker_fd, "replacement", 11) == 11);
    CHECK(close(marker_fd) == 0);

    const char* argv[] = {self, "--cwd-marker", "original"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.cwd_fd = cwd_fd;
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(close(cwd_fd) == 0);
    CHECK(jash_agent_process_get_result(process)->exit_code == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strcmp(text, "CWD_PINNED") == 0);
    jash_agent_process_destroy(process);

    snprintf(original_marker, sizeof original_marker, "%s/marker", renamed);
    CHECK(unlink(original_marker) == 0);
    CHECK(unlink(replacement_marker) == 0);
    CHECK(rmdir(renamed) == 0);
    CHECK(rmdir(original) == 0);
    return 0;
}

static int test_worker_closes_unknown_fds(const char* root, const char* self) {
    char sentinel_path[4096], out[4096], err[4096], fd_text[32], text[128];
    char error[256];
    snprintf(sentinel_path, sizeof sentinel_path, "%s/fd-sentinel", root);
    snprintf(out, sizeof out, "%s/fd-closed.out", root);
    snprintf(err, sizeof err, "%s/fd-closed.err", root);
    int sentinel = open(sentinel_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(sentinel >= 3);
    CHECK(fcntl(sentinel, F_SETFD, 0) == 0);
    snprintf(fd_text, sizeof fd_text, "%d", sentinel);
    const char* argv[] = {self, "--fd-closed", fd_text};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(jash_agent_process_get_result(process)->exit_code == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strcmp(text, "FD_CLOSED") == 0);
    CHECK(fcntl(sentinel, F_GETFD) >= 0);
    CHECK(close(sentinel) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_enoexec_never_falls_back_to_a_shell(const char* root) {
    char executable[4096], victim[4096], out[4096], err[4096], error[256];
    snprintf(executable, sizeof executable, "%s/plain-executable", root);
    snprintf(victim, sizeof victim, "%s/enoexec-victim", root);
    snprintf(out, sizeof out, "%s/enoexec.out", root);
    snprintf(err, sizeof err, "%s/enoexec.err", root);
    int fd = open(executable, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    CHECK(fd >= 0);
    char payload[8192];
    int payload_length = snprintf(payload, sizeof payload, "touch %s\n", victim);
    CHECK(payload_length > 0 && (size_t)payload_length < sizeof payload);
    CHECK(write(fd, payload, (size_t)payload_length) == payload_length);
    CHECK(close(fd) == 0);

    const char* argv[] = {executable};
    jash_execution_ir_t ir;
    one_command(&ir, executable, 1, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(jash_agent_process_get_result(process)->exit_code == 126);
    CHECK(access(victim, F_OK) != 0 && errno == ENOENT);
    CHECK(no_zombie(jash_agent_process_get_result(process)) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_background(const char* root, const char* self) {
    char out[4096], err[4096], error[256];
    snprintf(out, sizeof out, "%s/background.out", root);
    snprintf(err, sizeof err, "%s/background.err", root);
    const char* argv[] = {self, "--sleep-ms", "150"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->background == 1);
    CHECK(result->state == JASH_AGENT_PROCESS_RUNNING);
    size_t accepted = 99;
    CHECK(jash_agent_process_write_pty(process, "x", 1, 0, &accepted) == -1);
    CHECK(accepted == 0);
    CHECK(errno == ENOTTY);
    CHECK(jash_agent_process_write_stdin(process, "x", 1, 0, &accepted) == -1);
    CHECK(accepted == 0);
    CHECK(errno == ENOTTY);
    CHECK(jash_agent_process_wait(process, 2000) == 0);
    result = jash_agent_process_get_result(process);
    CHECK(result->state == JASH_AGENT_PROCESS_DONE && result->exit_code == 0);
    CHECK(result->completion_version == 1);
    CHECK(result->completion_notifications == 1);
#ifdef JASH_TEST_NO_PIDFD
    CHECK(result->completion_event_kind == 0);
#else
    CHECK(result->completion_event_kind == 0 || result->completion_event_kind == 1);
#endif
    for (int again = 0; again < 8; again++) {
        CHECK(jash_agent_process_wait(process, 0) == 0);
        CHECK(jash_agent_process_get_result(process)->completion_notifications == 1);
    }
    printf("completion contract: version=%u kind=%u notifications=%llu repeated_waits=8\n",
           result->completion_version, result->completion_event_kind,
           (unsigned long long)result->completion_notifications);
    CHECK(jash_agent_process_write_pty(process, "x", 1, 0, &accepted) == -1);
    CHECK(accepted == 0);
    CHECK(errno == ENOTTY);
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);

#ifdef __linux__
    char copy_out[4096], copy_err[4096];
    snprintf(copy_out, sizeof copy_out, "%s/pty-copy.out", root);
    snprintf(copy_err, sizeof copy_err, "%s/pty-copy.err", root);
    const char* copy_argv[] = {self, "--pty-copy", "16385"};
    one_command(&ir, self, 3, copy_argv);
    jash_agent_process_options_init(&options);
    options.stdout_path = copy_out;
    options.stderr_path = copy_err;
    options.background = 1;
    options.pty = 1;
    process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, copy_out, "PTY_COPY_READY", 2000) == 0);
    CHECK(jash_agent_process_write_pty(process, "T", 1, 1, &accepted) == 1);
    CHECK(accepted == 1);
    CHECK(errno == ETIMEDOUT);
    unsigned char payload[16384];
    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (unsigned char)(i * 37u);
    CHECK(jash_agent_process_write_pty(process, payload, sizeof payload,
                                       3000, &accepted) == 0);
    CHECK(accepted == sizeof payload);
    CHECK(jash_agent_process_wait(process, 3000) == 0);
    struct stat metadata;
    CHECK(stat(copy_out, &metadata) == 0);
    static const char ready[] = "PTY_COPY_READY\n";
    CHECK(metadata.st_size ==
          (off_t)(sizeof ready - 1u + 1u + sizeof payload));
    int fd = open(copy_out, O_RDONLY);
    CHECK(fd >= 0);
    unsigned char observed[sizeof ready - 1u + 1u + sizeof payload];
    size_t received = 0;
    while (received < sizeof observed) {
        ssize_t count = read(fd, observed + received, sizeof observed - received);
        if (count > 0) received += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    CHECK(close(fd) == 0);
    CHECK(received == sizeof observed);
    CHECK(memcmp(observed, ready, sizeof ready - 1u) == 0);
    CHECK(observed[sizeof ready - 1u] == 'T');
    CHECK(memcmp(observed + sizeof ready, payload, sizeof payload) == 0);
    CHECK(jash_agent_process_write_pty(process, "x", 1, 0, &accepted) == -1);
    CHECK(accepted == 0);
    CHECK(errno == EPIPE);
    jash_agent_process_destroy(process);
#endif
    return 0;
}

static int test_writable_stdin_pipe(const char* root, const char* self) {
    char out[4096], err[4096], text[128], error[256];
    snprintf(out, sizeof out, "%s/stdin-pipe.out", root);
    snprintf(err, sizeof err, "%s/stdin-pipe.err", root);
    const char* argv[] = {self, "--copy-err"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;
    options.stdin_pipe = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    size_t accepted = 0;
    CHECK(jash_agent_process_write_stdin(process, "first\n", 6, 1000,
                                         &accepted) == 0);
    CHECK(accepted == 6);
    CHECK(jash_agent_process_write_stdin(process, "second\n", 7, 1000,
                                         &accepted) == 0);
    CHECK(accepted == 7);
    CHECK(jash_agent_process_close_stdin(process) == 0);
    CHECK(jash_agent_process_wait(process, 2000) == 0);
    CHECK(read_text(out, text, sizeof text) == 13);
    CHECK(strcmp(text, "first\nsecond\n") == 0);
    CHECK(read_text(err, text, sizeof text) == 8);
    CHECK(strcmp(text, "COPY_ERR") == 0);
    CHECK(jash_agent_process_write_stdin(process, "x", 1, 0,
                                         &accepted) == -1);
    CHECK(accepted == 0 && errno == EPIPE);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_default_stdin_eof(const char* root, const char* self) {
    char out[4096], err[4096], text[128], error[256];
    snprintf(out, sizeof out, "%s/stdin-default.out", root);
    snprintf(err, sizeof err, "%s/stdin-default.err", root);
    const char* argv[] = {self, "--copy-err"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->state == JASH_AGENT_PROCESS_DONE && result->exit_code == 0);
    CHECK(read_text(out, text, sizeof text) == 0);
    CHECK(read_text(err, text, sizeof text) == 8);
    CHECK(strcmp(text, "COPY_ERR") == 0);
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_closed_stdin_reader(const char* root, const char* self) {
    char out[4096], err[4096], error[256];
    snprintf(out, sizeof out, "%s/stdin-closed.out", root);
    snprintf(err, sizeof err, "%s/stdin-closed.err", root);
    const char* argv[] = {self, "--close-stdin"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;
    options.stdin_pipe = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "STDIN_CLOSED", 2000) == 0);
    size_t accepted = 99;
    CHECK(jash_agent_process_write_stdin(process, "x", 1, 500,
                                         &accepted) == -1);
    CHECK(accepted == 0 && errno == EPIPE);
    CHECK(jash_agent_process_get_result(process)->state ==
          JASH_AGENT_PROCESS_RUNNING);
    CHECK(jash_agent_process_cancel(process, 0) == 0);
    CHECK(no_zombie(jash_agent_process_get_result(process)) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_spools_fail_closed(const char* root, const char* self) {
    char victim[4096], out[4096], err[4096], text[128], error[256];
    snprintf(victim, sizeof victim, "%s/spool-victim", root);
    snprintf(out, sizeof out, "%s/spool-link", root);
    snprintf(err, sizeof err, "%s/spool-safe.err", root);
    int fd = open(victim, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "PRESERVE", 8) == 8);
    CHECK(close(fd) == 0);
    CHECK(symlink(victim, out) == 0);

    const char* argv[] = {self, "--emit", "unsafe"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == -1);
    CHECK(process == NULL);
    CHECK(read_text(victim, text, sizeof text) == 8);
    CHECK(strcmp(text, "PRESERVE") == 0);

    CHECK(unlink(out) == 0);
    CHECK(link(victim, out) == 0);
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == -1);
    CHECK(process == NULL);
    CHECK(read_text(victim, text, sizeof text) == 8);
    CHECK(strcmp(text, "PRESERVE") == 0);

    CHECK(unlink(out) == 0);
    fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "MODE", 4) == 4);
    CHECK(close(fd) == 0);
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == -1);
    CHECK(process == NULL);
    CHECK(read_text(out, text, sizeof text) == 4);
    CHECK(strcmp(text, "MODE") == 0);

    CHECK(unlink(out) == 0);
    fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, "STDOUT", 6) == 6);
    CHECK(close(fd) == 0);
    CHECK(symlink(victim, err) == 0);
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == -1);
    CHECK(process == NULL);
    CHECK(read_text(out, text, sizeof text) == 6);
    CHECK(strcmp(text, "STDOUT") == 0);
    CHECK(read_text(victim, text, sizeof text) == 8);
    CHECK(strcmp(text, "PRESERVE") == 0);
    return 0;
}

static int test_output_limit(const char* root, const char* self) {
    char out[4096], err[4096], cap_out[4096], cap_err[4096];
    char pty_out[4096], pty_err[4096], error[256];
    snprintf(out, sizeof out, "%s/limit.out", root);
    snprintf(err, sizeof err, "%s/limit.err", root);
    snprintf(cap_out, sizeof cap_out, "%s/limit-reached.out", root);
    snprintf(cap_err, sizeof cap_err, "%s/limit-reached.err", root);
    snprintf(pty_out, sizeof pty_out, "%s/limit-pty.out", root);
    snprintf(pty_err, sizeof pty_err, "%s/limit-pty.err", root);
    const char* argv[] = {self, "--raise-fsize-and-write", "4096", "65536"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 4, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    CHECK(options.output_limit_bytes == JASH_AGENT_PROCESS_DEFAULT_OUTPUT_LIMIT);
    options.output_limit_bytes = 4096;
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->output_limit_bytes == 4096);
    CHECK(result->output_limit_hit == 1);
    CHECK(result->terminal_status_valid == 1);
    CHECK(result->term_signal == SIGXFSZ);
    struct stat st;
    CHECK(stat(out, &st) == 0);
    CHECK(st.st_size == 4096);
    char text[128];
    CHECK(read_text(err, text, sizeof text) > 0);
    CHECK(strcmp(text, "RLIMIT_LOCKED\n") == 0);
    CHECK(result->quiescent == 1);
    jash_agent_process_destroy(process);

    const char* cap_argv[] = {self, "--ignore-xfsz-and-write", "65536"};
    one_command(&ir, self, 3, cap_argv);
    options.stdout_path = cap_out;
    options.stderr_path = cap_err;
    process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    result = jash_agent_process_get_result(process);
    CHECK(result->exit_code == 0 && result->term_signal == 0);
    CHECK(result->output_limit_hit == 1);
    CHECK(stat(cap_out, &st) == 0);
    CHECK(st.st_size == 4096);
    jash_agent_process_destroy(process);

#ifdef __linux__
    one_command(&ir, self, 4, argv);
    options.stdout_path = pty_out;
    options.stderr_path = pty_err;
    options.pty = 1;
    process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    result = jash_agent_process_get_result(process);
    CHECK(result->output_limit_hit == 1);
    CHECK(result->terminal_status_valid == 1);
    CHECK(result->term_signal == SIGXFSZ);
    CHECK(stat(pty_out, &st) == 0);
    CHECK(st.st_size == 4096);
    CHECK(result->quiescent == 1);
    jash_agent_process_destroy(process);
#endif
    return 0;
}

static int test_detached_descendant_containment(const char* root,
                                                const char* self) {
    char out[4096], err[4096], text[128], error[256];
    snprintf(out, sizeof out, "%s/detached.out", root);
    snprintf(err, sizeof err, "%s/detached.err", root);
    const char* argv[] = {self, "--spawn-detached", "500"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "\n", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) > 0);
    pid_t detached = (pid_t)strtol(text, NULL, 10);
    CHECK(detached > 0 && !process_is_gone(detached));
    CHECK(jash_agent_process_get_result(process)->state ==
          JASH_AGENT_PROCESS_RUNNING);
    CHECK(jash_agent_process_wait(process, 3000) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    int gone_before_done = process_is_gone(detached);
    if (!gone_before_done) kill_and_reap_test_escape(detached);
    CHECK(gone_before_done);
    CHECK(result->state == JASH_AGENT_PROCESS_DONE);
    CHECK(result->quiescent == 1);
    CHECK(result->descendants_quiescent == 1);
    CHECK(result->descendants_forced == 1);
    jash_agent_process_destroy(process);

    const char* eof_argv[] = {self, "--spawn-detached", "5000"};
    one_command(&ir, self, 3, eof_argv);
    process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "\n", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) > 0);
    detached = (pid_t)strtol(text, NULL, 10);
    CHECK(detached > 0 && !process_is_gone(detached));
    jash_agent_process_destroy(process);
    int gone_before_eof_return = process_is_gone(detached);
    if (!gone_before_eof_return) kill_and_reap_test_escape(detached);
    CHECK(gone_before_eof_return);
    return 0;
}

static int test_pipeline_reaps_out_of_order(const char* root,
                                            const char* self) {
    char out[4096], err[4096], text[128], error[256];
    snprintf(out, sizeof out, "%s/reap.out", root);
    snprintf(err, sizeof err, "%s/reap.err", root);
    jash_execution_ir_t ir;
    jash_execution_ir_init(&ir);
    ir.stage_count = 2;
    ir.stages[0].argc = 3;
    ir.stages[0].argv[0] = self;
    ir.stages[0].argv[1] = "--sleep-ms";
    ir.stages[0].argv[2] = "750";
    ir.stages[1].argc = 3;
    ir.stages[1].argv[0] = self;
    ir.stages[1].argv[1] = "--pid-exit";
    ir.stages[1].argv[2] = "23";
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "\n", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) > 0);
    pid_t last_pid = (pid_t)strtol(text, NULL, 10);
    CHECK(last_pid > 0);
    for (unsigned elapsed = 0; elapsed < 300 && !process_is_gone(last_pid);
         elapsed += 10)
        sleep_ms(10);
    CHECK(process_is_gone(last_pid));
    CHECK(jash_agent_process_wait(process, 2000) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->exit_code == 23 && result->term_signal == 0);
    CHECK(result->terminal_status_valid == 1 && result->quiescent == 1);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_terminal_signal_is_exact(const char* root, const char* self) {
    char out[4096], err[4096], signal_arg[32], error[256];
    snprintf(out, sizeof out, "%s/signal.out", root);
    snprintf(err, sizeof err, "%s/signal.err", root);
    snprintf(signal_arg, sizeof signal_arg, "%d", SIGUSR1);
    const char* argv[] = {self, "--raise-signal", signal_arg};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    jash_agent_process_t* process = NULL;
    struct sigaction ignored, original;
    memset(&ignored, 0, sizeof ignored);
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGUSR1, &ignored, &original) == 0);
    sigset_t blocked, original_mask;
    CHECK(sigemptyset(&blocked) == 0);
    CHECK(sigaddset(&blocked, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, &original_mask) == 0);
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &original_mask, NULL) == 0);
    CHECK(sigaction(SIGUSR1, &original, NULL) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->terminal_status_valid == 1);
    CHECK(result->term_signal == SIGUSR1);
    CHECK(result->exit_code == 128 + SIGUSR1);
    CHECK(result->quiescent == 1);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_launcher_death_forces_group_quiescence(const char* root,
                                                       const char* self) {
    char out[4096], err[4096], text[128], error[256];
    snprintf(out, sizeof out, "%s/launcher-death.out", root);
    snprintf(err, sizeof err, "%s/launcher-death.err", root);
    const char* argv[] = {self, "--spawn-descendant"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "\n", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) > 0);
    pid_t descendant = (pid_t)strtol(text, NULL, 10);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    pid_t launcher = result->launcher_pid;
    pid_t worker = result->pgid;
    CHECK(descendant > 0 && worker > 0 && launcher > 0);
    CHECK(kill(launcher, SIGKILL) == 0);
    CHECK(jash_agent_process_wait(process, 5000) == -1);
    CHECK(errno == EUCLEAN);
    result = jash_agent_process_get_result(process);
    CHECK(result->state == JASH_AGENT_PROCESS_INDETERMINATE);
    CHECK(result->terminal_status_valid == 0);
    CHECK(result->launcher_unexpected == 1);
    CHECK(result->launcher_term_signal == SIGKILL);
    CHECK(result->indeterminate == 1 && result->quiescent == 0);
    CHECK(result->quiescence_forced == 1);
    CHECK(process_is_gone(worker));
    CHECK(process_is_gone(descendant));
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);

    process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "\n", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) > 0);
    descendant = (pid_t)strtol(text, NULL, 10);
    result = jash_agent_process_get_result(process);
    launcher = result->launcher_pid;
    worker = result->pgid;
    CHECK(kill(launcher, SIGKILL) == 0);
    jash_agent_process_destroy(process);
    CHECK(process_is_gone(worker));
    CHECK(process_is_gone(descendant));
    errno = 0;
    CHECK(waitpid(launcher, NULL, WNOHANG) == -1 && errno == ECHILD);
    return 0;
}

static int test_redirections_0_through_2(const char* root, const char* self) {
    char input[4096], redirected_out[4096], redirected_err[4096];
    char spool_out[4096], spool_err[4096], text[4096], error[256];
    snprintf(input, sizeof input, "%s/redirection.in", root);
    snprintf(redirected_out, sizeof redirected_out, "%s/redirection.direct.out", root);
    snprintf(redirected_err, sizeof redirected_err, "%s/redirection.direct.err", root);
    snprintf(spool_out, sizeof spool_out, "%s/redirection.spool.out", root);
    snprintf(spool_err, sizeof spool_err, "%s/redirection.spool.err", root);
    int input_fd = open(input, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(input_fd >= 0);
    CHECK(write(input_fd, "REDIRECTED", 10) == 10);
    CHECK(close(input_fd) == 0);

    const char* argv[] = {self, "--copy-err"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);
    ir.stages[0].redirection_count = 3;
    ir.stages[0].redirections[0] = (jash_execution_redirection_t) {
        .fd = 0, .kind = JASH_EXEC_REDIR_READ, .path = input,
    };
    ir.stages[0].redirections[1] = (jash_execution_redirection_t) {
        .fd = 1, .kind = JASH_EXEC_REDIR_WRITE_TRUNC, .path = redirected_out,
    };
    ir.stages[0].redirections[2] = (jash_execution_redirection_t) {
        .fd = 2, .kind = JASH_EXEC_REDIR_WRITE_APPEND, .path = redirected_err,
    };

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = spool_out;
    options.stderr_path = spool_err;
    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(jash_agent_process_get_result(process)->exit_code == 0);
    CHECK(read_text(redirected_out, text, sizeof text) >= 0);
    CHECK(strcmp(text, "REDIRECTED") == 0);
    CHECK(read_text(redirected_err, text, sizeof text) >= 0);
    CHECK(strcmp(text, "COPY_ERR") == 0);
    CHECK(read_text(spool_out, text, sizeof text) == 0);
    CHECK(read_text(spool_err, text, sizeof text) == 0);
    CHECK(no_zombie(jash_agent_process_get_result(process)) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_cancel_term_then_kill(const char* root, const char* self) {
    char out[4096], err[4096], error[256];
    snprintf(out, sizeof out, "%s/cancel.out", root);
    snprintf(err, sizeof err, "%s/cancel.err", root);
    const char* argv[] = {self, "--ignore-term"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "IGNORE_READY", 2000) == 0);
    jash_agent_process_state_t before =
        jash_agent_process_get_result(process)->state;
    CHECK(jash_agent_process_cancel(process, 50) == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(before <= result->state);
    CHECK(result->state == JASH_AGENT_PROCESS_DONE);
    CHECK(result->cancel_requested == 1 && result->kill_sent == 1);
    CHECK(result->terminal_status_valid == 1);
    CHECK(result->term_signal == SIGKILL);
    CHECK(result->quiescent == 1 && result->quiescence_forced == 1);
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);
    return 0;
}

static int test_pty_resize_and_separate_stderr(const char* root,
                                               const char* self) {
#ifndef __linux__
    (void)root;
    (void)self;
    return 0;
#else
    char out[4096], err[4096], text[4096], error[256];
    snprintf(out, sizeof out, "%s/pty.out", root);
    snprintf(err, sizeof err, "%s/pty.err", root);
    const char* argv[] = {self, "--pty-probe"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 2, argv);

    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = out;
    options.stderr_path = err;
    options.background = 1;
    options.pty = 1;
    options.pty_rows = 31;
    options.pty_cols = 91;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == 0);
    CHECK(wait_for_text(process, out, "PTY_READY", 2000) == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strstr(text, "in=1 out=1 err=0 rows=31 cols=91"));
    CHECK(jash_agent_process_resize_pty(process, 42, 112) == 0);
    CHECK(jash_agent_process_wait(process, 2000) == 0);
    CHECK(read_text(out, text, sizeof text) >= 0);
    CHECK(strstr(text, "PTY_RESIZED rows=42 cols=112"));
    CHECK(!strstr(text, "PTY_ERR"));
    CHECK(read_text(err, text, sizeof text) >= 0);
    CHECK(strcmp(text, "PTY_ERR\n") == 0);
    const jash_agent_process_result_t* result =
        jash_agent_process_get_result(process);
    CHECK(result->state == JASH_AGENT_PROCESS_DONE && result->exit_code == 0);
    size_t accepted = 99;
    CHECK(jash_agent_process_write_pty(process, "x", 1, 0, &accepted) == -1);
    CHECK(accepted == 0);
    CHECK(errno == EPIPE);
    CHECK(no_zombie(result) == 0);
    jash_agent_process_destroy(process);
    return 0;
#endif
}

static int test_nonregular_spool_is_rejected(const char* root,
                                             const char* self) {
#ifndef __linux__
    (void)root;
    (void)self;
    return 0;
#else
    char err[4096], error[256];
    snprintf(err, sizeof err, "%s/full.err", root);
    const char* argv[] = {self, "--emit", "FULL"};
    jash_execution_ir_t ir;
    one_command(&ir, self, 3, argv);
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.stdout_path = "/dev/full";
    options.stderr_path = err;
    options.background = 1;
    options.pty = 1;

    jash_agent_process_t* process = NULL;
    CHECK(jash_agent_process_start(&ir, &options, &process,
                                   error, sizeof error) == -1);
    CHECK(process == NULL);
    CHECK(access(err, F_OK) == -1 && errno == ENOENT);
    return 0;
#endif
}

int main(int argc, char** argv) {
    int mode = child_mode(argc, argv);
    if (mode >= 0) return mode;

    char self[4096];
    CHECK(realpath(argv[0], self) != NULL);
    char root[] = "/tmp/jash-agent-process-test-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    int before = fd_count();
    CHECK(before >= 0);

    /* An explicit focused gate remains runnable when a host cannot qualify
     * detached descendants. The default full suite still requires them. */
    if (argc == 2 && strcmp(argv[1], "--completion-contract") == 0) {
        CHECK(test_background(root, self) == 0);
        CHECK(test_pipeline_reaps_out_of_order(root, self) == 0);
        CHECK(test_terminal_signal_is_exact(root, self) == 0);
        CHECK(test_launcher_death_forces_group_quiescence(root, self) == 0);
        CHECK(test_cancel_term_then_kill(root, self) == 0);
        CHECK(test_pty_resize_and_separate_stderr(root, self) == 0);
        CHECK(fd_count() == before);
        CHECK(cleanup_root(root) == 0);
        puts("agent process completion/cancel/PTY contract: ok");
        return 0;
    }

    CHECK(test_validation_is_effect_free(root, self) == 0);
    CHECK(test_literal_pipeline_and_spools(root, self) == 0);
    CHECK(test_cwd_env_and_binary_stdin(root, self) == 0);
    CHECK(test_cwd_is_inode_pinned(root, self) == 0);
    CHECK(test_worker_closes_unknown_fds(root, self) == 0);
    CHECK(test_enoexec_never_falls_back_to_a_shell(root) == 0);
    CHECK(test_redirections_0_through_2(root, self) == 0);
    CHECK(test_background(root, self) == 0);
    CHECK(test_default_stdin_eof(root, self) == 0);
    CHECK(test_writable_stdin_pipe(root, self) == 0);
    CHECK(test_closed_stdin_reader(root, self) == 0);
    CHECK(test_spools_fail_closed(root, self) == 0);
    CHECK(test_output_limit(root, self) == 0);
    CHECK(test_detached_descendant_containment(root, self) == 0);
    CHECK(test_pipeline_reaps_out_of_order(root, self) == 0);
    CHECK(test_terminal_signal_is_exact(root, self) == 0);
    CHECK(test_launcher_death_forces_group_quiescence(root, self) == 0);
    CHECK(test_cancel_term_then_kill(root, self) == 0);
    CHECK(test_pty_resize_and_separate_stderr(root, self) == 0);
    CHECK(test_nonregular_spool_is_rejected(root, self) == 0);

    int after = fd_count();
    CHECK(after == before);
    CHECK(cleanup_root(root) == 0);
    printf("agent process IR: ok\n");
    return 0;
}
