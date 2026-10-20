// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — session_meta — LLM-friendly command metadata log

#include "session_meta.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>

static char g_meta_path[512];

static void meta_lazy_init(void) {
    if (g_meta_path[0]) return;

    const char* home = getenv("HOME");
    if (!home || !home[0]) return;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.jash", home);
    mkdir(dir, 0755);

    snprintf(g_meta_path, sizeof(g_meta_path), "%s/.jash/session_meta.jsonl", home);
}

// Write one JSON-safe string (escapes \, ", and control chars).
static void write_escaped(int fd, const char* s) {
    if (!s) return;
    ssize_t nw __attribute__((unused));
    while (*s) {
        char c = *s++;
        if (c == '"')       { nw = write(fd, "\\\"", 2); }
        else if (c == '\\') { nw = write(fd, "\\\\", 2); }
        else if (c == '\n') { nw = write(fd, "\\n",  2); }
        else if (c == '\r') { nw = write(fd, "\\r",  2); }
        else if (c == '\t') { nw = write(fd, "\\t",  2); }
        else                { nw = write(fd, &c, 1); }
    }
}

static void write_num(int fd, long long v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    ssize_t nw __attribute__((unused));
    if (n > 0) nw = write(fd, tmp, (size_t)n);
}

void jash_meta_record(const char* cmd, int exit_code, uint64_t dur_ms) {
    if (!cmd || !*cmd) return;

    // Skip pure-whitespace lines (empty Enter keypresses).
    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;

    meta_lazy_init();
    if (!g_meta_path[0]) return;

    struct timespec ts;
    long wall_ts = 0;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        wall_ts = ts.tv_sec;

    int fd = open(g_meta_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;

    ssize_t nw __attribute__((unused));
    nw = write(fd, "{\"ts\":", 6);
    write_num(fd, (long long)wall_ts);
    nw = write(fd, ",\"exit\":", 8);
    write_num(fd, (long long)exit_code);
    nw = write(fd, ",\"dur_ms\":", 10);
    write_num(fd, (long long)dur_ms);
    nw = write(fd, ",\"cmd\":\"", 8);
    write_escaped(fd, cmd);
    nw = write(fd, "\"}\n", 3);
    (void)nw;

    close(fd);
}

const char* jash_meta_path(void) {
    meta_lazy_init();
    return g_meta_path;
}
