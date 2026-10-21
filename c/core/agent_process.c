// SPDX-License-Identifier: LicenseRef-SPL-1.0

#define _GNU_SOURCE
#include "agent_process.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#endif

#ifdef JASH_BLIBC
/* Tina blibc declares PTY operations in its explicit POSIX pty header. */
#include <pty.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

/* EUCLEAN ("Structure needs cleaning") is Linux-only; NetBSD/macOS lack
 * it. EIO is the closest portable stand-in for the INDETERMINATE paths. */
#ifndef EUCLEAN
#define EUCLEAN EIO
#endif

extern char** environ;

struct jash_agent_process {
    jash_agent_process_result_t result;
    int pty_master_fd;
    int stdin_pipe;
    int stdin_writer_fd;
    int stdout_fd;
    int status_fd;
    int completion_fd; /* pidfd pins the launcher incarnation; never a PID lookup on wait. */
    int pty_eof;
    uint64_t pty_output_bytes;
    unsigned cancel_grace_ms;
    int launcher_reaped;
    int launcher_status;
    int completion_prepared;
    int pgid_quiescent;
    int launcher_registered;
    jash_agent_cgroup_t* cgroup;
};

#define JASH_AGENT_PROCESS_MAX_ACTIVE_LAUNCHERS 256u
static pid_t active_launchers[JASH_AGENT_PROCESS_MAX_ACTIVE_LAUNCHERS];

