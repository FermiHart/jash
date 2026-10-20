// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <core/agent_process.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Link-time faults only: production code has no environment-controlled hooks. */
static int fault;
static int attachments;
static pid_t test_parent;
static int deadline_test;
static int deadline_waited;
static int deadline_polled;
static int deadline_elapsed;
static char replaced_group[96];
static int replaced_original = -1;
ssize_t __real_write(int fd, const void* bytes, size_t count);
ssize_t __real_read(int fd, void* bytes, size_t count);
int __real_clock_gettime(clockid_t clock, struct timespec* time);
pid_t __real_waitpid(pid_t pid, int* status, int flags);
int __real_poll(struct pollfd* fds, nfds_t count, int timeout);
int __real_openat(int dir, const char* path, int flags, ...);

int __wrap_clock_gettime(clockid_t clock, struct timespec* time) {
    if (deadline_test && getpid() != test_parent && clock == CLOCK_MONOTONIC) {
        time->tv_sec = 100;
        time->tv_nsec = deadline_elapsed * 1000000L;
        return 0;
    }
    return __real_clock_gettime(clock, time);
}

pid_t __wrap_waitpid(pid_t pid, int* status, int flags) {
    if (deadline_test && getpid() != test_parent &&
        pid == -1 && (flags & WNOHANG) && !deadline_waited++)
        return 0; /* Force the launcher into its final polling interval. */
    return __real_waitpid(pid, status, flags);
}

int __wrap_poll(struct pollfd* fds, nfds_t count, int timeout) {
    if (deadline_test && getpid() != test_parent && !count && !deadline_polled++) {
        if (fault == 10) assert(timeout > 0 && timeout <= 2);
        siginfo_t info;
        memset(&info, 0, sizeof info);
        /* The worker exits while the clock is still before its 2 ms deadline.
         * The launcher wakes late; leave its real exit status unreaped. */
        assert(waitid(P_ALL, 0, &info, WEXITED | WNOWAIT) == 0 && info.si_pid > 0);
        deadline_elapsed = 3;
        return 0;
    }
    return __real_poll(fds, count, timeout);
}

int __wrap_openat(int dir, const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    if (fault == 11 && (flags & O_DIRECTORY) && !strncmp(path, "jash-", 5)) {
        assert(strlen(path) < sizeof replaced_group);
        strcpy(replaced_group, path);
        replaced_original = __real_openat(dir, path, flags);
        assert(replaced_original >= 0);
        assert(unlinkat(dir, path, AT_REMOVEDIR) == 0);
        assert(mkdirat(dir, path, 0700) == 0);
        errno = EMFILE;
        return -1;
    }
    return __real_openat(dir, path, flags, mode);
}

static int file_is(int fd, const char* suffix) {
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n < 0) return 0;
    path[n] = 0;
    size_t length = strlen(suffix);
    return (size_t)n >= length && !strcmp(path + n - length, suffix);
}

ssize_t __wrap_write(int fd, const void* bytes, size_t count) {
    if (fault && file_is(fd, "/cgroup.procs") && ++attachments == 2) {
        if (fault == 2) { raise(SIGKILL); _exit(125); }
        if (fault == 1) { errno = EACCES; return -1; }
    }
    return __real_write(fd, bytes, count);
}

ssize_t __wrap_read(int fd, void* bytes, size_t count) {
    ssize_t n = __real_read(fd, bytes, count);
    struct stat metadata;
    if (fault >= 6 && fault <= 8 && count == 1 && n == 1 &&
        fstat(fd, &metadata) == 0 && S_ISFIFO(metadata.st_mode)) {
        if (fault == 6) return 0;
        if (fault == 7) { errno = EIO; return -1; }
        ((char*)bytes)[0] = 'X';
    }
    if (fault == 3 && n > 0 && file_is(fd, "/cpu.max")) ((char*)bytes)[0] = '9';
    if (fault == 4 && file_is(fd, "/memory.events")) { memset(bytes, 'x', count); return (ssize_t)count; }
    if (fault == 5 && attachments == 2 && n > 0 && file_is(fd, "/pids.current")) {
        assert(count >= 3);
        memcpy(bytes, "99\n", 3);
        return 3;
    }
    return n;
}

