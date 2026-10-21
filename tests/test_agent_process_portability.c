// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Exercise private portability primitives without requiring the Linux-only
 * containment launcher. Every case is isolated and bounded against hangs. */
#define JASH_AGENT_PROCESS_TEST_PORTABLE_SIGWAIT 1
#include "../c/core/agent_process.c"
#include "../c/core/mesh_store.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #x, errno); _exit(1); } } while (0)
static volatile sig_atomic_t caught;
static void pipe_handler(int signo) { (void)signo; caught++; }

static void signal_case(int kind) {
    sigset_t empty, original, pending;
    sigemptyset(&empty);
    REQUIRE(stdin_signal_mask(SIG_SETMASK, &empty, NULL) == 0);
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = kind == 1 ? SIG_IGN : kind == 3 ? pipe_handler : SIG_DFL;
    REQUIRE(sigaction(SIGPIPE, &action, NULL) == 0);
    sigaddset(&empty, SIGUSR1);
    if (kind == 2) sigaddset(&empty, SIGPIPE);
    REQUIRE(stdin_signal_mask(SIG_BLOCK, &empty, NULL) == 0);
    REQUIRE(stdin_signal_mask(SIG_BLOCK, NULL, &original) == 0);
    if (kind == 2) REQUIRE(raise(SIGPIPE) == 0);
    int fds[2]; REQUIRE(pipe(fds) == 0);
    if (kind != 4) REQUIRE(close(fds[0]) == 0);
    errno = 0;
    ssize_t n = write_without_sigpipe(fds[1], "x", 1);
    if (kind == 4) {
        REQUIRE(n == 1);
        char c; REQUIRE(read(fds[0], &c, 1) == 1 && c == 'x');
        REQUIRE(close(fds[0]) == 0);
    } else REQUIRE(n == -1 && errno == EPIPE);
    sigset_t now;
    REQUIRE(stdin_signal_mask(SIG_BLOCK, NULL, &now) == 0);
    REQUIRE(sigismember(&now, SIGPIPE) == sigismember(&original, SIGPIPE));
    REQUIRE(sigismember(&now, SIGUSR1) == 1);
    REQUIRE(sigpending(&pending) == 0);
    REQUIRE(sigismember(&pending, SIGPIPE) == (kind == 2));
    struct sigaction after;
    REQUIRE(sigaction(SIGPIPE, NULL, &after) == 0);
    REQUIRE(after.sa_handler == action.sa_handler && caught == 0);
    REQUIRE(close(fds[1]) == 0);
}

static void fd_case(void) {
    int sentinel = open("/dev/null", O_RDONLY);
    REQUIRE(sentinel >= 3);
    int high = fcntl(sentinel, F_DUPFD, 100);
    REQUIRE(high >= 100);
    /* Directly cover the fallback selected by Darwin, even on Linux. */
    REQUIRE(close_worker_fds_loop() == 0);
    REQUIRE(fcntl(sentinel, F_GETFD) == -1 && errno == EBADF);
    REQUIRE(fcntl(high, F_GETFD) == -1 && errno == EBADF);
    REQUIRE(fcntl(STDERR_FILENO, F_GETFD) >= 0);
}

static void stat_case(void) {
    struct stat a = {0}, b = {0};
    REQUIRE(jash_mesh_same_file(&a, &b));
#ifdef __APPLE__
    b.st_mtimespec.tv_nsec = 1;
#else
    b.st_mtim.tv_nsec = 1;
#endif
    REQUIRE(!jash_mesh_same_file(&a, &b)); b = a;
#ifdef __APPLE__
    b.st_ctimespec.tv_nsec = 1;
#else
    b.st_ctim.tv_nsec = 1;
#endif
    REQUIRE(!jash_mesh_same_file(&a, &b)); b = a;
    b.st_ino = 1; REQUIRE(!jash_mesh_same_file(&a, &b));
}

int main(void) {
    for (int kind = 0; kind < 7; kind++) {
        pid_t pid = fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            alarm(10);
            if (kind < 5) signal_case(kind);
            else if (kind == 5) fd_case();
            else stat_case();
            _exit(0);
        }
        int status;
        REQUIRE(waitpid(pid, &status, 0) == pid);
        REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("portability: 7 bounded signal/fd/stat cases passed");
    return 0;
}
