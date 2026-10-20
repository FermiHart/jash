// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "agent_cgroup.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jash_agent_limits_validate(const jash_agent_limits_t* l, char* error, size_t cap) {
    const char* why = NULL;
    if (!l) why = "resource limits are required";
    else if (l->memory_max_bytes < (16ull << 20) || l->memory_max_bytes > (1ull << 40))
        why = "memory_max_bytes must be 16 MiB..1 TiB";
    else if (l->pids_max < 1 || l->pids_max > 4096)
        why = "pids_max must be 1..4096";
    else if (l->cpu_period_us < 1000 || l->cpu_period_us > 1000000)
        why = "cpu_period_us must be 1000..1000000";
    else if (l->cpu_quota_us < 1000 || l->cpu_quota_us > l->cpu_period_us)
        why = "cpu_quota_us must be 1000..cpu_period_us (at most one CPU)";
    else if (l->wall_timeout_ms < 1 || l->wall_timeout_ms > 30000)
        why = "wall_timeout_ms must be 1..30000";
    if (error && cap) snprintf(error, cap, "%s", why ? why : "");
    if (why) { errno = EINVAL; return -1; }
    return 0;
}

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

struct jash_agent_cgroup {
    int parent_fd;
    int fd;
    int kill_fd;
    char name[96];
    struct stat identity;
    int identity_valid;
};

static int read_file(int dir, const char* name, char* text, size_t cap) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, text + used, cap - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int saved = errno; close(fd); errno = saved; return -1; }
        if (!n) break;
        if (memchr(text + used, 0, (size_t)n)) {
            close(fd); errno = EIO; return -1;
        }
        used += (size_t)n;
        if (used == cap - 1) { close(fd); errno = EOVERFLOW; return -1; }
    }
    close(fd);
    text[used] = 0;
    return 0;
}

static int write_file(int dir, const char* name, const char* text) {
    int fd = openat(dir, name, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    ssize_t n;
    do { n = write(fd, text, len); } while (n < 0 && errno == EINTR);
    int saved = n < 0 ? errno : EIO;
    int closed = close(fd);
    if (n != (ssize_t)len) { errno = saved; return -1; }
    return closed;
}

/* Reject signs, overflow, duplicate requested keys, and trailing garbage. */
static int number(const char** cursor, uint64_t* out) {
    const char* p = *cursor;
    uint64_t n = 0;
    if (*p < '0' || *p > '9') { errno = EIO; return -1; }
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > (UINT64_MAX - digit) / 10) { errno = EOVERFLOW; return -1; }
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *out = n;
    return 0;
}

static int counter(int dir, const char* file, const char* key, uint64_t* out) {
    char text[4096];
    if (read_file(dir, file, text, sizeof text) != 0) return -1;
    const char* p = text;
    int found = 0;
    while (*p) {
        const char* start = p;
        while ((*p >= 'a' && *p <= 'z') || *p == '_') p++;
        size_t len = (size_t)(p - start);
        if (!len || *p++ != ' ') { errno = EIO; return -1; }
        uint64_t n;
        if (number(&p, &n) != 0) return -1;
        if (*p++ != '\n') { errno = EIO; return -1; }
        if (strlen(key) == len && memcmp(start, key, len) == 0) {
            if (found++) { errno = EIO; return -1; }
            *out = n;
        }
    }
    if (!found) { errno = EIO; return -1; }
    return 0;
}

int jash_agent_cgroup_empty(jash_agent_cgroup_t* g) {
    uint64_t populated;
    if (counter(g->fd, "cgroup.events", "populated", &populated) != 0) return -1;
    if (populated > 1) { errno = EIO; return -1; }
    return populated == 0;
}

int jash_agent_cgroup_snapshot(jash_agent_cgroup_t* g,
                               jash_agent_cgroup_snapshot_t* s) {
    char text[64];
    memset(s, 0, sizeof(*s));
    if (counter(g->fd, "memory.events", "max", &s->memory_events) != 0 ||
        counter(g->fd, "pids.events", "max", &s->pids_events) != 0 ||
        read_file(g->fd, "pids.current", text, sizeof text) != 0) return -1;
    const char* p = text;
    if (number(&p, &s->pids_current) != 0) return -1;
    if (strcmp(p, "\n")) { errno = EIO; return -1; }
    s->empty = jash_agent_cgroup_empty(g);
    return s->empty < 0 ? -1 : 0;
}

int jash_agent_cgroup_kill(jash_agent_cgroup_t* g) {
    ssize_t n;
    do { n = pwrite(g->kill_fd, "1", 1, 0); } while (n < 0 && errno == EINTR);
    if (n == 1) return 0;
    if (n >= 0) errno = EIO;
    return -1;
}

int jash_agent_cgroup_attach(jash_agent_cgroup_t* g, pid_t pid) {
    if (pid <= 0) { errno = EINVAL; return -1; }
    char text[32];
    snprintf(text, sizeof text, "%ld", (long)pid);
    return write_file(g->fd, "cgroup.procs", text);
}

