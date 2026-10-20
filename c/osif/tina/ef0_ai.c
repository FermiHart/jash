/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* TinaLinux EF-0: reserve '?' for advisory AETHER, never module dispatch. */
#include <core/jash_ai.h>
#include <theme/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

static int ai_mode;
static int ai_busy;
static int ai_debug;

static int mode_write(const char* mode) {
    int fd = open("/run/aether/mode", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t len = strlen(mode);
    int ok = write(fd, mode, len) == (ssize_t)len && write(fd, "\n", 1) == 1;
    close(fd);
    return ok ? 0 : -1;
}

static int mode_read(void) {
    int fd = open("/run/aether/mode", O_RDONLY);
    if (fd < 0) return 0;
    char mode[16] = {0};
    ssize_t n = read(fd, mode, sizeof(mode) - 1);
    close(fd);
    if (n <= 0) return 0;
    if (!strncmp(mode, "bypass", 6)) {
        (void)mode_write("on");
        return 1;
    }
    return !strncmp(mode, "on", 2);
}

void jash_ai_init(void) { ai_mode = mode_read(); }
int jash_ai_active(void) { return ai_mode; }
int jash_ai_auto(void) { return 0; }
int jash_ai_learn(void) { return 0; }
int jash_ai_busy(void) { return ai_busy; }
int jash_ai_debug(void) { return ai_debug; }
void jash_ai_set_mode(int mode) { (void)mode; ai_mode = mode_read(); }
void jash_ai_set_auto(int mode) { (void)mode; }
void jash_ai_set_learn(int mode) { (void)mode; }
void jash_ai_set_debug(int mode) { ai_debug = mode != 0; }
void jash_ai_set_busy(int busy) { ai_busy = busy != 0; }

void jash_ai_submit_query(jash_state_t* s, jash_writer_t w, void* ctx, const char* query) {
    char line[8192];
    int n = snprintf(line, sizeof(line), "? %s", query ? query : "");
    if (n >= 0 && (size_t)n < sizeof(line)) (void)jash_ai_route(s, w, ctx, line);
}

void jash_ai_help(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "\r\n=== AETHER EF-0 ===\r\n"
        "  ? on               wake advisory oracles\r\n"
        "  ? off              put oracles to sleep\r\n"
        "  ? status           show advisory posture\r\n"
        "  ? [oracle] query   advice only; never apply effects\r\n");
}

void jash_ai_render_spinner(jash_writer_t w, void* ctx) {
    if (!ai_mode) return;
    jash_writes(w, ctx, ai_busy ? "\033[31m⠋\033[0m" : "\033[32m●\033[0m");
}

/* Strict UTF-8 streaming sanitizer, including U+0080..U+009F terminal
 * controls; carry only an incomplete sequence between read(2) buffers. */
typedef struct { unsigned char carry[4]; size_t len; } stream_t;
static size_t expected_len(unsigned char c) {
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
static size_t valid_len(const unsigned char* s, size_t n) {
    if (n >= 2 && s[0] >= 0xc2 && s[0] <= 0xdf && s[1] >= 0x80 && s[1] <= 0xbf) return 2;
    if (n >= 3 && s[0] >= 0xe0 && s[0] <= 0xef &&
        s[1] >= 0x80 && s[1] <= 0xbf && s[2] >= 0x80 && s[2] <= 0xbf &&
        !(s[0] == 0xe0 && s[1] < 0xa0) && !(s[0] == 0xed && s[1] >= 0xa0)) return 3;
    if (n >= 4 && s[0] >= 0xf0 && s[0] <= 0xf4 &&
        s[1] >= 0x80 && s[1] <= 0xbf && s[2] >= 0x80 && s[2] <= 0xbf &&
        s[3] >= 0x80 && s[3] <= 0xbf &&
        !(s[0] == 0xf0 && s[1] < 0x90) && !(s[0] == 0xf4 && s[1] >= 0x90)) return 4;
    return 0;
}
static void write_untrusted(jash_writer_t w, void* ctx, stream_t* stream,
                            const unsigned char* data, size_t len, int final) {
    unsigned char bytes[2052];
    size_t n = stream->len + len;
    if (n > sizeof(bytes)) return;
    memcpy(bytes, stream->carry, stream->len);
    if (len) memcpy(bytes + stream->len, data, len);
    stream->len = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = bytes[i];
        if (c < 0x80) {
            if ((c >= 0x20 && c != 0x7f) || c == '\t' || c == '\n')
                (void)w(ctx, (const char*)&bytes[i], 1);
            i++;
            continue;
        }
        size_t expected = expected_len(c);
        if (expected && n - i < expected && !final) {
            stream->len = n - i;
            memcpy(stream->carry, bytes + i, stream->len);
            break;
        }
        size_t seq = valid_len(bytes + i, n - i);
        if (seq) {
            /* U+0080..U+009F are controls even in canonical UTF-8. */
            if (!(c == 0xc2 && bytes[i + 1] <= 0x9f))
                (void)w(ctx, (const char*)&bytes[i], seq);
            i += seq;
        } else i++;
    }
}