static int register_launcher(pid_t pid) {
    for (size_t i = 0; i < JASH_AGENT_PROCESS_MAX_ACTIVE_LAUNCHERS; i++) {
        if (active_launchers[i] != 0) continue;
        active_launchers[i] = pid;
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

static void unregister_launcher(pid_t pid) {
    for (size_t i = 0; i < JASH_AGENT_PROCESS_MAX_ACTIVE_LAUNCHERS; i++) {
        if (active_launchers[i] == pid) active_launchers[i] = 0;
    }
}

static int launcher_is_registered(pid_t pid) {
    for (size_t i = 0; i < JASH_AGENT_PROCESS_MAX_ACTIVE_LAUNCHERS; i++)
        if (active_launchers[i] == pid) return 1;
    return 0;
}

typedef struct {
    int ok;
    int error_number;
    pid_t pgid;
} launch_message_t;

#define TERMINAL_MESSAGE_MAGIC 0x4a415354u

typedef struct {
    uint32_t magic;
    uint32_t version,record_size;
    pid_t launcher_pid;
    int wait_status;
    int output_limit_hit;
    int descendants_quiescent;
    int descendants_forced;
    int resource_timed_out;
    int resource_empty;
    uint64_t resource_memory_events;
    uint64_t resource_pids_events;
} terminal_message_t;

static ssize_t write_without_sigpipe(int fd, const void* data, size_t length);

static void set_error(char* out, size_t cap, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

static void set_error(char* out, size_t cap, const char* format, ...) {
    if (!out || cap == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, cap, format, args);
    va_end(args);
}

static int fd_cloexec(int fd) {
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int fd_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

static int make_pipe(int fds[2]) {
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) == 0) return 0;
    if (errno != ENOSYS) return -1;
#endif
    if (pipe(fds) != 0) return -1;
    if (fd_cloexec(fds[0]) < 0 || fd_cloexec(fds[1]) < 0) {
        int saved = errno;
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void* data, size_t length) {
    const unsigned char* bytes = data;
    while (length) {
        ssize_t written = write(fd, bytes, length);
        if (written > 0) {
            bytes += written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int read_exact(int fd, void* data, size_t length) {
    unsigned char* bytes = data;
    size_t received = 0;
    while (received < length) {
        ssize_t count = read(fd, bytes + received, length - received);
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void transition(jash_agent_process_t* process,
                       jash_agent_process_state_t next) {
    if (next > process->result.state) process->result.state = next;
}

void jash_agent_process_options_init(jash_agent_process_options_t* options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    options->cwd_fd = -1;
    options->stdin_fd = -1;
    options->cgroup_parent_fd = -1;
    options->pty_rows = 24;
    options->pty_cols = 80;
    options->cancel_grace_ms = JASH_AGENT_PROCESS_DEFAULT_GRACE_MS;
    options->output_limit_bytes = JASH_AGENT_PROCESS_DEFAULT_OUTPUT_LIMIT;
}

static int validate_options(const jash_agent_process_options_t* options,
                            char* error, size_t error_cap) {
    if (!options) {
        set_error(error, error_cap, "process options are required");
        return -1;
    }
    if (options->resources &&
        jash_agent_limits_validate(options->resources, error, error_cap) != 0)
        return -1;
    if (!options->stdout_path || !options->stdout_path[0] ||
        !options->stderr_path || !options->stderr_path[0]) {
        set_error(error, error_cap, "separate stdout and stderr spool paths are required");
        return -1;
    }
    if (strnlen(options->stdout_path, JASH_AGENT_PROCESS_PATH_MAX + 1u) >
            JASH_AGENT_PROCESS_PATH_MAX ||
        strnlen(options->stderr_path, JASH_AGENT_PROCESS_PATH_MAX + 1u) >
            JASH_AGENT_PROCESS_PATH_MAX ||
        strcmp(options->stdout_path, options->stderr_path) == 0) {
        set_error(error, error_cap, "spool paths are too long or not distinct");
        return -1;
    }
    struct stat cwd_metadata;
    if (options->cwd_fd < -1 ||
        (options->cwd_fd >= 0 &&
         (fcntl(options->cwd_fd, F_GETFD) < 0 ||
          fstat(options->cwd_fd, &cwd_metadata) != 0 ||
          !S_ISDIR(cwd_metadata.st_mode)))) {
        set_error(error, error_cap, "cwd_fd is not an open directory");
        errno = EINVAL;
        return -1;
    }
    if (options->stdin_fd < -1 ||
        (options->stdin_fd >= 0 && fcntl(options->stdin_fd, F_GETFD) < 0)) {
        set_error(error, error_cap, "stdin_fd is invalid");
        return -1;
    }
    if ((options->pty && options->stdin_fd >= 0) ||
        (options->stdin_pipe && options->stdin_fd >= 0) ||
        (options->pty && options->stdin_pipe)) {
        set_error(error, error_cap,
                  "PTY, stdin_fd, and writable stdin pipe modes are exclusive");
        return -1;
    }
    if (options->stdin_pipe && !options->background) {
        set_error(error, error_cap,
                  "writable stdin pipe requires background supervision");
        return -1;
    }
    if (options->pty && (options->pty_rows == 0 || options->pty_rows > 65535u ||
                         options->pty_cols == 0 || options->pty_cols > 65535u)) {
        set_error(error, error_cap, "PTY dimensions must be in 1..65535");
        return -1;
    }
    if (options->cancel_grace_ms > JASH_AGENT_PROCESS_MAX_GRACE_MS) {
        set_error(error, error_cap, "cancel grace exceeds its limit");
        return -1;
    }
    if (options->spool_limit_only != 0 && options->spool_limit_only != 1) {
        set_error(error, error_cap, "spool_limit_only must be boolean");
        errno = EINVAL;
        return -1;
    }
    if (options->output_limit_bytes == 0 ||
        (uint64_t)(rlim_t)options->output_limit_bytes !=
            options->output_limit_bytes ||
        (rlim_t)options->output_limit_bytes == RLIM_INFINITY) {
        set_error(error, error_cap, "output limit is invalid");
        errno = EINVAL;
        return -1;
    }
    if (options->envp) {
        size_t bytes = 0;
        size_t count = 0;
        for (; count < JASH_AGENT_PROCESS_MAX_ENV && options->envp[count]; count++) {
            size_t length = strnlen(options->envp[count],
                                    JASH_AGENT_PROCESS_MAX_ENV_BYTES + 1u);
            if (length > JASH_AGENT_PROCESS_MAX_ENV_BYTES ||
                bytes > JASH_AGENT_PROCESS_MAX_ENV_BYTES - length - 1u) {
                set_error(error, error_cap, "environment exceeds its byte limit");
                return -1;
            }
            bytes += length + 1u;
        }
        if (count == JASH_AGENT_PROCESS_MAX_ENV && options->envp[count]) {
            set_error(error, error_cap, "environment has too many entries");
            return -1;
        }
    }
    return 0;
}

static int open_spool_untruncated(const char* path, int* out,
                                  struct stat* metadata) {
#ifndef O_NOFOLLOW
    (void)path;
    (void)out;
    (void)metadata;
    errno = ENOTSUP;
    return -1;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int created = 1;
    int fd = open(path, flags, 0600);
    if (fd < 0 && errno == EEXIST) {
        created = 0;
        flags = O_WRONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        fd = open(path, flags);
    }
    fd = fd_cloexec(fd);
    if (fd < 0) return -1;
    if (created && fchmod(fd, 0600) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (fstat(fd, metadata) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (!S_ISREG(metadata->st_mode) ||
        metadata->st_uid != geteuid() || metadata->st_nlink != 1 ||
        (metadata->st_mode & 0777) != 0600) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    *out = fd;
    return 0;
#endif
}

static int open_spools(const jash_agent_process_options_t* options,
                       int* stdout_fd, int* stderr_fd,
                       char* error, size_t error_cap) {
    struct stat stdout_metadata, stderr_metadata;
    if (open_spool_untruncated(options->stdout_path, stdout_fd,
                               &stdout_metadata) != 0) {
        set_error(error, error_cap, "cannot open stdout spool: %s", strerror(errno));
        return -1;
    }
    if (open_spool_untruncated(options->stderr_path, stderr_fd,
                               &stderr_metadata) != 0) {
        int saved = errno;
        close(*stdout_fd);
        *stdout_fd = -1;
        errno = saved;
        set_error(error, error_cap, "cannot open stderr spool: %s", strerror(errno));
        return -1;
    }
    if (stdout_metadata.st_dev == stderr_metadata.st_dev &&
        stdout_metadata.st_ino == stderr_metadata.st_ino) {
        close(*stdout_fd);
        close(*stderr_fd);
        *stdout_fd = *stderr_fd = -1;
        errno = EINVAL;
        set_error(error, error_cap,
                  "stdout and stderr spools are unsafe or not distinct");
        return -1;
    }
    if (ftruncate(*stdout_fd, 0) != 0 || ftruncate(*stderr_fd, 0) != 0) {
        int saved = errno;
        close(*stdout_fd);
        close(*stderr_fd);
        *stdout_fd = *stderr_fd = -1;
        errno = saved;
        set_error(error, error_cap,
                  "stdout and stderr spools are unsafe or not distinct");
        return -1;
    }
    return 0;
}

static int open_pty(unsigned rows, unsigned cols, int* master_fd, int* slave_fd,
                    char* error, size_t error_cap) {
#ifndef __linux__
    (void)rows;
    (void)cols;
    (void)master_fd;
    (void)slave_fd;
    errno = ENOTSUP;
    set_error(error, error_cap, "PTY execution is supported on Linux only");
    return -1;
#else
    int flags = O_RDWR | O_NOCTTY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int master = fd_cloexec(posix_openpt(flags));
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        int saved = errno;
        if (master >= 0) close(master);
        errno = saved;
        set_error(error, error_cap, "cannot allocate PTY: %s", strerror(errno));
        return -1;
    }
    char name[JASH_AGENT_PROCESS_PATH_MAX + 1u];
    int pts_error = ptsname_r(master, name, sizeof name);
    if (pts_error != 0) {
        close(master);
        errno = pts_error;
        set_error(error, error_cap, "cannot resolve PTY slave: %s", strerror(errno));
        return -1;
    }
    int slave = fd_cloexec(open(name, flags));
    if (slave < 0) {
        int saved = errno;
        close(master);
        errno = saved;
        set_error(error, error_cap, "cannot open PTY slave: %s", strerror(errno));
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    if (ioctl(slave, TIOCSWINSZ, &ws) != 0) {
        int saved = errno;
        close(master);
        close(slave);
        errno = saved;
        set_error(error, error_cap, "cannot size PTY: %s", strerror(errno));
        return -1;
    }
    int status = fcntl(master, F_GETFL, 0);
    if (status < 0 || fcntl(master, F_SETFL, status | O_NONBLOCK) != 0) {
        int saved = errno;
        close(master);
        close(slave);
        errno = saved;
        set_error(error, error_cap, "cannot make PTY nonblocking: %s", strerror(errno));
        return -1;
    }
    *master_fd = master;
    *slave_fd = slave;
    return 0;
#endif
}

static void close_pipeline(int pipes[JASH_EXEC_IR_MAX_STAGES - 1u][2],
                           size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
    }
}

static int reset_worker_signals(void) {
    sigset_t empty_mask;
    if (sigemptyset(&empty_mask) != 0 ||
        sigprocmask(SIG_SETMASK, &empty_mask, NULL) != 0)
        return -1;

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int signo = 1; signo < NSIG; signo++) {
        if (signo == SIGKILL || signo == SIGSTOP) continue;
        if (sigaction(signo, &action, NULL) != 0 && errno != EINVAL)
            return -1;
    }
    return 0;
}

static int close_worker_fds_loop(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return -1;
    rlim_t upper = limit.rlim_max;
    if (upper == RLIM_INFINITY || upper > (rlim_t)INT_MAX) {
        long open_max = sysconf(_SC_OPEN_MAX);
        if (open_max < 0 || open_max > INT_MAX) return -1;
        upper = (rlim_t)open_max;
    }
    for (int fd = 3; (rlim_t)fd < upper; fd++) {
        if (close(fd) == 0 || errno == EBADF) continue;
        if (errno == EINTR) {
            fd--;
            continue;
        }
        return -1;
    }
    return 0;
}

static int close_worker_fds(void) {
#ifdef __linux__
#if defined(SYS_close_range) && \
    !defined(JASH_AGENT_PROCESS_TEST_CLOSE_RANGE_FALLBACK)
    if (syscall(SYS_close_range, 3u, UINT_MAX, 0u) == 0) return 0;
#endif
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return close_worker_fds_loop();
    int scan_fd = dirfd(directory);
    if (scan_fd < 0) {
        closedir(directory);
        return close_worker_fds_loop();
    }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (!entry) {
            if (errno != 0) result = -1;
            break;
        }
        char* end = NULL;
        errno = 0;
        long value = strtol(entry->d_name, &end, 10);
        if (errno || end == entry->d_name || *end || value < 3 ||
            value > INT_MAX || value == scan_fd)
            continue;
        if (close((int)value) != 0 && errno != EBADF && errno != EINTR) {
            result = -1;
            break;
        }
    }
    int saved = errno;
    if (closedir(directory) != 0 && result == 0)
        return close_worker_fds_loop();
    errno = saved;
    return result == 0 ? 0 : close_worker_fds_loop();
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
    closefrom(3);
    return 0;
#else
    return close_worker_fds_loop();
#endif
}

static int open_redirection(const jash_execution_redirection_t* redir) {
    int flags;
    switch (redir->kind) {
    case JASH_EXEC_REDIR_READ:
        flags = O_RDONLY;
        break;
    case JASH_EXEC_REDIR_WRITE_TRUNC:
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case JASH_EXEC_REDIR_WRITE_APPEND:
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return fd_cloexec(open(redir->path, flags, 0600));
}

static const char* environment_value(const char* const* envp,
                                     const char* name) {
    size_t name_length = strlen(name);
    for (size_t i = 0; envp && envp[i]; i++)
        if (strncmp(envp[i], name, name_length) == 0 && envp[i][name_length] == '=')
            return envp[i] + name_length + 1u;
    return NULL;
}

static void exec_literal(const jash_execution_stage_t* stage,
                         const char* const* requested_envp) {
    char* argv[JASH_EXEC_IR_MAX_ARGV + 1u];
    for (size_t i = 0; i < stage->argc; i++) argv[i] = (char*)stage->argv[i];
    argv[stage->argc] = NULL;
    char* const* envp = requested_envp ? (char* const*)requested_envp : environ;
    const char* command = stage->argv[0];
    if (strchr(command, '/')) {
        execve(command, argv, envp);
        return;
    }

    const char* path = requested_envp ? environment_value(requested_envp, "PATH")
                                      : getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    int saw_access_denied = 0;
    const char* cursor = path;
    for (;;) {
        const char* colon = strchr(cursor, ':');
        size_t directory_length = colon ? (size_t)(colon - cursor) : strlen(cursor);
        size_t command_length = strlen(command);
        char candidate[JASH_AGENT_PROCESS_PATH_MAX + 1u];
        if (directory_length == 0) {
            if (command_length < sizeof candidate) {
                memcpy(candidate, command, command_length + 1u);
                execve(candidate, argv, envp);
                if (errno == EACCES) saw_access_denied = 1;
                else if (errno != ENOENT && errno != ENOTDIR) return;
            }
        } else if (directory_length + 1u + command_length < sizeof candidate) {
            memcpy(candidate, cursor, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1u, command, command_length + 1u);
            execve(candidate, argv, envp);
            if (errno == EACCES) saw_access_denied = 1;
            else if (errno != ENOENT && errno != ENOTDIR) return;
        }
        if (!colon) break;
        cursor = colon + 1;
    }
    errno = saw_access_denied ? EACCES : ENOENT;
}

static void worker_main(const jash_execution_ir_t* ir, size_t index,
                        pid_t pgid, int gate_read,
                        int pipes[JASH_EXEC_IR_MAX_STAGES - 1u][2],
                        int stdout_fd, int stderr_fd, int stdin_fd,
                        int pty_slave_fd, int control_fd,
                        const jash_agent_process_options_t* options) {
    close(control_fd);
    if (setpgid(0, pgid) != 0) _exit(125);
    char gate_byte = 0;
    if (read_exact(gate_read, &gate_byte, 1) != 0 || gate_byte != 'G') _exit(125);

    int input = index == 0 ? (options->pty ? pty_slave_fd : stdin_fd)
                           : pipes[index - 1u][0];
    int output = index + 1u == ir->stage_count
        ? (options->pty ? pty_slave_fd : stdout_fd)
        : pipes[index][1];
    if (dup2(input, STDIN_FILENO) < 0 || dup2(output, STDOUT_FILENO) < 0 ||
        dup2(stderr_fd, STDERR_FILENO) < 0)
        _exit(125);

    const jash_execution_stage_t* stage = &ir->stages[index];
    for (size_t r = 0; r < stage->redirection_count; r++) {
        int fd = open_redirection(&stage->redirections[r]);
        if (fd < 0 || dup2(fd, (int)stage->redirections[r].fd) < 0) {
            if (fd >= 0) close(fd);
            dprintf(STDERR_FILENO, "jash-agent-process: redirection failed: %s\n",
                    strerror(errno));
            _exit(126);
        }
        close(fd);
    }

    close(gate_read);
    close_pipeline(pipes, ir->stage_count - 1u);
    if (stdout_fd >= 0) close(stdout_fd);
    if (stderr_fd >= 0) close(stderr_fd);
    if (stdin_fd >= 0) close(stdin_fd);
    if (pty_slave_fd >= 0) close(pty_slave_fd);
    if (reset_worker_signals() != 0) _exit(125);
    if (close_worker_fds() != 0) _exit(125);
    exec_literal(stage, options->envp);
    dprintf(STDERR_FILENO, "jash-agent-process: exec '%s' failed: %s\n",
            stage->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static void send_launch_failure(int control_fd, int error_number) {
    launch_message_t message = {.ok = 0, .error_number = error_number, .pgid = -1};
    (void)write_all(control_fd, &message, sizeof message);
}

static void reap_launcher_children(size_t count) {
    size_t reaped = 0;
    while (reaped < count) {
        pid_t waited = waitpid(-1, NULL, 0);
        if (waited > 0) {
            reaped++;
            continue;
        }
        if (waited < 0 && errno == EINTR) continue;
        break;
    }
}

#define JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS 4096u
#define JASH_AGENT_PROCESS_CHILDREN_TEXT_CAP       65536u
#define JASH_AGENT_PROCESS_PROC_ENTRIES_MAX        65536u
#define JASH_AGENT_PROCESS_CLEANUP_MS              5000

/* A cleanup deadline is a failure boundary, never evidence of quiescence.
 * In particular, a missing /proc source must not make destroy retry forever. */
static int cleanup_deadline(int timeout_ms, int64_t* deadline) {
    int64_t now = monotonic_ms();
    if (now < 0) return -1;
    *deadline = now + (timeout_ms < 0 ? JASH_AGENT_PROCESS_CLEANUP_MS : timeout_ms);
    return 0;
}

static int cleanup_pause(int64_t deadline) {
    int64_t now = monotonic_ms();
    if (now < 0) return -1;
    if (now >= deadline) { errno = ETIMEDOUT; return -1; }
    int remaining = deadline - now < 10 ? (int)(deadline - now) : 10;
    int result = poll(NULL, 0, remaining);
    return result < 0 && errno != EINTR ? -1 : 0;
}

#ifdef __linux__
/* Read the identity and parent from the kernel's complete stat record. comm
 * may contain spaces, newlines and ')' characters, so only its final ')' is
 * a delimiter. An unrelated or malformed record is never an empty source. */
static int proc_stat_parent(int proc_fd, const char* path, pid_t expected,
                            pid_t* parent) {
    int fd = openat(proc_fd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    char text[4096]; size_t used = 0; int error = 0;
    for (;;) {
        ssize_t count = read(fd, text + used, sizeof(text) - 1u - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        used += (size_t)count;
        if (used == sizeof(text) - 1u) {
            unsigned char extra;
            do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
            if (count) error = count < 0 ? errno : EOVERFLOW;
            break;
        }
    }
    if (close(fd) && !error) error = errno;
    if (error) { errno = error; return -1; }
    if (!used || text[used - 1u] != '\n' || memchr(text, 0, used)) {
        errno = EIO; return -1;
    }
    text[used] = 0;
    char* end = NULL; errno = 0;
    long identifier = strtol(text, &end, 10);
    if (errno || text[0] < '1' || text[0] > '9' || identifier <= 0 || identifier > INT_MAX ||
        identifier != (long)expected || end == text || end[0] != ' ' || end[1] != '(') {
        errno = EIO; return -1;
    }
    char* close_comm = strrchr(end + 2, ')');
    if (!close_comm || close_comm[1] != ' ' ||
        !((close_comm[2] >= 'A' && close_comm[2] <= 'Z') ||
          (close_comm[2] >= 'a' && close_comm[2] <= 'z')) || close_comm[3] != ' ') {
        errno = EIO; return -1;
    }
    const char* field = close_comm + 4;
    errno = 0; long ppid = strtol(field, &end, 10);
    if (errno || end == field || ppid < 0 || ppid > INT_MAX || *end != ' ') {
        errno = EIO; return -1;
    }
    *parent = (pid_t)ppid;
    return 0;
}

static int direct_children_from_proc(int proc_fd, pid_t self, pid_t* children,
                                     size_t cap, size_t* child_count) {
    int scan_fd = dup(proc_fd);
    if (scan_fd < 0) return -1;
    DIR* directory = fdopendir(scan_fd);
    if (!directory) { int error = errno; close(scan_fd); errno = error; return -1; }
    size_t examined = 0, found = 0; int error = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (++examined > JASH_AGENT_PROCESS_PROC_ENTRIES_MAX) { error = EOVERFLOW; break; }
        const char* name = entry->d_name;
        if (*name < '0' || *name > '9') continue;
        char* end = NULL; errno = 0; long pid = strtol(name, &end, 10);
        if (errno || *end || *name == '0' || pid <= 0 || pid > INT_MAX) { error = EIO; break; }
        char path[64]; int length = snprintf(path, sizeof(path), "%s/stat", name);
        if (length < 0 || (size_t)length >= sizeof(path)) { error = EOVERFLOW; break; }
        pid_t parent = 0;
        if (proc_stat_parent(proc_fd, path, (pid_t)pid, &parent)) {
            /* A disappearing process cannot become our live child. All other
             * read failures leave the population unknown, including EACCES. */
            if (errno == ENOENT || errno == ESRCH) continue;
            error = errno; break;
        }
        if (parent != self) continue;
        if (found == cap) { error = EOVERFLOW; break; }
        children[found++] = (pid_t)pid;
    }
    if (closedir(directory) && !error) error = errno;
    if (error) { errno = error; return -1; }
    *child_count = found;
    return 0;
}
#endif

static int read_direct_children(pid_t* children, size_t cap,
                                size_t* child_count) {
#ifndef __linux__
    (void)children;
    (void)cap;
    (void)child_count;
    errno = ENOTSUP;
    return -1;
#else
    *child_count = 0;
    int proc_fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (proc_fd < 0) return -1;
    pid_t self = getpid(), parent = 0;
    int identity = proc_stat_parent(proc_fd, "self/stat", self, &parent);
    if (identity || parent != getppid()) {
        int error = identity ? errno : EIO;
        close(proc_fd); errno = error; return -1;
    }
    char path[128];
    int length = snprintf(path, sizeof path, "self/task/%ld/children", (long)self);
    if (length < 0 || (size_t)length >= sizeof path) {
        close(proc_fd);
        errno = EOVERFLOW;
        return -1;
    }
    int fd;
#ifdef JASH_AGENT_PROCESS_TEST_NO_CHILDREN
    fd = -1; errno = ENOENT;
#else
    fd = openat(proc_fd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
#endif
    if (fd < 0) {
        int error = errno, result = -1;
        /* Some Linux kernels omit task/children (CONFIG_CHECKPOINT_RESTORE).
         * A complete bounded stat scan provides the same direct-parent fact. */
        if (error == ENOENT || error == ENOTDIR || error == ENOSYS || error == ENOTSUP) {
            result = direct_children_from_proc(proc_fd, self, children, cap, child_count);
            error = errno;
        }
        if (close(proc_fd) && !result) { result = -1; error = errno; }
        errno = error; return result;
    }
    if (close(proc_fd)) { int error = errno; close(fd); errno = error; return -1; }
    char text[JASH_AGENT_PROCESS_CHILDREN_TEXT_CAP];
    size_t used = 0;
    for (;;) {
        ssize_t count = read(fd, text + used, sizeof text - 1u - used);
        if (count > 0) {
            used += (size_t)count;
            if (used == sizeof text - 1u) {
                char extra;
                do { count = read(fd, &extra, 1); }
                while (count < 0 && errno == EINTR);
                int read_error = count < 0 ? errno : 0;
                int close_error = close(fd) ? errno : 0;
                if (count != 0) {
                    errno = read_error ? read_error : EOVERFLOW;
                    return -1;
                }
                if (close_error) { errno = close_error; return -1; }
                break;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        int saved = errno;
        int close_error = close(fd) ? errno : 0;
        if (count < 0) {
            errno = saved;
            return -1;
        }
        if (close_error) { errno = close_error; return -1; }
        break;
    }
    text[used] = '\0';
    size_t found = 0;
    char* cursor = text;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\t') cursor++;
        if (!*cursor) break;
        errno = 0;
        char* end = NULL;
        long value = strtol(cursor, &end, 10);
        if (errno != 0 || *cursor < '1' || *cursor > '9' || end == cursor || value <= 0 || value > INT_MAX ||
            (*end && *end != ' ' && *end != '\n' && *end != '\t') || found == cap) {
            errno = errno ? errno : EOVERFLOW;
            return -1;
        }
        for (size_t i = 0; i < found; i++) if (children[i] == (pid_t)value) {
            errno = EIO; return -1;
        }
        children[found++] = (pid_t)value;
        cursor = end;
    }
    *child_count = found;
    return 0;
#endif
}

static int kill_pid_safely(pid_t pid, int* forced) {
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0u);
    if (pidfd < 0) return errno == ESRCH ? 0 : -1;
    /* Pin first, then verify that the numeric identity still names a child
     * we may reap. Never signal a foreign process found after PID reuse. */
    siginfo_t information = {0};
    int checked;
    do { checked = waitid(P_PID, (id_t)pid, &information, WEXITED | WNOHANG | WNOWAIT); }
    while (checked < 0 && errno == EINTR);
    if (checked < 0 || information.si_pid) {
        int error = errno; close(pidfd);
        if (checked < 0 && error != ECHILD && error != ESRCH) { errno = error; return -1; }
        return 0;
    }
    int result = (int)syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, NULL, 0u);
    int saved = errno;
    close(pidfd);
    if (result != 0 && saved != ESRCH) {
        errno = saved;
        return -1;
    }
    if (!result) *forced = 1;
    return 0;
#else
    (void)pid;
    (void)forced;
    errno = ENOTSUP;
    return -1;
#endif
}

static int kill_and_reap_launcher_descendants(int* forced) {
    int64_t deadline;
    if (cleanup_deadline(-1, &deadline)) return -1;
    for (;;) {
        pid_t waited = waitpid(-1, NULL, WNOHANG);
        if (waited > 0) continue;
        if (waited < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) return 0;
            return -1;
        }

        pid_t children[JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS];
        size_t child_count = 0;
        if (read_direct_children(children,
                                 JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS,
                                 &child_count) != 0)
            return -1;
        if (child_count == 0) {
            /* Recheck waitpid after the snapshot: an exiting child can leave
             * it empty, but only ECHILD proves that the reaper has no child. */
            if (cleanup_pause(deadline)) return -1;
            continue;
        }
        for (size_t i = 0; i < child_count; i++) {
            if (kill_pid_safely(children[i], forced) != 0) return -1;
        }
        if (cleanup_pause(deadline)) return -1;
    }
}

static int kill_and_reap_unregistered_children(int* forced) {
    int64_t deadline;
    if (cleanup_deadline(-1, &deadline)) return -1;
    for (;;) {
        pid_t children[JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS];
        size_t child_count = 0;
        if (read_direct_children(children,
                                 JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS,
                                 &child_count) != 0)
            return -1;
        size_t unregistered = 0;
        for (size_t i = 0; i < child_count; i++) {
            if (launcher_is_registered(children[i])) continue;
            unregistered++;
            if (kill_pid_safely(children[i], forced) != 0) return -1;
        }
        if (unregistered == 0) return 0;
        for (size_t i = 0; i < child_count; i++) {
            if (launcher_is_registered(children[i])) continue;
            pid_t waited;
            do { waited = waitpid(children[i], NULL, WNOHANG); }
            while (waited < 0 && errno == EINTR);
            if (waited < 0 && errno != ECHILD) return -1;
        }
        if (cleanup_pause(deadline)) return -1;
    }
}

static int spool_reached_limit(int fd, uint64_t limit, int* reached) {
    struct stat metadata;
    if (fstat(fd, &metadata) != 0) return -1;
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        errno = EIO;
        return -1;
    }
    if ((uint64_t)metadata.st_size >= limit) *reached = 1;
    return 0;
}

static void launcher_main(const jash_execution_ir_t* ir,
                          const jash_agent_process_options_t* options,
                          int stdout_fd, int stderr_fd, int stdin_fd,
                          int stdin_writer_fd,
                          int pty_master_fd, int pty_slave_fd,
                          int control_read, int control_write,
                          jash_agent_cgroup_t* cgroup) {
    close(control_read);
    char registered = 0;
    if (read_exact(control_write, &registered, 1) != 0 || registered != 'R') _exit(125);
    if (stdin_writer_fd >= 0) close(stdin_writer_fd);
    if (pty_master_fd >= 0) close(pty_master_fd);
    if (options->cwd_fd >= 0 && fchdir(options->cwd_fd) != 0) {
        send_launch_failure(control_write, errno);
        _exit(125);
    }
    if (options->cwd_fd >= 0) close(options->cwd_fd);
    if (options->close_in_launcher_count) {
        if (!options->close_in_launcher || options->close_in_launcher_count > 16) {
            send_launch_failure(control_write, EINVAL); _exit(125);
        }
        int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullfd < 0) { send_launch_failure(control_write, errno); _exit(125); }
        for (size_t i = 0; i < options->close_in_launcher_count; i++) {
            int fd = options->close_in_launcher[i];
            if (fd < 0) continue;
            if (fd == control_write || fd == stdout_fd || fd == stderr_fd || fd == stdin_fd || fd == pty_slave_fd || fd == nullfd) {
                send_launch_failure(control_write, EINVAL); _exit(125);
            }
            if (fd < 3) {
                if (dup2(nullfd, fd) < 0) { send_launch_failure(control_write, errno); _exit(125); }
            } else close(fd);
        }
        close(nullfd);
    }
    if (options->pty) {
        if (setsid() < 0 || ioctl(pty_slave_fd, TIOCSCTTY, 0) != 0) {
            send_launch_failure(control_write, errno);
            _exit(125);
        }
    }
    if (!options->spool_limit_only) {
        struct rlimit file_limit;
        if (getrlimit(RLIMIT_FSIZE, &file_limit) != 0) {
            send_launch_failure(control_write, errno);
            _exit(125);
        }
        if (file_limit.rlim_max != RLIM_INFINITY &&
            (rlim_t)options->output_limit_bytes > file_limit.rlim_max) {
            send_launch_failure(control_write, EFBIG);
            _exit(125);
        }
        file_limit.rlim_cur = (rlim_t)options->output_limit_bytes;
        file_limit.rlim_max = (rlim_t)options->output_limit_bytes;
        if (setrlimit(RLIMIT_FSIZE, &file_limit) != 0) {
            send_launch_failure(control_write, errno);
            _exit(125);
        }
    }
    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR || signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        send_launch_failure(control_write, errno);
        _exit(125);
    }
#ifdef __linux__
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
        send_launch_failure(control_write, errno);
        _exit(125);
    }
#else
    send_launch_failure(control_write, ENOTSUP);
    _exit(125);
#endif

    int pipeline[JASH_EXEC_IR_MAX_STAGES - 1u][2];
    for (size_t i = 0; i < JASH_EXEC_IR_MAX_STAGES - 1u; i++)
        pipeline[i][0] = pipeline[i][1] = -1;
    for (size_t i = 0; i + 1u < ir->stage_count; i++) {
        if (make_pipe(pipeline[i]) != 0) {
            int saved = errno;
            close_pipeline(pipeline, i);
            send_launch_failure(control_write, saved);
            _exit(125);
        }
    }
    int gate[2] = {-1, -1};
    if (make_pipe(gate) != 0) {
        int saved = errno;
        close_pipeline(pipeline, ir->stage_count - 1u);
        send_launch_failure(control_write, saved);
        _exit(125);
    }

    pid_t workers[JASH_EXEC_IR_MAX_STAGES];
    size_t worker_count = 0;
    pid_t pgid = 0;
    for (size_t i = 0; i < ir->stage_count; i++) {
        pid_t child = fork();
        if (child == 0) {
            close(gate[1]);
            worker_main(ir, i, pgid, gate[0], pipeline, stdout_fd, stderr_fd,
                        stdin_fd, pty_slave_fd, control_write, options);
        }
        if (child < 0) {
            int saved = errno;
            if (pgid > 0) (void)kill(-pgid, SIGKILL);
            close(gate[0]);
            close(gate[1]);
            reap_launcher_children(worker_count);
            close_pipeline(pipeline, ir->stage_count - 1u);
            send_launch_failure(control_write, saved);
            _exit(125);
        }
        if (pgid == 0) pgid = child;
        if (setpgid(child, pgid) != 0 && errno != EACCES) {
            int saved = errno;
            (void)kill(-pgid, SIGKILL);
            close(gate[0]);
            close(gate[1]);
            workers[worker_count++] = child;
            reap_launcher_children(worker_count);
            close_pipeline(pipeline, ir->stage_count - 1u);
            send_launch_failure(control_write, saved);
            _exit(125);
        }
        workers[worker_count++] = child;
    }

    if (cgroup) {
        for (size_t i = 0; i < worker_count; i++)
            if (jash_agent_cgroup_attach(cgroup, workers[i]) != 0) goto launch_abort;
        /* Migration bypasses pids admission, so verify the blocked population. */
        jash_agent_cgroup_snapshot_t snapshot;
        if (jash_agent_cgroup_snapshot(cgroup, &snapshot) != 0) goto launch_abort;
        if (snapshot.pids_current > options->resources->pids_max ||
            snapshot.pids_current != worker_count || snapshot.memory_events || snapshot.pids_events) {
            errno = EBUSY;
            goto launch_abort;
        }
    }

    if (options->pty) {
        signal(SIGTTOU, SIG_IGN);
        if (tcsetpgrp(pty_slave_fd, pgid) != 0) {
            int saved = errno;
            (void)kill(-pgid, SIGKILL);
            close(gate[0]);
            close(gate[1]);
            reap_launcher_children(worker_count);
            close_pipeline(pipeline, ir->stage_count - 1u);
            send_launch_failure(control_write, saved);
            _exit(125);
        }
    }

    launch_message_t message = {.ok = 1, .error_number = 0, .pgid = pgid};
    if (write_all(control_write, &message, sizeof message) != 0) goto launch_abort;
    /* The parent must have registered us before any payload is released. EOF
     * at either gate is failure, including launcher/parent death during setup. */
    char acknowledged = 0;
    if (read_exact(control_write, &acknowledged, 1) != 0 || acknowledged != 'G') {
        errno = EIO;
        goto launch_abort;
    }
    int64_t started = monotonic_ms();
    if (started < 0) goto launch_abort;
    char tokens[JASH_EXEC_IR_MAX_STAGES];
    memset(tokens, 'G', worker_count);
    if (write_all(gate[1], tokens, worker_count) != 0) goto launch_abort;
    close(gate[0]);
    close(gate[1]);
    close_pipeline(pipeline, ir->stage_count - 1u);
    close(stdin_fd);
    if (pty_slave_fd >= 0) close(pty_slave_fd);

    int last_status = 125 << 8;
    int output_limit_hit = 0;
    int resource_timed_out = 0;
    int resource_failed = 0;
    jash_agent_cgroup_snapshot_t resources = {0};
    int worker_reaped[JASH_EXEC_IR_MAX_STAGES] = {0};
    size_t direct_reaped = 0;
    while (direct_reaped < worker_count) {
        int status = 0;
        pid_t waited;
        size_t drained = 0;
        /* Reap ready statuses before deciding whether a late wake is a timeout.
         * Bound draining so continuous descendant exits cannot starve the timer. */
        do {
            do { waited = waitpid(-1, &status, (options->spool_limit_only || cgroup) ? WNOHANG : 0); }
            while (waited < 0 && errno == EINTR);
            if (waited <= 0) break;
            for (size_t w = 0; w < worker_count; w++) {
                if (waited != workers[w] || worker_reaped[w]) continue;
                worker_reaped[w] = 1;
                direct_reaped++;
                if (w + 1u == worker_count) last_status = status;
                break;
            }
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ)
                output_limit_hit = 1;
        } while (cgroup && direct_reaped < worker_count &&
                 ++drained < JASH_AGENT_PROCESS_MAX_TRACKED_DESCENDANTS);
        if (waited < 0) break;
        int wait_ms = 10;
        if (cgroup) {
            jash_agent_cgroup_snapshot_t snapshot = {0};
            if (jash_agent_cgroup_snapshot(cgroup, &snapshot) != 0) resource_failed = 1;
            else {
                if (snapshot.memory_events > resources.memory_events)
                    resources.memory_events = snapshot.memory_events;
                if (snapshot.pids_events > resources.pids_events)
                    resources.pids_events = snapshot.pids_events;
            }
            int64_t now = monotonic_ms();
            if (now < 0) resource_failed = 1;
            else {
                int64_t remaining = (int64_t)options->resources->wall_timeout_ms - (now - started);
                if (remaining <= 0 && !resource_failed) {
                    /* Observe population after the deadline observation, not
                     * from a snapshot taken before the worker could exit. */
                    int empty = jash_agent_cgroup_empty(cgroup);
                    if (empty < 0) resource_failed = 1;
                    else if (!empty) resource_timed_out = 1;
                }
                if (remaining < wait_ms) wait_ms = remaining > 0 ? (int)remaining : 1;
            }
            if (resource_timed_out || resource_failed || output_limit_hit) {
                if (jash_agent_cgroup_kill(cgroup) != 0) resource_failed = 1;
                (void)kill(-pgid, SIGKILL);
            }
        }
        /* Assertion checks may build large artifacts. Their spool threshold is
         * polled instead of imposing RLIMIT_FSIZE on every file they create. */
        if ((options->spool_limit_only || cgroup) && !output_limit_hit) {
            if (spool_reached_limit(stdout_fd, options->output_limit_bytes,
                                    &output_limit_hit) != 0 ||
                spool_reached_limit(stderr_fd, options->output_limit_bytes,
                                    &output_limit_hit) != 0)
                output_limit_hit = 1;
            if (output_limit_hit) {
                if (cgroup && jash_agent_cgroup_kill(cgroup) != 0) resource_failed = 1;
                (void)kill(-pgid, SIGKILL);
            }
        }
        if (!waited) (void)poll(NULL, 0, wait_ms);
    }
    int descendants_forced = 0;
    if (cgroup) {
        int empty = jash_agent_cgroup_empty(cgroup);
        if (empty == 0) descendants_forced = 1;
        if (empty < 0 || jash_agent_cgroup_kill(cgroup) != 0) resource_failed = 1;
    }
    int descendants_quiescent = direct_reaped == worker_count &&
        kill_and_reap_launcher_descendants(&descendants_forced) == 0;
    if (cgroup) {
        int64_t cleanup_deadline = monotonic_ms() + 5000;
        while (jash_agent_cgroup_empty(cgroup) == 0 && monotonic_ms() < cleanup_deadline)
            (void)poll(NULL, 0, 10);
        jash_agent_cgroup_snapshot_t snapshot;
        if (jash_agent_cgroup_snapshot(cgroup, &snapshot) != 0) resource_failed = 1;
        else {
            resources.empty = snapshot.empty;
            if (snapshot.memory_events > resources.memory_events)
                resources.memory_events = snapshot.memory_events;
            if (snapshot.pids_events > resources.pids_events)
                resources.pids_events = snapshot.pids_events;
        }
        if (!resources.empty) resource_failed = 1;
    }
    int spools_measured = spool_reached_limit(
        stdout_fd, options->output_limit_bytes, &output_limit_hit) == 0 &&
        spool_reached_limit(stderr_fd, options->output_limit_bytes,
                            &output_limit_hit) == 0;
    close(stdout_fd);
    close(stderr_fd);
    int terminal_written = -1;
    if (direct_reaped == worker_count && descendants_quiescent &&
        spools_measured && !resource_failed) {
        terminal_message_t terminal = {
            .magic = TERMINAL_MESSAGE_MAGIC,
            .version = 1,
            .record_size = sizeof(terminal_message_t),
            .launcher_pid = getpid(),
            .wait_status = last_status,
            .output_limit_hit = output_limit_hit,
            .descendants_quiescent = 1,
            .descendants_forced = descendants_forced,
            .resource_timed_out = resource_timed_out,
            .resource_empty = resources.empty,
            .resource_memory_events = resources.memory_events,
            .resource_pids_events = resources.pids_events,
        };
        terminal_written = write_all(control_write, &terminal, sizeof terminal);
    }
    close(control_write);
    _exit(terminal_written == 0 && descendants_quiescent && spools_measured
              ? 0 : 125);

launch_abort:
    {
        int saved = errno;
        if (cgroup) (void)jash_agent_cgroup_kill(cgroup);
        for (size_t i = 0; i < worker_count; i++) (void)kill(workers[i], SIGKILL);
        close(gate[0]);
        close(gate[1]);
        reap_launcher_children(worker_count);
        send_launch_failure(control_write, saved);
        _exit(125);
    }
}

static void close_process_fds(jash_agent_process_t* process) {
    if(process->completion_fd>=0) {
        close(process->completion_fd);
        process->completion_fd=-1;
    }
    if (process->pty_master_fd >= 0) {
        close(process->pty_master_fd);
        process->pty_master_fd = -1;
    }
    if (process->stdin_writer_fd >= 0) {
        close(process->stdin_writer_fd);
        process->stdin_writer_fd = -1;
    }
    if (process->stdout_fd >= 0) {
        close(process->stdout_fd);
        process->stdout_fd = -1;
    }
    if (process->status_fd >= 0) {
        close(process->status_fd);
        process->status_fd = -1;
    }
}

static int ensure_descendant_reaping(char* error, size_t error_cap) {
#ifdef __linux__
    int enabled = 0;
    if (prctl(PR_GET_CHILD_SUBREAPER, &enabled) != 0 ||
        (!enabled && prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)) {
        set_error(error, error_cap, "cannot enable descendant reaping: %s",
                  strerror(errno));
        return -1;
    }
#else
    (void)error;
    (void)error_cap;
#endif
    return 0;
}

int jash_agent_process_start(const jash_execution_ir_t* ir,
                             const jash_agent_process_options_t* options,
                             jash_agent_process_t** out,
                             char* error, size_t error_cap) {
    if (out) *out = NULL;
    if (!out) {
        set_error(error, error_cap, "output process pointer is required");
        errno = EINVAL;
        return -1;
    }
    jash_execution_ir_error_t ir_error;
    if (jash_execution_ir_validate(ir, &ir_error) != 0) {
        set_error(error, error_cap, "%s", jash_execution_ir_error_string(ir_error));
        errno = EINVAL;
        return -1;
    }
    if (validate_options(options, error, error_cap) != 0) return -1;
    if (options->resources && options->resources->pids_max < ir->stage_count) {
        errno = EINVAL;
        set_error(error, error_cap, "pids_max must cover all pipeline stages");
        return -1;
    }
    if (ensure_descendant_reaping(error, error_cap) != 0) return -1;

    int stdout_fd = -1, stderr_fd = -1, stdin_fd = -1;
    int stdin_writer_fd = -1;
    int pty_master = -1, pty_slave = -1;
    int control[2] = {-1, -1};
    jash_agent_cgroup_t* cgroup = NULL;
    if (options->resources &&
        jash_agent_cgroup_create(options->cgroup_parent_fd, options->resources,
                                 &cgroup, error, error_cap) != 0) return -1;
    if (open_spools(options, &stdout_fd, &stderr_fd, error, error_cap) != 0)
        goto fail;
    if (options->pty) {
        if (open_pty(options->pty_rows, options->pty_cols,
                     &pty_master, &pty_slave, error, error_cap) != 0)
            goto fail;
    } else if (options->stdin_pipe) {
        int writable_stdin[2] = {-1, -1};
        if (make_pipe(writable_stdin) != 0 ||
            fd_nonblocking(writable_stdin[1]) != 0) {
            int saved = errno;
            if (writable_stdin[0] >= 0) close(writable_stdin[0]);
            if (writable_stdin[1] >= 0) close(writable_stdin[1]);
            errno = saved;
            set_error(error, error_cap, "cannot create writable stdin pipe: %s",
                      strerror(errno));
            goto fail;
        }
        stdin_fd = writable_stdin[0];
        stdin_writer_fd = writable_stdin[1];
    } else if (options->stdin_fd >= 0) {
        stdin_fd = fd_cloexec(dup(options->stdin_fd));
        if (stdin_fd < 0) {
            set_error(error, error_cap, "cannot duplicate stdin_fd: %s", strerror(errno));
            goto fail;
        }
    } else {
        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        stdin_fd = fd_cloexec(open("/dev/null", flags));
        if (stdin_fd < 0) {
            set_error(error, error_cap, "cannot open /dev/null: %s", strerror(errno));
            goto fail;
        }
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, control) != 0 ||
        fd_cloexec(control[0]) < 0 || fd_cloexec(control[1]) < 0) {
        set_error(error, error_cap, "cannot create launch control pipe: %s", strerror(errno));
        goto fail;
    }

    jash_agent_process_t* process = calloc(1, sizeof(*process));
    if (!process) {
        set_error(error, error_cap, "out of memory");
        goto fail;
    }
    process->pty_master_fd = -1;
    process->stdin_pipe = !!options->stdin_pipe;
    process->stdin_writer_fd = -1;
    process->stdout_fd = -1;
    process->status_fd = -1;
    process->completion_fd = -1;
    process->cancel_grace_ms = options->cancel_grace_ms;
    process->result.exit_code = -1;
    process->result.background = !!options->background;
    process->result.pty = !!options->pty;
    process->result.output_limit_bytes = options->output_limit_bytes;
    process->cgroup = cgroup;
    if (options->resources) {
        process->result.resources = *options->resources;
        process->result.resources_enforced = 1;
    }

    pid_t launcher = fork();
    if (launcher == 0)
        launcher_main(ir, options, stdout_fd, stderr_fd, stdin_fd,
                      stdin_writer_fd,
                      pty_master, pty_slave, control[0], control[1], cgroup);
    if (launcher < 0) {
        int saved = errno;
        free(process);
        errno = saved;
        set_error(error, error_cap, "cannot fork launcher: %s", strerror(errno));
        goto fail;
    }
    if (register_launcher(launcher) != 0) {
        int saved = errno;
        (void)kill(launcher, SIGKILL);
        while (waitpid(launcher, NULL, 0) < 0 && errno == EINTR) {}
        free(process);
        errno = saved;
        set_error(error, error_cap, "cannot track launcher: %s", strerror(errno));
        goto fail;
    }
    process->launcher_registered = 1;

    close(control[1]);
    control[1] = -1;
    close(stderr_fd);
    stderr_fd = -1;
    close(stdin_fd);
    stdin_fd = -1;
    close(pty_slave);
    pty_slave = -1;
    if (!options->pty) {
        close(stdout_fd);
        stdout_fd = -1;
    }

    launch_message_t message = {0};
    int launch_read = write_without_sigpipe(control[0], "R", 1) == 1
        ? read_exact(control[0], &message, sizeof message) : -1;
    if (launch_read == 0 && message.ok &&
        write_without_sigpipe(control[0], "G", 1) != 1) launch_read = -1;
    if (launch_read != 0 || !message.ok) {
        int saved = launch_read != 0 ? EIO : message.error_number;
        close(control[0]);
        control[0] = -1;
        while (waitpid(launcher, NULL, 0) < 0 && errno == EINTR) {}
        unregister_launcher(launcher);
        process->launcher_registered = 0;
        close(pty_master);
        if (stdin_writer_fd >= 0) close(stdin_writer_fd);
        if (options->pty) close(stdout_fd);
        free(process);
        if (cgroup) {
            (void)jash_agent_cgroup_kill(cgroup);
            if (launch_read != 0) {
                int forced = 0;
                (void)kill_and_reap_unregistered_children(&forced);
            }
            (void)jash_agent_cgroup_destroy(cgroup);
        }
        errno = saved ? saved : EIO;
        set_error(error, error_cap, "launcher preparation failed: %s", strerror(errno));
        return -1;
    }
    process->status_fd = control[0];
    control[0] = -1;
    process->result.launcher_pid = launcher;
    process->result.pgid = message.pgid;
    process->result.state = JASH_AGENT_PROCESS_RUNNING;
#if defined(__linux__) && defined(SYS_pidfd_open) && !defined(JASH_TEST_NO_PIDFD)
    process->completion_fd=(int)syscall(SYS_pidfd_open,launcher,0);
    if(process->completion_fd>=0) {
        if(fd_cloexec(process->completion_fd)<0) {
            close(process->completion_fd);process->completion_fd=-1;
        } else process->result.completion_event_kind=1;
    }
#endif
    if (options->pty) {
        process->pty_master_fd = pty_master;
        process->stdout_fd = stdout_fd;
    }
    process->stdin_writer_fd = stdin_writer_fd;
    stdin_writer_fd = -1;
    *out = process;
    if (!options->background && jash_agent_process_wait(process, -1) != 0) {
        set_error(error, error_cap, "foreground wait failed: %s", strerror(errno));
        jash_agent_process_destroy(process);
        *out = NULL;
        return -1;
    }
    return 0;

fail:
    {
        int saved = errno;
        if (control[0] >= 0) close(control[0]);
        if (control[1] >= 0) close(control[1]);
        if (stdout_fd >= 0) close(stdout_fd);
        if (stderr_fd >= 0) close(stderr_fd);
        if (stdin_fd >= 0) close(stdin_fd);
        if (stdin_writer_fd >= 0) close(stdin_writer_fd);
        if (pty_master >= 0) close(pty_master);
        if (pty_slave >= 0) close(pty_slave);
        if (cgroup) {
            (void)jash_agent_cgroup_kill(cgroup);
            (void)jash_agent_cgroup_destroy(cgroup);
        }
        errno = saved;
        return -1;
    }
}

#define JASH_AGENT_PROCESS_PTY_DRAIN_BUDGET (64u * 1024u)

static int drain_pty(jash_agent_process_t* process, int64_t deadline) {
    if (process->pty_master_fd < 0 || process->pty_eof) return 0;
#ifdef JASH_AGENT_PROCESS_TEST_DRAIN_DELAY_US
    if (deadline >= 0)
        usleep(JASH_AGENT_PROCESS_TEST_DRAIN_DELAY_US);
#endif
    if (deadline >= 0) {
        int64_t now = monotonic_ms();
        if (now < 0) return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return 1;
        }
    }
    char buffer[8192];
    size_t drained = 0;
    for (;;) {
        ssize_t count = read(process->pty_master_fd, buffer, sizeof buffer);
        if (count > 0) {
            size_t amount = (size_t)count;
            drained += amount;
            if (!process->result.output_limit_hit) {
                uint64_t remaining = process->result.output_limit_bytes -
                                     process->pty_output_bytes;
                size_t allowed = remaining < amount ? (size_t)remaining : amount;
                if (allowed && write_all(process->stdout_fd, buffer, allowed) != 0)
                    return -1;
                process->pty_output_bytes += allowed;
                if (allowed < amount) {
                    process->result.output_limit_hit = 1;
                    if (process->cgroup && jash_agent_cgroup_kill(process->cgroup) != 0)
                        return -1;
                    if (kill(-process->result.pgid, SIGXFSZ) != 0 &&
                        errno != ESRCH)
                        return -1;
                }
            }
            if (deadline >= 0) {
                int64_t now = monotonic_ms();
                if (now < 0) return -1;
                if (now >= deadline) {
                    errno = ETIMEDOUT;
                    return 1;
                }
            } else if (drained >= JASH_AGENT_PROCESS_PTY_DRAIN_BUDGET) {
                return 2;
            }
            continue;
        }
        if (count == 0 || (count < 0 && errno == EIO)) {
            process->pty_eof = 1;
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

static int process_group_absent(pid_t pgid) {
    if (pgid <= 0) return 1;
    if (kill(-pgid, 0) == 0 || errno == EPERM) return 0;
    return errno == ESRCH ? 1 : -1;
}

static int reap_adopted_group(pid_t pgid) {
    if (pgid <= 0) { errno = EINVAL; return -1; }
    for (;;) {
        pid_t waited = waitpid(-pgid, NULL, WNOHANG);
        if (waited > 0) continue;
        if (waited < 0 && errno == EINTR) continue;
        return waited < 0 && errno != ECHILD ? -1 : 0;
    }
}

static int force_group_quiescence(jash_agent_process_t* process,
                                  int timeout_ms) {
    if (reap_adopted_group(process->result.pgid)) return -1;
    int absent = process_group_absent(process->result.pgid);
    if (absent < 0) return -1;
    if (absent) {
        process->pgid_quiescent = 1;
        return 0;
    }
    process->result.quiescence_forced = 1;
    process->result.kill_sent = 1;
    transition(process, JASH_AGENT_PROCESS_KILL_SENT);
    if (kill(-process->result.pgid, SIGKILL) != 0 && errno != ESRCH)
        return -1;

    int64_t deadline;
    if (cleanup_deadline(timeout_ms, &deadline)) return -1;
    for (;;) {
        if (reap_adopted_group(process->result.pgid)) return -1;
        absent = process_group_absent(process->result.pgid);
        if (absent < 0) return -1;
        if (absent) {
            process->pgid_quiescent = 1;
            return 0;
        }
        if (cleanup_pause(deadline)) return -1;
    }
}

static void apply_terminal_status(jash_agent_process_t* process, int status) {
    process->result.terminal_status_valid = 1;
    if (WIFEXITED(status)) {
        process->result.exit_code = WEXITSTATUS(status);
        process->result.term_signal = 0;
    } else if (WIFSIGNALED(status)) {
        process->result.term_signal = WTERMSIG(status);
        process->result.exit_code = 128 + process->result.term_signal;
    } else {
        process->result.exit_code = 125;
    }
}

static void record_launcher_reaped(jash_agent_process_t* process, int status) {
    if(!process->launcher_reaped)process->result.completion_notifications++;
    process->launcher_reaped = 1;
    process->launcher_status = status;
    if (process->launcher_registered) {
        unregister_launcher(process->result.launcher_pid);
        process->launcher_registered = 0;
    }
}

static int finish_process(jash_agent_process_t* process,
                          int quiescence_timeout_ms) {
    if (!process->completion_prepared) {
        terminal_message_t terminal;
        int terminal_valid = process->status_fd >= 0 &&
            read_exact(process->status_fd, &terminal, sizeof terminal) == 0 &&
            terminal.magic == TERMINAL_MESSAGE_MAGIC &&
            terminal.version == 1 && terminal.record_size == sizeof(terminal) &&
            terminal.launcher_pid == process->result.launcher_pid &&
            terminal.descendants_quiescent == 1 &&
            (!process->cgroup || terminal.resource_empty == 1);
        if (process->status_fd >= 0) {
            close(process->status_fd);
            process->status_fd = -1;
        }
        if (terminal_valid) {
            process->result.completion_version=terminal.version;
            apply_terminal_status(process, terminal.wait_status);
            if (terminal.output_limit_hit) process->result.output_limit_hit = 1;
            process->result.descendants_quiescent = 1;
            process->result.descendants_forced = terminal.descendants_forced;
            process->result.resource_timed_out = terminal.resource_timed_out;
            process->result.resource_memory_events = terminal.resource_memory_events;
            process->result.resource_pids_events = terminal.resource_pids_events;
            process->result.resource_empty = terminal.resource_empty;
            process->result.resource_violation = terminal.resource_timed_out ||
                terminal.resource_memory_events || terminal.resource_pids_events;
        } else {
            process->result.launcher_unexpected = 1;
            process->result.indeterminate = 1;
            process->result.exit_code = 125;
            process->result.term_signal = 0;
            if (process->cgroup) (void)jash_agent_cgroup_kill(process->cgroup);
            if (WIFSIGNALED(process->launcher_status))
                process->result.launcher_term_signal =
                    WTERMSIG(process->launcher_status);
        }
        process->completion_prepared = 1;
    }
    if (force_group_quiescence(process, quiescence_timeout_ms) != 0) {
        process->result.cleanup_error_number = errno ? errno : EIO;
        process->result.indeterminate = 1;
        process->result.quiescent = 0;
        transition(process, JASH_AGENT_PROCESS_INDETERMINATE);
        return -1;
    }
    while (drain_pty(process, -1) == 2) {}
    if (process->result.indeterminate) {
        int descendants_forced = 0;
        if (kill_and_reap_unregistered_children(&descendants_forced) == 0)
            process->result.descendants_quiescent = 1;
        else process->result.cleanup_error_number = errno ? errno : EIO;
        if (descendants_forced) process->result.descendants_forced = 1;
        if (process->cgroup) {
            jash_agent_cgroup_snapshot_t snapshot;
            if (jash_agent_cgroup_snapshot(process->cgroup, &snapshot) == 0) {
                process->result.resource_empty = snapshot.empty;
                if (snapshot.memory_events > process->result.resource_memory_events)
                    process->result.resource_memory_events = snapshot.memory_events;
                if (snapshot.pids_events > process->result.resource_pids_events)
                    process->result.resource_pids_events = snapshot.pids_events;
                if (snapshot.memory_events || snapshot.pids_events)
                    process->result.resource_violation = 1;
            }
        }
        process->result.quiescent = 0;
        transition(process, JASH_AGENT_PROCESS_INDETERMINATE);
        close_process_fds(process);
        errno = EUCLEAN;
        return -1;
    }
    if (process->cgroup) {
        jash_agent_cgroup_snapshot_t snapshot;
        if (jash_agent_cgroup_snapshot(process->cgroup, &snapshot) != 0 || !snapshot.empty) {
            (void)jash_agent_cgroup_kill(process->cgroup);
            process->result.resource_empty = 0;
            process->result.indeterminate = 1;
            transition(process, JASH_AGENT_PROCESS_INDETERMINATE);
            errno = EUCLEAN;
            return -1;
        }
        process->result.resource_empty = 1;
        if (snapshot.memory_events > process->result.resource_memory_events)
            process->result.resource_memory_events = snapshot.memory_events;
        if (snapshot.pids_events > process->result.resource_pids_events)
            process->result.resource_pids_events = snapshot.pids_events;
        process->result.resource_violation = process->result.resource_timed_out ||
            process->result.resource_memory_events || process->result.resource_pids_events;
        int removed = jash_agent_cgroup_destroy(process->cgroup);
        process->cgroup = NULL;
        if (removed != 0) {
            process->result.indeterminate = 1;
            transition(process, JASH_AGENT_PROCESS_INDETERMINATE);
            errno = EUCLEAN;
            return -1;
        }
    }
    if (process->result.pty &&
        process->pty_output_bytes >= process->result.output_limit_bytes)
        process->result.output_limit_hit = 1;
    process->result.quiescent = process->pgid_quiescent &&
                                process->result.descendants_quiescent;
    if (!process->result.quiescent) {
        process->result.indeterminate = 1;
        transition(process, JASH_AGENT_PROCESS_INDETERMINATE);
        close_process_fds(process);
        errno = EUCLEAN;
        return -1;
    }
    transition(process, JASH_AGENT_PROCESS_DONE);
    close_process_fds(process);
    return 0;
}

int jash_agent_process_poll(jash_agent_process_t* process, int timeout_ms) {
    if (!process || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }
    if (process->result.state == JASH_AGENT_PROCESS_DONE) return 1;
    if (process->result.state == JASH_AGENT_PROCESS_INDETERMINATE) {
        errno = EUCLEAN;
        return -1;
    }
    int64_t deadline = timeout_ms < 0 ? -1 : monotonic_ms() + timeout_ms;
    for (;;) {
        int drained = drain_pty(process, -1);
        if (drained < 0) return -1;
        if (process->launcher_reaped) {
            if (finish_process(process, 5000) != 0) return -1;
            return 1;
        }
        int status = 0;
        pid_t waited = waitpid(process->result.launcher_pid, &status, WNOHANG);
        if (waited == process->result.launcher_pid) {
            record_launcher_reaped(process, status);
            if (finish_process(process, 5000) != 0) return -1;
            return 1;
        }
        if (waited < 0 && errno != EINTR) return -1;
        if (timeout_ms == 0) return 0;
        int wait_ms = process->completion_fd>=0 ? -1 : 10;
        if (deadline >= 0) {
            int64_t remaining = deadline - monotonic_ms();
            if (remaining <= 0) return 0;
            if (wait_ms<0 || remaining < wait_ms)
                wait_ms = remaining>INT_MAX?INT_MAX:(int)remaining;
        }
        struct pollfd pfds[2];
        nfds_t count=0;
        if(process->completion_fd>=0)pfds[count++]=(struct pollfd){
            .fd=process->completion_fd,.events=POLLIN|POLLHUP};
        if(process->pty_master_fd>=0&&!process->pty_eof)pfds[count++]=(struct pollfd){
            .fd=process->pty_master_fd,.events=POLLIN|POLLHUP};
        int rc = poll(pfds,count,wait_ms);
        if (rc < 0 && errno != EINTR) return -1;
        if(rc>0) {
            process->result.wait_wakeups++;
            for(nfds_t i=0;i<count;i++)if(pfds[i].revents&POLLNVAL) {
                errno=EBADF;return -1;
            }
        }
    }
}

int jash_agent_process_wait(jash_agent_process_t* process, int timeout_ms) {
    int result = jash_agent_process_poll(process, timeout_ms);
    if (result < 0) return -1;
    return result == 1 ? 0 : 1;
}

static int reap_launcher_without_io(jash_agent_process_t* process,
                                    int timeout_ms) {
    int64_t deadline;
    if (cleanup_deadline(timeout_ms, &deadline)) return -1;
    for (;;) {
        if (process->launcher_reaped)
            return finish_process(process, timeout_ms);
        int status = 0;
        pid_t waited = waitpid(process->result.launcher_pid, &status, WNOHANG);
        if (waited == process->result.launcher_pid) {
            record_launcher_reaped(process, status);
            return finish_process(process, timeout_ms);
        }
        if (waited < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (cleanup_pause(deadline)) return errno == ETIMEDOUT ? 1 : -1;
    }
}

int jash_agent_process_cancel(jash_agent_process_t* process,
                              unsigned grace_ms) {
    if (!process || grace_ms > JASH_AGENT_PROCESS_MAX_GRACE_MS) {
        errno = EINVAL;
        return -1;
    }
    if (process->result.state == JASH_AGENT_PROCESS_DONE) return 0;
    if (process->result.state == JASH_AGENT_PROCESS_INDETERMINATE) {
        errno = EUCLEAN;
        return -1;
    }
    if (process->stdin_pipe) (void)jash_agent_process_close_stdin(process);
    process->result.cancel_requested = 1;
    transition(process, JASH_AGENT_PROCESS_TERM_SENT);
    if (kill(-process->result.pgid, SIGTERM) != 0 && errno != ESRCH) return -1;
    if (process->cgroup) {
        if (jash_agent_cgroup_kill(process->cgroup) != 0) return -1;
        process->result.kill_sent = 1;
        transition(process, JASH_AGENT_PROCESS_KILL_SENT);
    }
    int waited = jash_agent_process_wait(process, (int)grace_ms);
    if (waited == 0) return 0;
    if (waited < 0 && process->result.state == JASH_AGENT_PROCESS_INDETERMINATE) return -1;

    process->result.kill_sent = 1;
    process->result.quiescence_forced = 1;
    transition(process, JASH_AGENT_PROCESS_KILL_SENT);
    if (kill(-process->result.pgid, SIGKILL) != 0 && errno != ESRCH) return -1;
    waited = jash_agent_process_wait(process, 5000);
    if (waited == 0) return 0;
    if (waited < 0 && process->result.state == JASH_AGENT_PROCESS_INDETERMINATE) return -1;
    waited = reap_launcher_without_io(process, 5000);
    if (waited == 0) return 0;

    /* A broken launcher is killed before completion; finish_process then
     * force-kills and confirms the entire worker group is absent. */
    if (!process->launcher_reaped && kill(process->result.launcher_pid, SIGKILL) && errno != ESRCH)
        return -1;
    waited = reap_launcher_without_io(process, -1);
    if (waited > 0) { errno = ETIMEDOUT; return -1; }
    return waited;
}

int jash_agent_process_resize_pty(jash_agent_process_t* process,
                                  unsigned rows, unsigned cols) {
    if (!process || !process->result.pty || process->pty_master_fd < 0 ||
        process->result.state >= JASH_AGENT_PROCESS_INDETERMINATE ||
        rows == 0 || rows > 65535u || cols == 0 || cols > 65535u) {
        errno = EINVAL;
        return -1;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    return ioctl(process->pty_master_fd, TIOCSWINSZ, &ws);
}

int jash_agent_process_write_pty(jash_agent_process_t* process,
                                 const void* data, size_t length,
                                 int timeout_ms, size_t* accepted_bytes) {
    if (accepted_bytes) *accepted_bytes = 0;
    if (!process || (!data && length) || !accepted_bytes ||
        timeout_ms < 0 || timeout_ms > 30000) {
        errno = EINVAL;
        return -1;
    }
    if (!process->result.pty) {
        errno = ENOTTY;
        return -1;
    }
    if (process->result.state >= JASH_AGENT_PROCESS_INDETERMINATE ||
        process->pty_master_fd < 0 || process->pty_eof) {
        errno = EPIPE;
        return -1;
    }

    const unsigned char* bytes = data;
    size_t offset = 0;
    int64_t deadline = monotonic_ms();
    if (deadline < 0) return -1;
    deadline += timeout_ms;
    while (offset < length) {
        ssize_t written = write(process->pty_master_fd, bytes + offset,
                                length - offset);
        if (written > 0) {
            offset += (size_t)written;
            *accepted_bytes = offset;
            int drained = drain_pty(process, deadline);
            if (drained != 0) return drained;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;

        int drained = drain_pty(process, deadline);
        if (drained != 0) return drained;
        struct pollfd pfd = {
            .fd = process->pty_master_fd,
            .events = POLLIN | POLLOUT | POLLHUP,
        };
        int ready;
        for (;;) {
            int64_t now = monotonic_ms();
            if (now < 0) return -1;
            int64_t remaining = deadline - now;
            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return 1;
            }
            ready = poll(&pfd, 1, (int)remaining);
            if (ready < 0 && errno == EINTR) continue;
            break;
        }
        if (ready < 0) return -1;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return 1;
        }
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            drained = drain_pty(process, deadline);
            if (drained != 0) return drained;
            if (process->pty_eof || (pfd.revents & (POLLHUP | POLLERR))) {
                errno = EPIPE;
                return -1;
            }
        }
    }
    return 0;
}

/* sigprocmask is unspecified in a multi-threaded POSIX process. Darwin's
 * libSystem supplies pthread_sigmask without an additional link dependency. */
static int stdin_signal_mask(int how, const sigset_t* set, sigset_t* old) {
#ifdef __APPLE__
    int error = pthread_sigmask(how, set, old);
    if (error) { errno = error; return -1; }
    return 0;
#else
    return sigprocmask(how, set, old);
#endif
}

static ssize_t write_without_sigpipe(int fd, const void* data, size_t length) {
    sigset_t blocked, prior, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (stdin_signal_mask(SIG_BLOCK, &blocked, &prior) != 0) return -1;
    if (sigpending(&pending) != 0) {
        int error = errno;
        if (stdin_signal_mask(SIG_SETMASK, &prior, NULL) != 0) return -1;
        errno = error;
        return -1;
    }
    int was_pending = sigismember(&pending, SIGPIPE);
    ssize_t written;
    do { written = write(fd, data, length); } while (written < 0 && errno == EINTR);
    int saved = errno;
    if (written < 0 && saved == EPIPE && !was_pending) {
#if defined(__APPLE__) || defined(JASH_AGENT_PROCESS_TEST_PORTABLE_SIGWAIT)
        /* Darwin has sigwait, not sigtimedwait. Check pending first: an
         * ignored SIGPIPE can yield EPIPE without queuing any signal. Never
         * consume a signal that was already pending before our write. */
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
            int signo;
            (void)sigwait(&blocked, &signo);
        }
#else
        struct timespec no_wait = {0, 0};
        while (sigtimedwait(&blocked, NULL, &no_wait) < 0 && errno == EINTR) {}
#endif
    }
    if (stdin_signal_mask(SIG_SETMASK, &prior, NULL) != 0) return -1;
    errno = saved;
    return written;
}

int jash_agent_process_write_stdin(jash_agent_process_t* process,
                                   const void* data, size_t length,
                                   int timeout_ms, size_t* accepted_bytes) {
    if (accepted_bytes) *accepted_bytes = 0;
    if (!process || (!data && length) || !accepted_bytes ||
        timeout_ms < 0 || timeout_ms > 30000) {
        errno = EINVAL;
        return -1;
    }
    if (!process->stdin_pipe) {
        errno = ENOTTY;
        return -1;
    }
    if (process->stdin_writer_fd < 0) {
        errno = EPIPE;
        return -1;
    }
    if (process->result.state >= JASH_AGENT_PROCESS_INDETERMINATE) {
        errno = EPIPE;
        return -1;
    }

    const unsigned char* bytes = data;
    size_t offset = 0;
    int64_t deadline = monotonic_ms();
    if (deadline < 0) return -1;
    deadline += timeout_ms;
    while (offset < length) {
        ssize_t written = write_without_sigpipe(process->stdin_writer_fd,
                                                bytes + offset,
                                                length - offset);
        if (written > 0) {
            offset += (size_t)written;
            *accepted_bytes = offset;
            continue;
        }
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;

        struct pollfd pfd = {
            .fd = process->stdin_writer_fd,
            .events = POLLOUT | POLLHUP,
        };
        int ready;
        for (;;) {
            int64_t now = monotonic_ms();
            if (now < 0) return -1;
            int64_t remaining = deadline - now;
            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return 1;
            }
            ready = poll(&pfd, 1, (int)remaining);
            if (ready < 0 && errno == EINTR) continue;
            break;
        }
        if (ready < 0) return -1;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return 1;
        }
        if (pfd.revents & (POLLHUP | POLLERR)) {
            errno = EPIPE;
            return -1;
        }
    }
    return 0;
}

int jash_agent_process_close_stdin(jash_agent_process_t* process) {
    if (!process) {
        errno = EINVAL;
        return -1;
    }
    if (!process->stdin_pipe) {
        errno = ENOTTY;
        return -1;
    }
    if (process->stdin_writer_fd >= 0) {
        close(process->stdin_writer_fd);
        process->stdin_writer_fd = -1;
    }
    return 0;
}

const jash_agent_process_result_t*
jash_agent_process_get_result(const jash_agent_process_t* process) {
    return process ? &process->result : NULL;
}

int jash_agent_process_destroy_checked(jash_agent_process_t* process,
                                       jash_agent_process_result_t* final_result) {
    if (!process) {
        if (final_result) memset(final_result, 0, sizeof(*final_result));
        return 0;
    }
    int error = process->result.cleanup_error_number;
    if (process->cgroup && jash_agent_cgroup_kill(process->cgroup) && !error)
        error = errno ? errno : EIO;
    if (process->result.state != JASH_AGENT_PROCESS_DONE &&
        process->result.state != JASH_AGENT_PROCESS_INDETERMINATE) {
        if (jash_agent_process_cancel(process, process->cancel_grace_ms) && !error)
            error = errno ? errno : EIO;
    }
    if (process->result.state != JASH_AGENT_PROCESS_DONE &&
        process->result.state != JASH_AGENT_PROCESS_INDETERMINATE) {
        process->result.cancel_requested = 1;
        process->result.kill_sent = 1;
        process->result.quiescence_forced = 1;
        transition(process, JASH_AGENT_PROCESS_KILL_SENT);
        (void)kill(-process->result.pgid, SIGKILL);
        if (!process->launcher_reaped) {
            (void)kill(process->result.launcher_pid, SIGKILL);
            int status = 0;
            pid_t waited = 0; int64_t deadline;
            if (cleanup_deadline(-1, &deadline)) error = errno ? errno : EIO;
            else for (;;) {
                waited = waitpid(process->result.launcher_pid, &status, WNOHANG);
                if (waited > 0 || (waited < 0 && errno != EINTR)) break;
                if (cleanup_pause(deadline)) { if (!error) error = errno; break; }
            }
            if (waited == process->result.launcher_pid) {
                record_launcher_reaped(process, status);
            }
        }
        if (process->launcher_reaped && finish_process(process, -1) && !error)
            error = errno ? errno : EIO;
    }
    if (!process->pgid_quiescent && force_group_quiescence(process, -1) && !error)
        error = errno ? errno : EIO;
    if (process->result.indeterminate ||
        !process->result.descendants_quiescent) {
        int descendants_forced = 0;
        if (kill_and_reap_unregistered_children(&descendants_forced)) {
            if (!error) error = errno ? errno : EIO;
        } else process->result.descendants_quiescent = 1;
        if (descendants_forced) process->result.descendants_forced = 1;
    }
    if (process->launcher_registered)
        unregister_launcher(process->result.launcher_pid);
    close_process_fds(process);
    if (process->cgroup && jash_agent_cgroup_destroy(process->cgroup) && !error)
        error = errno ? errno : EIO;
    if (!error && (!process->launcher_reaped || !process->pgid_quiescent ||
                   !process->result.descendants_quiescent || process->result.indeterminate))
        error = EUCLEAN;
    if (error) {
        process->result.cleanup_error_number = error;
        process->result.indeterminate = 1;
        process->result.quiescent = 0;
        process->result.state = JASH_AGENT_PROCESS_INDETERMINATE;
    }
    if (final_result) *final_result = process->result;
    free(process);
    if (error) { errno = error; return -1; }
    return 0;
}

void jash_agent_process_destroy(jash_agent_process_t* process) {
    (void)jash_agent_process_destroy_checked(process, NULL);
}