static void sleep_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

static int64_t milliseconds(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void* thread_idle(void* unused) { (void)unused; sleep_ms(200); return NULL; }

static int payload(const char* mode) {
    if (!strcmp(mode, "--zero")) return 0;
    if (!strcmp(mode, "--pids") || !strcmp(mode, "--pids-wait")) {
        pid_t child = fork(); /* Exactly one controlled admission attempt. */
        if (child < 0) {
            if (errno != EAGAIN) return 41;
            if (!strcmp(mode, "--pids-wait")) {
                int fd = open("denied", O_CREAT | O_WRONLY, 0600);
                assert(fd >= 0);
                close(fd);
                sleep_ms(500);
            }
            return 0;
        }
        if (!child) _exit(0);
        waitpid(child, NULL, 0);
        return 42;
    }
    if (!strcmp(mode, "--threads")) {
        pthread_t thread;
        int rc = pthread_create(&thread, NULL, thread_idle, NULL);
        if (rc == EAGAIN) return 0;
        if (!rc) pthread_join(thread, NULL);
        return 42;
    }
    if (!strcmp(mode, "--memory")) {
        size_t size = 12u << 20; /* Two workers total <=24 MiB, never host pressure. */
        volatile char* memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return 43;
        for (size_t i = 0; i < size; i += 4096) memory[i] = 1;
        sleep_ms(500);
        munmap((void*)memory, size);
        return 0;
    }
    if (!strcmp(mode, "--late-marker")) {
        sleep_ms(500);
        int fd = open("marker", O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) close(fd);
        return 0;
    }
    if (!strcmp(mode, "--detach")) {
        pid_t child = fork();
        if (child < 0) return 44;
        if (!child) {
            assert(setsid() >= 0);
            FILE* f = fopen("detached", "w");
            assert(f);
            fprintf(f, "%ld\n", (long)getpid());
            fclose(f);
            sleep_ms(1000);
            int fd = open("marker", O_CREAT | O_WRONLY, 0600);
            if (fd >= 0) close(fd);
            _exit(0);
        }
        for (int i = 0; i < 100 && access("detached", F_OK); i++) sleep_ms(1);
        return 23;
    }
    if (!strcmp(mode, "--cpu")) {
        struct timespec t;
        assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0);
        int64_t start = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
        do {
            for (volatile unsigned i = 0; i < 10000; i++) {}
            assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0);
        } while ((int64_t)t.tv_sec * 1000000000 + t.tv_nsec - start < 30000000);
        return 0;
    }
    if (!strcmp(mode, "--signal")) { raise(SIGKILL); return 45; }
    if (!strcmp(mode, "--output")) {
        char bytes[4096];
        memset(bytes, 'x', sizeof bytes);
        for (unsigned i = 0; i < 8; i++) if (write(1, bytes, sizeof bytes) < 0) break;
        return 0;
    }
    return 46;
}

static jash_agent_process_t* start(jash_agent_process_options_t* options,
                                    const char* binary, const char* mode, size_t stages) {
    jash_execution_ir_t ir;
    jash_execution_ir_init(&ir);
    ir.stage_count = stages;
    for (size_t i = 0; i < stages; i++) {
        ir.stages[i].argc = 2;
        ir.stages[i].argv[0] = binary;
        ir.stages[i].argv[1] = mode;
    }
    jash_agent_process_t* process = NULL;
    char error[256];
    int rc = jash_agent_process_start(&ir, options, &process, error, sizeof error);
    if (rc) fprintf(stderr, "start: %s\n", error);
    assert(rc == 0 && process);
    return process;
}

static const jash_agent_process_result_t* finish(jash_agent_process_t* process) {
    assert(jash_agent_process_wait(process, 5000) == 0);
    const jash_agent_process_result_t* r = jash_agent_process_get_result(process);
    assert(r->resources_enforced && r->resource_empty && r->quiescent);
    assert(r->terminal_status_valid && !r->indeterminate);
    return r;
}

