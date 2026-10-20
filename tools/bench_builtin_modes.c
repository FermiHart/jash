/* Linux measurement helper. Fork after exec has replaced the Python runner's
 * address space, so wait4's peak RSS belongs to the native command, not Python.
 * Child stdout/stderr remain attached to the runner's capture pipes. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) return 125;
    char *end = NULL;
    errno = 0;
    long report_fd = strtol(argv[1], &end, 10);
    if (errno || !end || *end || report_fd < 3 || report_fd > INT_MAX) return 125;
    struct timespec before, after;
    if (clock_gettime(CLOCK_MONOTONIC, &before)) return 125;
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) return 125;
    if (!child) {
        close((int)report_fd);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
        execv(argv[2], argv + 2);
        _exit(127);
    }
    struct rusage usage = {0};
    int status = 0;
    pid_t waited;
    do { waited = wait4(child, &status, 0, &usage); } while (waited < 0 && errno == EINTR);
    if (waited != child || clock_gettime(CLOCK_MONOTONIC, &after)) return 125;
    int64_t elapsed = (int64_t)(after.tv_sec - before.tv_sec) * INT64_C(1000000000)
                    + after.tv_nsec - before.tv_nsec;
    char report[512];
    int size = snprintf(report, sizeof report,
        "{\"elapsed_ns\":%lld,\"peak_rss_kib\":%ld,\"user_seconds\":%.6f,\"system_seconds\":%.6f}\n",
        (long long)elapsed, usage.ru_maxrss,
        (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1000000.0,
        (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1000000.0);
    if (size < 0 || (size_t)size >= sizeof report) return 125;
    size_t offset = 0;
    while (offset < (size_t)size) {
        ssize_t wrote = write((int)report_fd, report + offset, (size_t)size - offset);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return 125;
        offset += (size_t)wrote;
    }
    if (close((int)report_fd)) return 125;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}
