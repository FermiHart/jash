// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash -- core/output_budget -- stdout cap for LLM agent sessions (Feature 1, ADR-0022)

#include "output_budget.h"
#include "salience.h"       /* ADR-0042 F4: JASH_GOVERNOR=salient ranking */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscalls.h>

long jash_budget_limit(void) {
    const char* e = getenv("JASH_LLM_BUDGET");
    if (!e || !*e) return 0;
    char* end = NULL;
    long v = strtol(e, &end, 10);
    if (end && (*end == 'k' || *end == 'K')) v *= 1024;
    else if (end && (*end == 'm' || *end == 'M')) v *= 1024 * 1024;
    return v > 0 ? v : 0;
}

int jash_tmp_path(const char* name, char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = jash_tmpdir();
    char root[512];
    char dir[512];
    snprintf(root, sizeof(root), "%s/.jash", home);
    snprintf(dir, sizeof(dir), "%s/.jash/tmp", home);
    if (mkdir(root, 0700) != 0 && errno != EEXIST) return 0;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return 0;
    struct stat st;
    if (lstat(root, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return 0;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        return 0;
    int n = snprintf(buf, cap, "%s/%s_%d", dir, name, (int)getpid());
    return (n > 0 && (size_t)n < cap) ? 1 : 0;
}

int jash_budget_setup(int* fd_out, char* path_out, size_t path_len) {
    if (!jash_tmp_path(".jash_out", path_out, path_len))
        snprintf(path_out, path_len, "/tmp/.jash_out_%d", (int)getpid());
    int flags = O_RDWR | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path_out, flags, 0600);
    if (fd < 0) return 0;
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0) {
        close(fd);
        return 0;
    }
    *fd_out = fd;
    return 1;
}

static size_t ansi_strip(const char* src, size_t len, char* dst, size_t cap) {
    size_t di = 0;
    for (size_t i = 0; i < len && di < cap; ) {
        if (src[i] == '\033' && i + 1 < len) {
            char next = src[i + 1];
            if (next == '[') {
                i += 2;
                while (i < len && (src[i] < 0x40 || src[i] > 0x7e)) i++;
                if (i < len) i++;
                continue;
            } else if (next == ']') {
                i += 2;
                while (i < len && src[i] != '\007') i++;
                if (i < len) i++;
                continue;
            } else if (next >= 0x40 && next <= 0x7e) {
                i += 2; continue;
            }
        }
        dst[di++] = src[i++];
    }
    return di;
}

int jash_budget_flush(const char* tmppath, long limit, const char* cmdname,
                      int strip_ansi, jash_writer_t w, void* ctx) {
    /* ADR-0042 F4: salient governor ranks lines instead of tail-capping. On
     * -1 (declined: too large / I/O) fall through to the classic tail path. */
    if (limit > 0 && jash_governor_is_salient()) {
        int sr = jash_salience_flush(tmppath, limit, cmdname, strip_ansi, w, ctx);
        if (sr >= 0) return sr;
    }
    int fd = open(tmppath, O_RDONLY);
    if (fd < 0) return 0;
    char raw[8192], stripped[8192];
    long shown = 0, total = 0;
    ssize_t nr;
    int overflowed = 0, ofd = -1;
    char overflow_path[256] = {0};
    while ((nr = read(fd, raw, sizeof(raw))) > 0) {
        total += nr;
        const char* eb = raw;
        size_t el = (size_t)nr;
        if (strip_ansi) { el = ansi_strip(raw, (size_t)nr, stripped, sizeof(stripped)); eb = stripped; }
        if (limit == 0) {
            ssize_t nw __attribute__((unused)); nw = write(STDOUT_FILENO, eb, el);
        } else {
            long cap = limit - shown;
            if (cap > 0) {
                long out = (long)el < cap ? (long)el : cap;
                ssize_t nw __attribute__((unused)); nw = write(STDOUT_FILENO, eb, (size_t)out);
                shown += out;
                if (out < (long)el && !overflowed) {
                    const char* home = getenv("HOME"); if (!home) home = jash_tmpdir();
                    char dir[256]; snprintf(dir, sizeof(dir), "%s/.jash/overflow", home);
                    mkdir(dir, 0700);
                    const char* base = cmdname ? cmdname : "cmd";
                    snprintf(overflow_path, sizeof(overflow_path), "%s/%d-%s.txt", dir, (int)getpid(), base);
                    ofd = open(overflow_path, O_WRONLY | O_CREAT | O_TRUNC, 0600); overflowed = 1;
                    if (ofd >= 0) { nw = write(ofd, eb + out, el - (size_t)out); }
                } else if (overflowed && ofd >= 0) { nw = write(ofd, eb, el); }
            } else {
                if (!overflowed) {
                    const char* home = getenv("HOME"); if (!home) home = jash_tmpdir();
                    char dir[256]; snprintf(dir, sizeof(dir), "%s/.jash/overflow", home);
                    mkdir(dir, 0700);
                    const char* base = cmdname ? cmdname : "cmd";
                    snprintf(overflow_path, sizeof(overflow_path), "%s/%d-%s.txt", dir, (int)getpid(), base);
                    ofd = open(overflow_path, O_WRONLY | O_CREAT | O_TRUNC, 0600); overflowed = 1;
                }
                if (ofd >= 0) { ssize_t nw __attribute__((unused)); nw = write(ofd, eb, el); }
            }
        }
    }
    int fully_shown = !overflowed;
    close(fd); if (ofd >= 0) close(ofd);
    if (overflowed && total > shown) {
        /* Context governor (ADR-0023 E5): the held output is captured, but the
         * value is telling the agent HOW to get it — close the loop to `recall`
         * (its overflow-query facet) instead of dumping a bare path. */
        const char* handle = overflow_path;
        for (const char* p = overflow_path; *p; p++) if (*p == '/') handle = p + 1;
        char msg[640];
        snprintf(msg, sizeof(msg),
                 "[context-governor] output capped: %ld B shown, %ld B held — "
                 "read the rest with `recall` (recall tail 40 | recall grep PAT | recall %s)\n",
                 shown, total - shown, handle);
        jash_writes(w, ctx, msg);
    }
    return fully_shown;
}