static int child_group(int root) {
    DIR* d = fdopendir(dup(root));
    assert(d);
    rewinddir(d);
    int child = -1;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "jash-", 5)) continue;
        assert(child == -1);
        child = openat(root, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        assert(child >= 0);
    }
    closedir(d);
    return child;
}

static void readback(int dir, const char* name, const char* expected) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    char text[256];
    ssize_t n = read(fd, text, sizeof text - 1);
    assert(n >= 0);
    text[n] = 0;
    assert(!strcmp(text, expected));
    close(fd);
}

static void deadline_edge(jash_agent_process_options_t* options, const char* binary) {
    jash_agent_limits_t limits = *options->resources;
    limits.wall_timeout_ms = 2;
    jash_agent_process_options_t bounded = *options;
    bounded.resources = &limits;
    test_parent = getpid();
    deadline_test = 1;
    const int faults[] = {9, 10, 6, 7, 8, 12};
    for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) {
        fault = faults[i];
        deadline_waited = deadline_polled = deadline_elapsed = 0;
        jash_agent_process_t* process = start(&bounded, binary,
                                             fault == 12 ? "--late-marker" : "--zero", 1);
        if (fault == 12) assert(jash_agent_process_cancel(process, 0) == 0);
        const jash_agent_process_result_t* r = finish(process);
        if (fault == 12) assert(r->cancel_requested && (r->term_signal == SIGTERM || r->term_signal == SIGKILL));
        else assert(r->exit_code == (fault >= 6 && fault <= 8 ? 125 : 0));
        assert(!r->resource_timed_out && !r->resource_violation && access("marker", F_OK) == -1);
        jash_agent_process_destroy(process);
        assert(child_group(options->cgroup_parent_fd) == -1);
    }
    fault = 0;
    deadline_test = 0;
    puts("PASS: final poll fits remaining 2 ms; late wake preserves exit, gate-failure, and cancel status");
}

static void descriptor_edge(int root, const jash_agent_limits_t* limits) {
    struct rlimit saved, reduced;
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    reduced = saved;
    reduced.rlim_cur = 64;
    jash_agent_cgroup_t* group = NULL;
    char error[256];
    for (size_t slots = 1; slots <= 4; slots++) {
        assert(setrlimit(RLIMIT_NOFILE, &reduced) == 0);
        int descriptors[64];
        size_t count = 0;
        int fd;
        while ((fd = dup(root)) >= 0) {
            assert(count < 64);
            descriptors[count++] = fd;
        }
        assert(errno == EMFILE && count > slots);
        /* Fail child open, configuration open, or snapshot open respectively;
         * four slots are enough to configure and return a usable handle. */
        for (size_t i = 0; i < slots; i++) close(descriptors[--count]);
        int rc = jash_agent_cgroup_create(root, limits, &group, error, sizeof error);
        int saved_errno = errno;
        while (count) close(descriptors[--count]);
        assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
        if (slots < 4) assert(rc == -1 && saved_errno == EMFILE && !group);
        else {
            assert(rc == 0 && group);
            assert(jash_agent_cgroup_destroy(group) == 0);
            group = NULL;
        }
        assert(child_group(root) == -1);
    }

    fault = 11;
    assert(jash_agent_cgroup_create(root, limits, &group, error, sizeof error) == -1);
    assert(errno == EMFILE && !group && replaced_group[0]);
    fault = 0;
    struct stat original, replacement;
    assert(fstat(replaced_original, &original) == 0);
    assert(fstatat(root, replaced_group, &replacement, AT_SYMLINK_NOFOLLOW) == 0);
    assert(original.st_ino != replacement.st_ino);
    close(replaced_original);
    assert(unlinkat(root, replaced_group, AT_REMOVEDIR) == 0);
    assert(child_group(root) == -1);
    puts("PASS: descriptor exhaustion leaves no own cgroup; replacement identity is never deleted");
}

