// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/token_tracker — real token counting for sessions (ADR-0034)
// F E R M I ∞ H A R T

#include "token_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/syscalls.h>

static char s_session_path[512];
static int s_initialized = 0;

void jash_token_init(void) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = jash_tmpdir();
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.jash", home);
    mkdir(dir, 0700);
    snprintf(s_session_path, sizeof(s_session_path), "%s/.jash/token_usage.session", home);
    s_initialized = 1;
}

void jash_token_record(long cmd_tokens, long output_bytes, double exec_ms) {
    if (!s_initialized) jash_token_init();

    // Read current values
    jash_session_tokens_t cur = {0, 0, 0, 0};
    FILE* f = fopen(s_session_path, "r");
    if (f) {
        /* A short/absent read leaves `cur` at its {0,0,0,0} init, which is the
         * right answer for a missing or truncated counter file. glibc marks
         * fscanf warn_unused_result, so the outcome is checked explicitly
         * rather than discarded. */
        if (fscanf(f, "%ld %ld %ld %ld", &cur.input_tokens, &cur.output_tokens,
                   &cur.total_commands, &cur.total_ms) != 4) {
            cur = (jash_session_tokens_t){0, 0, 0, 0};
        }
        fclose(f);
    }

    // Accumulate
    cur.input_tokens += cmd_tokens;
    cur.output_tokens += output_bytes / JASH_CHARS_PER_TOKEN;
    cur.total_commands++;
    cur.total_ms += (long)exec_ms;

    // Write back
    f = fopen(s_session_path, "w");
    if (f) {
        fprintf(f, "%ld %ld %ld %ld\n", cur.input_tokens, cur.output_tokens,
                cur.total_commands, cur.total_ms);
        fclose(f);
    }
}

int jash_session_tokens(jash_session_tokens_t* out) {
    if (!s_initialized) jash_token_init();

    FILE* f = fopen(s_session_path, "r");
    if (!f) {
        out->input_tokens = 0;
        out->output_tokens = 0;
        out->total_commands = 0;
        out->total_ms = 0;
        return 0;
    }
    int r = fscanf(f, "%ld %ld %ld %ld", &out->input_tokens, &out->output_tokens,
                   &out->total_commands, &out->total_ms);
    fclose(f);
    return (r == 4) ? 0 : -1;
}

int jash_session_tokens_readonly(jash_session_tokens_t* out, int* exists) {
    if (!out) { errno = EINVAL; return -1; }
    const char* home = getenv("HOME");
    if (!home || !*home) home = jash_tmpdir();
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/.jash/token_usage.session", home);
    if (length < 0 || (size_t)length >= sizeof(path)) { errno = ENAMETOOLONG; return -1; }
    if (exists) *exists = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        *out = (jash_session_tokens_t){0}; return 0;
    }
    if (exists) *exists = 1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    unsigned char buffer[257]; size_t used = 0; int failed = 0;
    for (;;) {
        ssize_t n = read(fd, buffer + used, sizeof(buffer) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { failed = errno; break; }
        if (!n) break;
        used += (size_t)n;
        if (used == sizeof(buffer)) { failed = E2BIG; break; }
    }
    if (close(fd) && !failed) failed = errno;
    if (failed) { errno = failed; return -1; }
    size_t offset = 0; long values[4];
    for (size_t i = 0; i < 4; i++) {
        while (offset < used && isspace(buffer[offset])) offset++;
        if (offset == used || buffer[offset] < '0' || buffer[offset] > '9') { errno = EINVAL; return -1; }
        unsigned long number = 0;
        while (offset < used && buffer[offset] >= '0' && buffer[offset] <= '9') {
            unsigned digit = buffer[offset++] - '0';
            if (number > ((unsigned long)LONG_MAX - digit) / 10u) { errno = EOVERFLOW; return -1; }
            number = number * 10u + digit;
        }
        if (offset < used && !isspace(buffer[offset])) { errno = EINVAL; return -1; }
        values[i] = (long)number;
    }
    while (offset < used && isspace(buffer[offset])) offset++;
    if (offset != used) { errno = EINVAL; return -1; }
    *out = (jash_session_tokens_t){values[0], values[1], values[2], values[3]}; return 0;
}

void jash_session_tokens_reset(void) {
    if (!s_initialized) jash_token_init();

    FILE* f = fopen(s_session_path, "w");
    if (f) {
        fprintf(f, "0 0 0 0\n");
        fclose(f);
    }
}

// Writer wrapper that counts bytes
static long token_wrap_write(void* ctx, const char* buf, size_t n) {
    jash_token_wrap_t* tw = (jash_token_wrap_t*)ctx;
    tw->bytes_counted += (long)n;
    return tw->real_w(tw->real_ctx, buf, n);
}

void jash_token_wrap_init(jash_token_wrap_t* tw, jash_writer_t w, void* ctx) {
    tw->real_w = w;
    tw->real_ctx = ctx;
    tw->bytes_counted = 0;
}

jash_writer_t jash_token_wrap_writer(jash_token_wrap_t* tw) {
    return token_wrap_write;
}

long jash_token_wrap_bytes(jash_token_wrap_t* tw) {
    return tw->bytes_counted;
}