/* ── writer-level budget enforcement for builtins ───────────────────────── */

static void budget_overflow_open(jash_budget_wrap_t* bw) {
    if (bw->overflow_fd >= 0) return;
    const char* home = getenv("HOME"); if (!home || !*home) home = jash_tmpdir();
    char dir[256]; snprintf(dir, sizeof(dir), "%s/.jash/overflow", home);
    mkdir(dir, 0700);
    const char* base = bw->cmdname ? bw->cmdname : "cmd";
    snprintf(bw->overflow_path, sizeof(bw->overflow_path),
             "%s/%d-%s.txt", dir, (int)getpid(), base);
    bw->overflow_fd = open(bw->overflow_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

static long jash_budget_wrapped_write(void* ctx, const char* buf, size_t n) {
    jash_budget_wrap_t* bw = (jash_budget_wrap_t*)ctx;
    if (bw->limit <= 0) return bw->real_w(bw->real_ctx, buf, n);

    long remaining = bw->limit - bw->shown;
    if (remaining > 0) {
        long show = (long)n < remaining ? (long)n : remaining;
        long r = bw->real_w(bw->real_ctx, buf, (size_t)show);
        if (r < 0) return r;
        bw->shown += show;
        if (show < (long)n) {
            budget_overflow_open(bw);
            if (bw->overflow_fd >= 0) {
                ssize_t nw __attribute__((unused));
                nw = write(bw->overflow_fd, buf + show, n - (size_t)show);
            }
            bw->truncated = 1;
        }
        return (long)n;
    }

    budget_overflow_open(bw);
    if (bw->overflow_fd >= 0) {
        ssize_t nw __attribute__((unused));
        nw = write(bw->overflow_fd, buf, n);
    }
    bw->truncated = 1;
    return (long)n;
}

void jash_budget_wrap_init(jash_budget_wrap_t* bw, jash_writer_t w, void* ctx, const char* cmdname) {
    bw->real_w = w;
    bw->real_ctx = ctx;
    bw->limit = jash_budget_limit();
    bw->shown = 0;
    bw->overflow_fd = -1;
    bw->overflow_path[0] = '\0';
    bw->cmdname = cmdname;
    bw->truncated = 0;
}

jash_writer_t jash_budget_wrap_writer(jash_budget_wrap_t* bw) {
    return jash_budget_wrapped_write;
}

void jash_budget_unwrap(jash_writer_t* writer, void** context) {
    while (*writer == jash_budget_wrapped_write && *context) {
        jash_budget_wrap_t* wrapper = *context;
        *writer = wrapper->real_w;
        *context = wrapper->real_ctx;
    }
}

void jash_budget_wrap_finish(jash_budget_wrap_t* bw, jash_writer_t notify_w, void* notify_ctx) {
    if (bw->overflow_fd >= 0) { close(bw->overflow_fd); bw->overflow_fd = -1; }
    if (bw->truncated && bw->overflow_path[0]) {
        const char* handle = bw->overflow_path;
        for (const char* p = bw->overflow_path; *p; p++) if (*p == '/') handle = p + 1;
        char msg[640];
        snprintf(msg, sizeof(msg),
                 "[context-governor] output capped: %ld B shown — read the rest with `recall %s`\n",
                 bw->shown, handle);
        jash_writes(notify_w, notify_ctx, msg);
    }
}