int main(int argc, char** argv) {
    if (argc == 2 && strncmp(argv[1], "--", 2) == 0) return payload(argv[1]);
    jash_agent_limits_t limits = {16ull << 20, 2, 10000, 100000, 1000};
    char error[256];
    assert(jash_agent_limits_validate(&limits, error, sizeof error) == 0);
#define BOUNDS(field, low, high) do { \
    jash_agent_limits_t test = limits; \
    test.field = (low); assert(jash_agent_limits_validate(&test, error, sizeof error) == 0); \
    test.field = (high); assert(jash_agent_limits_validate(&test, error, sizeof error) == 0); \
    test.field = (low) - 1; assert(jash_agent_limits_validate(&test, error, sizeof error) == -1); \
    test.field = (high) + 1; assert(jash_agent_limits_validate(&test, error, sizeof error) == -1); \
} while (0)
    BOUNDS(memory_max_bytes, 16ull << 20, 1ull << 40);
    BOUNDS(pids_max, 1, 4096);
    BOUNDS(wall_timeout_ms, 1, 30000);
    BOUNDS(cpu_quota_us, 1000, 100000);
    limits.cpu_quota_us = 1000;
    BOUNDS(cpu_period_us, 1000, 1000000);
    assert(jash_agent_limits_validate(NULL, error, sizeof error) == -1);