static int sanitize_stdin(jash_writer_t w, void* ctx) {
    unsigned char buf[2048];
    stream_t stream = {{0}, 0};
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) { write_untrusted(w, ctx, &stream, buf, (size_t)n, 0); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 1;
        break;
    }
    write_untrusted(w, ctx, &stream, NULL, 0, 1);
    return 0;
}

static int command_is(const char* cmd, const char* word) {
    size_t n = strlen(word);
    return !strncmp(cmd, word, n) &&
        (cmd[n] == 0 || cmd[n] == ' ' || cmd[n] == '\t');
}

static int query(jash_writer_t w, void* ctx, const char* cmd) {
    static const char* names[] = {
        "sophia", "hermes", "forge", "argus", "lyra", "atlas", "council"
    };
    const char* oracle = names[0];
    const char* question = cmd;
    const char* end = cmd;
    while (*end && *end != ' ' && *end != '\t') end++;
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        if (strlen(names[i]) == (size_t)(end - cmd) && !strncmp(cmd, names[i], end - cmd)) {
            oracle = names[i];
            question = end;
            while (*question == ' ' || *question == '\t') question++;
            break;
        }
    }
    if (!*question) {
        jash_writes(w, ctx, "aether: ask something after ?\r\n");
        return 1;
    }
    int output[2];
    if (pipe(output)) return 1;
    pid_t pid = fork();
    if (pid < 0) { close(output[0]); close(output[1]); return 1; }
    if (pid == 0) {
        close(output[0]);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        execl("/usr/bin/tina-oracle-ask", "tina-oracle-ask",
              oracle, question, (char*)NULL);
        _exit(127);
    }
    close(output[1]);
    ai_busy = 1;
    unsigned char buf[2048];
    stream_t stream = {{0}, 0};
    int read_failed = 0;
    for (;;) {
        ssize_t n = read(output[0], buf, sizeof(buf));
        if (n > 0) { write_untrusted(w, ctx, &stream, buf, (size_t)n, 0); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) read_failed = 1;
        break;
    }
    write_untrusted(w, ctx, &stream, NULL, 0, 1);
    close(output[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    ai_busy = 0;
    if (waited != pid || read_failed) return 1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        jash_writes(w, ctx, "aether: advisory helper unavailable\r\n");
        return 127;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

/* For the guest, this returns an exit status (0 success); all '?' traffic
 * reaches this function before any native module prehook or dispatch. */
int jash_ai_route(jash_state_t* s, jash_writer_t w, void* ctx, const char* line) {
    (void)s;
    if (!line || line[0] != '?') return 1;
    const char* cmd = line + 1;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd || !strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        jash_ai_help(w, ctx); return 0;
    }
    if (!strcmp(cmd, "--ef0-sanitize-check")) return sanitize_stdin(w, ctx);
    if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
        int on = cmd[1] == 'n';
        if (mode_write(on ? "on" : "sleep")) {
            jash_writes(w, ctx, "aether: mode change failed\r\n"); return 1;
        }
        ai_mode = on;
        jash_writes(w, ctx, on ? "aether: advisory mode on (EF-0; no effects).\r\n"
                               : "aether: oracles asleep.\r\n");
        return 0;
    }
    if (!strcmp(cmd, "status")) {
        ai_mode = mode_read();
        jash_writes(w, ctx, ai_mode ? "aether: mode on (EF-0 advisory-only).\r\n"
                                    : "aether: mode sleep (EF-0 advisory-only).\r\n");
        return 0;
    }
    if (command_is(cmd, "bypass")) {
        jash_writes(w, ctx, "aether: bypass disabled; EF-0 remains advisory-only.\r\n");
        return 1;
    }
    if (command_is(cmd, "auto") || command_is(cmd, "learn") || command_is(cmd, "qa")) {
        jash_writes(w, ctx, "aether: autonomous Jash AI controls disabled by EF-0.\r\n");
        return 1;
    }
    ai_mode = mode_read();
    if (!ai_mode) {
        jash_writes(w, ctx, "aether: oracles asleep; use '? on' to wake them\r\n");
        return 1;
    }
    return query(w, ctx, cmd);
}
