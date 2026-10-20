// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — record — session recording for replay (impl).

#include "record.h"
#include <core/env.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/syscalls.h>
#include <time.h>

static char g_session_path[256];
static int g_recording = 0;
/* PERF (bench-loss root cause #4): the session log was opened and closed
 * around EVERY recorded command — path resolution per evaluated line. Keep
 * the append fd open for the life of the shell (O_APPEND keeps each write
 * atomic and self-positioning) and mark it CLOEXEC so children never
 * inherit it. F E R M I ∞ H A R T */
static long g_session_fd = -1;

void jash_record_init(jash_state_t* s) {
    if (!s) return;
    const char* home = jash_env_get(s, "HOME");
    if (!home) home = jash_tmpdir();

    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.jash_sessions", home);
    mkdir(dir, 0755);

    /* BUG-063 / ADR-0038 W5+W8: key the session file on the AGENT session id
     * when present, so every `jash -c` in one logical session APPENDS to one
     * record (the impact map and the handoff compiler both need per-session
     * accumulation — a per-process timestamped file would fragment every
     * command into its own file). Fall back to a timestamp otherwise. */
    const char* sid = jash_env_get(s, "CLAUDE_CODE_SESSION_ID");
    if (!sid || !*sid) sid = jash_env_get(s, "JASH_SESSION");
    if (sid && *sid) {
        char safe[128];
        unsigned j = 0;
        for (const char* p = sid; *p && j + 1 < sizeof(safe); p++) {
            char c = *p;
            safe[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '_';
        }
        safe[j] = '\0';
        snprintf(g_session_path, sizeof(g_session_path),
                 "%s/.jash_sessions/session_%s.jash", home, safe);
    } else {
        struct timespec tsp;
        long ts = 0;
        if (clock_gettime(CLOCK_REALTIME, &tsp) == 0) ts = tsp.tv_sec;
        snprintf(g_session_path, sizeof(g_session_path),
                 "%s/.jash_sessions/session_%ld.jash", home, ts);
    }
    if (g_session_fd >= 0) { sys_close(g_session_fd); g_session_fd = -1; }
    g_recording = 1;
}

void jash_record_command(jash_state_t* s, const char* cmd,
                         int exit_code, uint64_t duration_ns) {
    (void)s;
    if (!g_recording || !cmd || !*cmd) return;

    if (g_session_fd < 0) {
        /* BUG-063: the old raw flag 0x241 is O_WRONLY|O_CREAT|O_TRUNC on
         * Linux — it TRUNCATED the session file on every command, so only
         * the last command ever survived (silently starving the impact map
         * and handoff of history). Use the portable symbolic append flags. */
        g_session_fd = sys_open(g_session_path,
                                O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_session_fd < 0) return;
        fcntl((int)g_session_fd, F_SETFD, FD_CLOEXEC);
    }
    long fd = g_session_fd;

    struct timespec tsp;
    long ts = 0;
    if (clock_gettime(CLOCK_REALTIME, &tsp) == 0) {
        ts = tsp.tv_sec;
    }

    char line[2048];
    int n = 0;

    /* timestamp */
    char tmp[20]; int ti = 0;
    long v = ts;
    if (v == 0) tmp[ti++] = '0';
    else { while (v > 0 && ti < 19) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
    for (int i = ti - 1; i >= 0; i--) line[n++] = tmp[i];
    line[n++] = '|';

    /* exit_code */
    v = exit_code;
    ti = 0;
    if (v == 0) tmp[ti++] = '0';
    else { while (v > 0 && ti < 19) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
    for (int i = ti - 1; i >= 0; i--) line[n++] = tmp[i];
    line[n++] = '|';

    /* duration_ns */
    v = (long)duration_ns;
    ti = 0;
    if (v == 0) tmp[ti++] = '0';
    else { while (v > 0 && ti < 19) { tmp[ti++] = '0' + (v % 10); v /= 10; } }
    for (int i = ti - 1; i >= 0; i--) line[n++] = tmp[i];
    line[n++] = '|';

    /* command */
    int ci = 0;
    while (cmd[ci] && n < 2040) line[n++] = cmd[ci++];
    line[n++] = '\n';

    sys_write(fd, line, n);
}

const char* jash_record_session_path(void) {
    return g_session_path;
}

int jash_record_list_sessions(char out[][256], int max) {
    int count = 0;
    if (g_session_path[0] && count < max) {
        int i = 0;
        while (g_session_path[i] && i < 255) { out[count][i] = g_session_path[i]; i++; }
        out[count][i] = '\0';
        count++;
    }
    return count;
}

int jash_record_replay(const char* path, jash_record_replay_cb cb, void* ctx) {
    if (!path || !cb) return -1;

    long fd = sys_open(path, 0, 0);
    if (fd < 0) return -1;

    char buf[8192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    char* p = buf;
    while (*p) {
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* ec_str = p;
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* dur_str = p;
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* cmd = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;

        int exit_code = 0;
        { char* s = ec_str; while (*s) { exit_code = exit_code * 10 + (*s - '0'); s++; } }

        uint64_t duration = 0;
        { char* s = dur_str; while (*s) { duration = duration * 10 + (*s - '0'); s++; } }

        if (cmd[0]) cb(cmd, exit_code, duration, ctx);
    }
    return 0;
}

int jash_record_stats(const char* path, jash_record_stats_t* stats) {
    if (!path || !stats) return -1;
    memset(stats, 0, sizeof(*stats));

    long fd = sys_open(path, 0, 0);
    if (fd < 0) return -1;

    char buf[8192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    char* p = buf;
    while (*p) {
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* ec_str = p;
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* dur_str = p;
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;

        char* cmd = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;

        int exit_code = 0;
        { char* s = ec_str; while (*s) { exit_code = exit_code * 10 + (*s - '0'); s++; } }

        uint64_t duration = 0;
        { char* s = dur_str; while (*s) { duration = duration * 10 + (*s - '0'); s++; } }

        stats->total_commands++;
        if (exit_code != 0) stats->failed_commands++;
        stats->total_duration_ns += duration;
        if (duration > stats->longest_command_ns) {
            stats->longest_command_ns = duration;
            int i = 0;
            while (cmd[i] && i < 255) { stats->longest_command[i] = cmd[i]; i++; }
            stats->longest_command[i] = '\0';
        }
    }
    return 0;
}