#undef BOUNDS
    limits.cpu_quota_us = 100001;
    assert(jash_agent_limits_validate(&limits, error, sizeof error) == -1);
    limits.cpu_quota_us = 10000;
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    assert(options.resources == NULL && options.cgroup_parent_fd == -1);
    options.resources = &limits;
    options.stdout_path = "stdout";
    options.stderr_path = "stderr";
    jash_execution_ir_t ir;
    jash_execution_ir_init(&ir);
    ir.stage_count = 1;
    ir.stages[0].argc = 2;
    ir.stages[0].argv[0] = "/usr/bin/touch";
    ir.stages[0].argv[1] = "marker";
    jash_agent_process_t* process = NULL;
    assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == -1);
    assert(!process && access("marker", F_OK) == -1);
    options.cgroup_parent_fd = open(".", O_RDONLY | O_DIRECTORY);
    assert(options.cgroup_parent_fd >= 0);
    assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == -1);
    assert(!process && access("marker", F_OK) == -1);
    close(options.cgroup_parent_fd);
    puts("PASS: limits validation and all-or-refused startup");
    if (argc == 1) return 0;
    assert(argc == 2 || argc == 3);
    assert(argc == 2 || !strcmp(argv[2], "deadline") || !strcmp(argv[2], "descriptors"));
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root >= 0);
    options.cgroup_parent_fd = root;
    options.background = 1;
    limits.cpu_quota_us = 50000;
    limits.wall_timeout_ms = 2000;

    if (argc == 2 || !strcmp(argv[2], "deadline")) deadline_edge(&options, argv[0]);
    if (argc == 2 || !strcmp(argv[2], "descriptors")) descriptor_edge(root, &limits);
    if (argc == 3) { close(root); return 0; }

    /* A real cgroup with controllers withheld must fail before redirections. */
    assert(mkdirat(root, "unconfigured", 0700) == 0);
    options.cgroup_parent_fd = openat(root, "unconfigured", O_RDONLY | O_DIRECTORY);
    ir.stages[0].redirection_count = 1;
    ir.stages[0].redirections[0] = (jash_execution_redirection_t){1, JASH_EXEC_REDIR_WRITE_TRUNC, "marker"};
    assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == -1);
    assert(!process && access("marker", F_OK) == -1);
    close(options.cgroup_parent_fd);
    assert(unlinkat(root, "unconfigured", AT_REMOVEDIR) == 0);
    options.cgroup_parent_fd = root;
    ir.stage_count = 3;
    ir.stages[1] = ir.stages[2] = ir.stages[0];
    assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == -1);
    assert(!process && access("marker", F_OK) == -1);
    puts("PASS: missing controllers and stage-count refusal before redirections");

    ir.stage_count = 2;
    for (fault = 1; fault <= 5; fault++) {
        attachments = 0;
        assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == -1);
        assert(!process && access("marker", F_OK) == -1);
        assert(child_group(root) == -1);
    }
    fault = 0;
    puts("PASS: partial attach, launcher death, readback mismatch/overflow, migration population refusal");
    for (fault = 6; fault <= 8; fault++) {
        assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == 0);
        const jash_agent_process_result_t* gated = finish(process);
        assert(gated->exit_code == 125 && !gated->resource_violation);
        assert(access("marker", F_OK) == -1);
        jash_agent_process_destroy(process);
    }
    fault = 0;
    puts("PASS: worker gate EOF, error, and wrong token exit 125 before redirections/exec");

    limits.pids_max = 1;
    const char* modes[] = {"--pids", "--threads"};
    for (size_t i = 0; i < 2; i++) {
        process = start(&options, argv[0], modes[i], 1);
        const jash_agent_process_result_t* r = finish(process);
        assert(r->exit_code == 0 && r->resource_violation && r->resource_pids_events > 0);
        assert(!r->resource_timed_out && !r->resource_memory_events);
        jash_agent_process_destroy(process);
        assert(child_group(root) == -1);
    }
    puts("PASS: handled process/thread admission EAGAIN remains a violation at rc=0");

    process = start(&options, argv[0], "--pids-wait", 1);
    for (unsigned i = 0; i < 200 && access("denied", F_OK); i++) sleep_ms(1);
    assert(access("denied", F_OK) == 0);
    assert(kill(jash_agent_process_get_result(process)->launcher_pid, SIGKILL) == 0);
    assert(jash_agent_process_wait(process, 5000) == -1);
    const jash_agent_process_result_t* lost = jash_agent_process_get_result(process);
    assert(lost->indeterminate && lost->resource_empty && !lost->quiescent);
    assert(lost->resource_violation && lost->resource_pids_events > 0);
    jash_agent_process_destroy(process);
    assert(child_group(root) == -1);
    puts("PASS: unexpected launcher death cleans the aggregate and retains observed max events");

    limits.pids_max = 2;
    process = start(&options, argv[0], "--memory", 2);
    const jash_agent_process_result_t* r = finish(process);
    assert(r->resource_memory_events > 0 && r->resource_violation && !r->resource_timed_out);
    jash_agent_process_destroy(process);
    puts("PASS: aggregate two-worker memory pressure (24 MiB maximum allocation)");

    limits.wall_timeout_ms = 100;
    process = start(&options, argv[0], "--late-marker", 1);
    int group = child_group(root);
    assert(group >= 0);
    readback(group, "cpu.max", "50000 100000\n");
    readback(group, "memory.max", "16777216\n");
    readback(group, "memory.swap.max", "0\n");
    readback(group, "memory.oom.group", "1\n");
    readback(group, "pids.max", "2\n");
    sleep_ms(350); /* Deliberately no process poll/wait until after the deadline. */
    siginfo_t info;
    memset(&info, 0, sizeof info);
    r = jash_agent_process_get_result(process);
    assert(waitid(P_PID, (id_t)r->launcher_pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0);
    assert(info.si_pid == r->launcher_pid);
    readback(group, "cgroup.events", "populated 0\nfrozen 0\n");
    close(group);
    int64_t before = milliseconds();
    assert(jash_agent_process_poll(process, 0) == 1);
    assert(milliseconds() - before < 100);
    r = jash_agent_process_get_result(process);
    assert(r->resource_timed_out && r->resource_violation && r->resource_empty);
    assert(r->term_signal == SIGKILL && !r->resource_memory_events);
    assert(access("marker", F_OK) == -1);
    jash_agent_process_destroy(process);
    puts("PASS: configured readbacks and autonomous deadline without client polling");

    jash_execution_ir_init(&ir);
    ir.stage_count = 2;
    ir.stages[0].argc = ir.stages[1].argc = 2;
    ir.stages[0].argv[0] = ir.stages[1].argv[0] = argv[0];
    ir.stages[0].argv[1] = "--late-marker";
    ir.stages[1].argv[1] = "--zero";
    assert(jash_agent_process_start(&ir, &options, &process, error, sizeof error) == 0);
    r = finish(process);
    assert(r->exit_code == 0 && r->term_signal == 0 && r->resource_timed_out && r->resource_violation);
    assert(access("marker", F_OK) == -1);
    jash_agent_process_destroy(process);
    puts("PASS: timeout preserves last-worker rc=0 while marking the aggregate violation");

    limits.wall_timeout_ms = 5000;
    limits.cpu_quota_us = 5000;
    process = start(&options, argv[0], "--cpu", 2);
    group = child_group(root);
    assert(group >= 0);
    int cpu_fd = openat(group, "cpu.stat", O_RDONLY);
    char cpu_stat[1024];
    assert(cpu_fd >= 0);
    unsigned long long throttled = 0;
    /* finish() removes the empty cgroup: inspect the live controller first. */
    for (unsigned attempt = 0; attempt < 300 && !throttled; attempt++) {
        ssize_t cpu_len = pread(cpu_fd, cpu_stat, sizeof cpu_stat - 1, 0);
        assert(cpu_len > 0 && cpu_len < (ssize_t)sizeof cpu_stat - 1);
        cpu_stat[cpu_len] = 0;
        const char* throttle_count = strstr(cpu_stat, "nr_throttled ");
        assert(throttle_count && sscanf(throttle_count, "nr_throttled %llu", &throttled) == 1);
        if (!throttled) sleep_ms(10);
    }
    assert(throttled > 0);
    close(cpu_fd);
    close(group);
    r = finish(process);
    assert(r->exit_code == 0 && !r->resource_violation);
    jash_agent_process_destroy(process);
    puts("PASS: two CPU workers share bandwidth; real kernel throttling is not a violation");
    limits.cpu_quota_us = 50000;
    limits.wall_timeout_ms = 2000;
    process = start(&options, argv[0], "--signal", 1);
    r = finish(process);
    assert(r->term_signal == SIGKILL && !r->resource_violation && !r->resource_memory_events);
    jash_agent_process_destroy(process);
    puts("PASS: CPU bandwidth is not a violation; SIGKILL is not inferred OOM");

    process = start(&options, argv[0], "--detach", 1);
    r = finish(process);
    assert(r->exit_code == 23 && r->descendants_forced && !r->resource_violation);
    FILE* f = fopen("detached", "r");
    long detached;
    assert(f && fscanf(f, "%ld", &detached) == 1);
    fclose(f);
    assert(kill((pid_t)detached, 0) == -1 && errno == ESRCH);
    assert(access("marker", F_OK) == -1);
    jash_agent_process_destroy(process);
    assert(child_group(root) == -1);
    puts("PASS: detached cleanup preserves exact last-worker exit status");

    process = start(&options, argv[0], "--late-marker", 1);
    assert(jash_agent_process_cancel(process, 0) == 0);
    r = finish(process);
    assert(r->cancel_requested && !r->resource_timed_out && !r->resource_violation);
    jash_agent_process_destroy(process);
    options.output_limit_bytes = 4096;
    process = start(&options, argv[0], "--output", 1);
    r = finish(process);
    assert(r->output_limit_hit && !r->resource_violation);
    jash_agent_process_destroy(process);
    assert(child_group(root) == -1);
    options.output_limit_bytes = 65536;
    /* Root fd is pinned, not borrowed beyond start. */
    options.cgroup_parent_fd = dup(root);
    process = start(&options, argv[0], "--cpu", 1);
    close(options.cgroup_parent_fd);
    options.cgroup_parent_fd = root;
    limits.memory_max_bytes = 32ull << 20;
    r = finish(process);
    assert(r->exit_code == 0 && r->resources.memory_max_bytes == (16ull << 20));
    jash_agent_process_destroy(process);
    assert(child_group(root) == -1);
    close(root);
    puts("PASS: cancel/output-limit aggregate cleanup");
    return 0;
}
