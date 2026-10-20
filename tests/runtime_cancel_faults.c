// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <poll.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static int used;
static int maintenance_result_published;
static int admission_written, admission_paused;
static int history_paused;
static int journal_gc_captured, journal_gc_sync_failed;

static void admission_pause(const char* gate) {
    if (!gate) return;
    int marker = open(gate, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (marker >= 0) {
        close(marker); char release[4096]; snprintf(release, sizeof(release), "%s.release", gate);
        for (int i = 0; i < 10000 && access(release, F_OK); i++) usleep(1000);
    }
}

int fstatat(int fd, const char* path, struct stat* st, int flags) {
    int (*next)(int, const char*, struct stat*, int) = dlsym(RTLD_NEXT, "fstatat");
    int result = next(fd, path, st, flags);
    const char* gate = getenv("JASH_TEST_HISTORY_STAT_GATE");
    if (!result && gate && !history_paused && strstr(path, ".intent")) {
        history_paused = 1; admission_pause(gate);
    }
    return result;
}

int fstatvfs(int fd, struct statvfs* vfs) {
    int (*next)(int, struct statvfs*) = dlsym(RTLD_NEXT, "fstatvfs");
    int result = next(fd, vfs);
    const char* pressure = getenv("JASH_TEST_JOURNAL_SPACE");
    if (!result && pressure) {
        if (!strcmp(pressure, "unknown")) { errno = EIO; return -1; }
        if (!strcmp(pressure, "inodes")) vfs->f_favail = 0;
        else vfs->f_bavail = 0;
    }
    return result;
}

int open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    int (*next)(const char*, int, ...) = dlsym(RTLD_NEXT, "open");
    if ((flags & O_CREAT) && strstr(path, ".maintenance-decision.dsse")) {
        admission_pause(getenv("JASH_TEST_MAINT_ADMISSION_GATE"));
        if (getenv("JASH_TEST_MAINT_ADMISSION_FAIL")) { errno = EIO; return -1; }
    }
    int fd = next(path, flags, mode);
    if (fd >= 0 && (flags & O_CREAT) && strstr(path, ".maintenance-decision.dsse"))
        admission_pause(getenv("JASH_TEST_MAINT_ADMISSION_OPEN_GATE"));
    return fd;
}

static void await_shutdown(void) {
    int (*next)(struct pollfd*, nfds_t, int) = dlsym(RTLD_NEXT, "poll");
    DIR* directory = opendir("/proc/self/fd"); struct dirent* entry;
    while (directory && (entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        int fd = atoi(entry->d_name), type = 0; socklen_t size = sizeof(type);
        if (!getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) && type == SOCK_SEQPACKET) {
            struct pollfd lifecycle = {fd, POLLIN, 0}; (void)next(&lifecycle, 1, 5000); break;
        }
    }
    if (directory) closedir(directory);
}

static const char* process_wait_kind(struct pollfd* fds, nfds_t count, int timeout) {
    if (timeout <= 0 || timeout > 50) return NULL;
    if (!count && timeout <= 10) return "polling";
    /* The supervisor now waits on the launcher's pidfd.  Match that actual
     * completion source instead of relying on the former sleep-only loop. */
    if (count > 2) return NULL;
    for (nfds_t i = 0; i < count; i++) {
        if (!(fds[i].events & POLLIN)) continue;
        char path[64], target[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[i].fd);
        ssize_t size = readlink(path, target, sizeof(target) - 1);
        if (size < 0) continue;
        target[size] = 0;
        if (!strcmp(target, "anon_inode:[pidfd]")) return "completion-pidfd";
    }
    return NULL;
}