int jash_agent_cgroup_destroy(jash_agent_cgroup_t* g) {
    if (!g) return 0;
    int rc = 0;
    if (g->name[0]) {
        struct stat named;
        if (!g->identity_valid ||
            fstatat(g->parent_fd, g->name, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
            g->identity.st_dev != named.st_dev || g->identity.st_ino != named.st_ino) {
            errno = EBUSY;
            rc = -1;
        } else {
            /* cgroupfs rmdir atomically refuses populated groups. Unlike an
             * events-file read, this also works when setup exhausted all FDs. */
            rc = unlinkat(g->parent_fd, g->name, AT_REMOVEDIR);
        }
    }
    int saved = errno;
    if (g->kill_fd >= 0) close(g->kill_fd);
    if (g->fd >= 0) close(g->fd);
    if (g->parent_fd >= 0) close(g->parent_fd);
    free(g);
    errno = saved;
    return rc;
}

int jash_agent_cgroup_create(int parent_fd, const jash_agent_limits_t* l,
                             jash_agent_cgroup_t** out, char* error, size_t cap) {
    if (!out) { errno = EINVAL; return -1; }
    *out = NULL;
    if (jash_agent_limits_validate(l, error, cap) != 0) return -1;
    jash_agent_cgroup_t* g = calloc(1, sizeof(*g));
    if (!g) return -1;
    g->fd = g->kill_fd = g->parent_fd = -1;
    g->parent_fd = fcntl(parent_fd, F_DUPFD_CLOEXEC, 3);
    struct statfs fs;
    if (g->parent_fd < 0 || fstatfs(g->parent_fd, &fs) != 0) goto fail;
    if ((unsigned long)fs.f_type != 0x63677270ul) { errno = ENOTSUP; goto fail; }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) goto fail;
    int made = 0;
    for (unsigned i = 0; i < 128; i++) {
        snprintf(g->name, sizeof g->name, "jash-%ld-%lld-%ld-%u", (long)getpid(),
                 (long long)now.tv_sec, now.tv_nsec, i);
        if (mkdirat(g->parent_fd, g->name, 0700) == 0) { made = 1; break; }
        if (errno != EEXIST) break;
    }
    if (!made) { g->name[0] = 0; goto fail; }
    /* Keep rollback identity before the first post-mkdir FD allocation. Never
     * remove a replacement at this name if opening the child subsequently fails. */
    if (fstatat(g->parent_fd, g->name, &g->identity, AT_SYMLINK_NOFOLLOW) != 0) goto fail;
    g->identity_valid = 1;
    g->fd = openat(g->parent_fd, g->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (g->fd < 0) goto fail;
    struct stat opened;
    if (fstat(g->fd, &opened) != 0) goto fail;
    if (opened.st_dev != g->identity.st_dev || opened.st_ino != g->identity.st_ino) {
        errno = ESTALE;
        goto fail;
    }
    const char* files[] = {"memory.max", "pids.max", "memory.swap.max", "memory.oom.group", "cpu.max"};
    uint64_t values[] = {l->memory_max_bytes, l->pids_max, 0, 1};
    for (size_t i = 0; i < 5; i++) {
        char expected[96], actual[128];
        if (i == 4) snprintf(expected, sizeof expected, "%llu %llu\n",
                             (unsigned long long)l->cpu_quota_us,
                             (unsigned long long)l->cpu_period_us);
        else snprintf(expected, sizeof expected, "%llu\n", (unsigned long long)values[i]);
        if (write_file(g->fd, files[i], expected) != 0 ||
            read_file(g->fd, files[i], actual, sizeof actual) != 0) goto fail;
        if (strcmp(expected, actual)) { errno = EIO; goto fail; }
    }
    g->kill_fd = openat(g->fd, "cgroup.kill", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    jash_agent_cgroup_snapshot_t snapshot;
    if (g->kill_fd < 0 || jash_agent_cgroup_kill(g) != 0 ||
        jash_agent_cgroup_snapshot(g, &snapshot) != 0) goto fail;
    if (!snapshot.empty || snapshot.pids_current || snapshot.memory_events || snapshot.pids_events) {
        errno = EBUSY; goto fail;
    }
    *out = g;
    return 0;
fail:
    {
        int saved = errno;
        (void)jash_agent_cgroup_destroy(g);
        if (error && cap) snprintf(error, cap, "cgroup resource setup refused: %s", strerror(saved));
        errno = saved;
        return -1;
    }
}
#else
int jash_agent_cgroup_create(int fd, const jash_agent_limits_t* l,
                             jash_agent_cgroup_t** out, char* error, size_t cap) {
    (void)fd; (void)l;
    if (out) *out = NULL;
    if (error && cap) snprintf(error, cap, "cgroup-v2 resources require Linux");
    errno = ENOTSUP; return -1;
}
int jash_agent_cgroup_attach(jash_agent_cgroup_t* g, pid_t p) {
    (void)g; (void)p; errno = ENOTSUP; return -1;
}
int jash_agent_cgroup_snapshot(jash_agent_cgroup_t* g, jash_agent_cgroup_snapshot_t* s) {
    (void)g; (void)s; errno = ENOTSUP; return -1;
}
int jash_agent_cgroup_kill(jash_agent_cgroup_t* g) { (void)g; errno = ENOTSUP; return -1; }
int jash_agent_cgroup_empty(jash_agent_cgroup_t* g) { (void)g; errno = ENOTSUP; return -1; }
int jash_agent_cgroup_destroy(jash_agent_cgroup_t* g) { (void)g; return 0; }
#endif