int poll(struct pollfd* fds, nfds_t count, int timeout) {
    int (*next)(struct pollfd*, nfds_t, int) = dlsym(RTLD_NEXT, "poll");
    const char* marker = getenv("JASH_TEST_SHUTDOWN_WAIT_GATE");
    const char* wait_kind = marker ? process_wait_kind(fds, count, timeout) : NULL;
    if (wait_kind) {
        char armed[4096]; snprintf(armed, sizeof(armed), "%s.arm", marker);
        if (!access(armed, F_OK)) {
            int ready = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (ready >= 0) {
                char observation[96];
                int size = snprintf(observation, sizeof(observation), "%ld %s\n", (long)getpid(), wait_kind);
                if (size > 0 && (size_t)size < sizeof(observation)) (void)write(ready, observation, (size_t)size);
                close(ready);
                await_shutdown();
            }
        }
    }
    return next(fds, count, timeout);
}

/* Deterministic pause after one real effect, before the next cancellation
 * boundary. Test-owned inherited pipes, no timers or production fault knobs. */
static void barrier(const char* source, int rc) {
    const char* ready = getenv("JASH_TEST_CANCEL_READY_FD");
    const char* release = getenv("JASH_TEST_CANCEL_RELEASE_FD");
    const char* leaf = strrchr(source, '/'); leaf = leaf ? leaf + 1 : source;
    int fifo = getenv("JASH_TEST_CANCEL_FIFO") != NULL;
    if (rc || used || ((!ready || !release) && !fifo) || strcmp(leaf, "first")) return;
    used = 1;
    char byte = '1';
    int notify = fifo ? open(".cancel-ready", O_WRONLY | O_CLOEXEC) : atoi(ready);
    int resume = fifo ? open(".cancel-release", O_RDONLY | O_CLOEXEC) : atoi(release);
    if (notify >= 0) (void)write(notify, &byte, 1);
    if (resume >= 0) while (read(resume, &byte, 1) < 0 && errno == EINTR) {}
    if (getenv("JASH_TEST_WAIT_SHUTDOWN_NATIVE")) await_shutdown();
    if (fifo) { if (notify >= 0) close(notify); if (resume >= 0) close(resume); }
}

ssize_t sendmsg(int fd, const struct msghdr* message, int flags) {
    if (getenv("JASH_TEST_LOCAL_SEND_FAIL") && message->msg_controllen) { errno = EPIPE; return -1; }
    ssize_t (*next)(int, const struct msghdr*, int) = dlsym(RTLD_NEXT, "sendmsg");
    return next(fd, message, flags);
}

int socketpair(int domain, int type, int protocol, int endpoints[2]) {
    if (getenv("JASH_TEST_LOCAL_PAIR_FAIL") && (type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) == SOCK_STREAM) { errno = EMFILE; return -1; }
    int (*next)(int, int, int, int[2]) = dlsym(RTLD_NEXT, "socketpair");
    return next(domain, type, protocol, endpoints);
}

ssize_t write(int fd, const void* data, size_t size) {
    ssize_t (*next)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
    const char* enospc = getenv("JASH_TEST_JOURNAL_ENOSPC");
    if (enospc) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, "/agent_maintenance/.") &&
                ((!strcmp(enospc, "intent") && strstr(path, ".intent.")) || (!strcmp(enospc, "result") && strstr(path, ".result.")))) {
                errno = ENOSPC; return -1;
            }
        }
    }
    const char* pause = getenv("JASH_TEST_JOB_REPLY_PAUSE");
    if (pause && memmem(data, size, "\"receipt_phase\":\"start\"", 23)) {
        int marker = open(pause, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (marker >= 0) {
            close(marker);
            char release[4096]; snprintf(release, sizeof(release), "%s.release", pause);
            for (int attempt = 0; attempt < 5000 && access(release, F_OK); attempt++) usleep(1000);
        }
    }
    ssize_t rc = next(fd, data, size);
    const char* point = getenv("JASH_TEST_CANCEL_POINT");
    if (point && !strcmp(point, "before") && fd == STDOUT_FILENO && size >= 26 &&
        !memcmp(data, "{\"schema\":\"jash.mutation.v1\"", 26)) {
        used = 0; barrier("first", rc < 0 ? -1 : 0);
    }
    if (point && !strcmp(point, "copy") && rc > 0 && fd != STDOUT_FILENO) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) { path[n] = 0; barrier(path, 0); }
    }
    return rc;
}

int unlink(const char* path) {
    int (*next)(const char*) = dlsym(RTLD_NEXT, "unlink");
    int rc = next(path); barrier(path, rc); return rc;
}

int unlinkat(int fd, const char* name, int flags) {
    if (getenv("JASH_TEST_JOURNAL_GC_UNLINK_FAIL") && !strncmp(name, ".jgc-", 5)) { errno = EIO; return -1; }
    if (getenv("JASH_TEST_STORE_UNLINK_FAIL") && !strncmp(name, ".gc-", 4)) { errno = EIO; return -1; }
    int (*next)(int, const char*, int) = dlsym(RTLD_NEXT, "unlinkat");
    return next(fd, name, flags);
}

int renameat2(int from, const char* source, int to, const char* target, unsigned flags) {
    if (getenv("JASH_TEST_DATA_PUBLISH_FAIL") && strstr(target, "/agent_artifacts/")) { errno = ENOSPC; return -1; }
    int journal_capture = !strncmp(target, ".jgc-", 5);
    if (journal_capture && getenv("JASH_TEST_JOURNAL_GC_CAPTURE_FAIL")) { errno = EIO; return -1; }
    if (journal_capture && getenv("JASH_TEST_JOURNAL_GC_SWAP")) {
        if (!renameat(from, source, from, ".saved-journal-original")) {
            int fd = openat(from, source, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { (void)write(fd, "raced", 5); close(fd); }
        }
    }
    const char* gate = getenv("JASH_TEST_MAINT_COMMIT_GATE");
    if (gate && strstr(target, "/agent_maintenance/") && strstr(target, ".result")) {
        int marker = open(gate, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (marker >= 0) { close(marker); char release[4096]; snprintf(release, sizeof(release), "%s.release", gate);
            for (int i = 0; i < 10000 && access(release, F_OK); i++) usleep(1000); }
    }
    if (getenv("JASH_TEST_MAINT_COMMIT_FAIL") && strstr(target, "/agent_maintenance/") && strstr(target, ".result")) { errno = EIO; return -1; }
    if (getenv("JASH_TEST_JOB_ARCHIVE_FAIL") && strstr(target, ".result")) { errno = EIO; return -1; }
    if (getenv("JASH_TEST_STORE_CAPTURE_SWAP") && !strncmp(target, ".gc-blake3-", 11)) {
        if (!renameat(from, source, from, "saved-original")) {
            int fd = openat(from, source, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { (void)write(fd, "raced", 5); close(fd); }
        }
    }
    int (*next)(int, const char*, int, const char*, unsigned) = dlsym(RTLD_NEXT, "renameat2");
    int rc = next(from, source, to, target, flags);
    if (!rc && strstr(target, "/agent_artifacts/")) admission_pause(getenv("JASH_TEST_DATA_PUBLISH_GATE"));
    if (!rc && journal_capture) {
        journal_gc_captured = 1;
        if (getenv("JASH_TEST_JOURNAL_GC_PUBLIC_CHANGE")) {
            char public[96]; snprintf(public, sizeof(public), "%.*s.result", 64, target + 5);
            int fd = openat(to, public, O_WRONLY | O_TRUNC, 0600);
            if (fd >= 0) { (void)write(fd, "changed", 7); close(fd); }
            fd = openat(from, source, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) { (void)write(fd, "occupied", 8); close(fd); }
        }
        admission_pause(getenv("JASH_TEST_JOURNAL_GC_CAPTURE_GATE"));
    }
    if (!rc && strstr(target, "/agent_maintenance/") && strstr(target, ".result")) maintenance_result_published = 1;
    const char* intent_gate = getenv("JASH_TEST_MAINT_INTENT_GATE");
    if (!rc && intent_gate && strstr(target, "/agent_maintenance/") && strstr(target, ".intent")) {
        int marker = open(intent_gate, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (marker >= 0) { close(marker); char release[4096]; snprintf(release, sizeof(release), "%s.release", intent_gate);
            for (int i = 0; i < 10000 && access(release, F_OK); i++) usleep(1000); }
    }
    const char* point = getenv("JASH_TEST_CANCEL_POINT");
    if (!point || strcmp(point, "commit")) barrier(source, rc);
    return rc;
}

int renameat(int from, const char* source, int to, const char* target) {
    if (getenv("JASH_TEST_DATA_POLICY_FAIL") && !strcmp(target, "data_policy")) { errno = EIO; return -1; }
    if (getenv("JASH_TEST_JOURNAL_POLICY_FAIL") && !strcmp(target, "journal_quota")) { errno = ENOSPC; return -1; }
    int (*next)(int, const char*, int, const char*) = dlsym(RTLD_NEXT, "renameat");
    return next(from, source, to, target);
}

int fsync(int fd) {
    int (*next)(int) = dlsym(RTLD_NEXT, "fsync");
    if (journal_gc_captured && !journal_gc_sync_failed && getenv("JASH_TEST_JOURNAL_GC_SYNC_FAIL")) {
        journal_gc_sync_failed = 1; errno = EIO; return -1;
    }
    if (getenv("JASH_TEST_MAINT_ADMISSION_SYNC_FAIL")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, ".maintenance-decision.dsse")) admission_written = 1;
            const char* leaf = strrchr(path, '/');
            if (admission_written && leaf && !strcmp(leaf, "/agent_maintenance")) { errno = EIO; return -1; }
        }
    }
    if (getenv("JASH_TEST_MAINT_DIRSYNC_FAIL")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0; const char* leaf = strrchr(path, '/');
            if (strstr(path, "/agent_maintenance/") && leaf && leaf[1] != '.' && n >= 7 && !strcmp(path + n - 7, ".result")) maintenance_result_published = 1;
            if (maintenance_result_published && leaf && !strcmp(leaf, "/agent_maintenance")) { errno = EIO; return -1; }
        }
    }
    if (getenv("JASH_TEST_MAINT_PREPARE_FAIL")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) { path[n] = 0; if (strstr(path, "/agent_maintenance/.") && strstr(path, ".intent.")) { errno = EIO; return -1; } }
    }
    int rc = next(fd);
    if (getenv("JASH_TEST_MAINT_ADMISSION_AFTER_GATE")) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0 && !rc) {
            path[n] = 0;
            if (strstr(path, ".maintenance-decision.dsse")) admission_written = 1;
            const char* leaf = strrchr(path, '/');
            if (admission_written && !admission_paused && leaf && !strcmp(leaf, "/agent_maintenance")) {
                admission_paused = 1; admission_pause(getenv("JASH_TEST_MAINT_ADMISSION_AFTER_GATE"));
            }
        }
    }
    const char* gate = getenv("JASH_TEST_SHUTDOWN_ARCHIVE_GATE");
    if (gate) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) {
            path[n] = 0;
            if (strstr(path, "/agent_job_results/.") && strstr(path, ".tmp")) {
                int marker = open(gate, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (marker >= 0) {
                    close(marker);
                    char release[4096]; snprintf(release, sizeof(release), "%s.release", gate);
                    for (int i = 0; i < 5000 && access(release, F_OK); i++) usleep(1000);
                }
            }
        }
    }
    const char* parent = getenv("JASH_TEST_CANCEL_COMMIT_PARENT");
    if (parent) {
        char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n >= 0) { path[n] = 0; if (!strcmp(parent, path)) barrier("first", rc); }
    }
    return rc;
}
