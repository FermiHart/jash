// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_weave - Flow Integration (ADR-0099 W6)
// F E R M I ∞ H A R T
//
// weave {"name": "command", ...} [--json]
//
// Runs N named shell commands and emits one unified result — human summary
// or a single JSON envelope (ADR-0099 W4). Thin by design (W2.6): no risk
// logic, no state, just batch + envelope.
//
// Capture: each task runs as `( cmd ) > tmpfile 2>&1` through the shell's
// own redirect machinery — writer-based capture only sees builtin output;
// external commands write straight to fd 1. The tempfile catches both,
// stdout+stderr merged into `out` (agents want everything), read back with
// a hard cap so a runaway task can never flood the session window
// (ADR-0044 budget discipline). Governor/nudge text keeps going to the REAL
// stderr (BUG-148): it never enters the envelope.

#include <builtins/builtin.h>
#include <core/state.h>
#include <parser/eval.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "discovery_output.h"
#include "value_output.h"

#define WEAVE_MAX_TASKS   32
#define WEAVE_NAME_CAP    64
#define WEAVE_CMD_CAP     1024
#define WEAVE_OUT_CAP     16384

typedef struct {
    char name[WEAVE_NAME_CAP];
    char cmd[WEAVE_CMD_CAP];
    int  rc;
    char out[WEAVE_OUT_CAP];
    size_t out_len;
    int  out_truncated;
} weave_task_t;

/* Run one task: evaluate `( cmd ) > tmp 2>&1`, then read tmp back capped
 * at WEAVE_OUT_CAP. Returns the task's rc; fills out/out_len/truncated. */
static int weave_run_task(jash_state_t* s, jash_writer_t w, void* wctx,
                          weave_task_t* t) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "/tmp/jash_weave_%ld_XXXXXX", (long)getpid());
    int fd = mkstemp(tmp);
    if (fd < 0) {
        /* No tempfile: run uncaptured rather than not at all. */
        t->out[0] = '\0';
        return jash_eval_line(s, w, wctx, t->cmd);
    }
    (void)close(fd);

    char line[WEAVE_CMD_CAP + 160];
    int n = snprintf(line, sizeof line, "( %s ) > %s 2>&1", t->cmd, tmp);
    if (n <= 0 || (size_t)n >= sizeof line) {
        unlink(tmp);
        t->out[0] = '\0';
        return 2;
    }
    /* w/wctx passed through: the `> tmp 2>&1` redirect catches everything
     * the task emits, so nothing reaches the session writer. */
    int rc = jash_eval_line(s, w, wctx, line);

    fd = open(tmp, O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, t->out, WEAVE_OUT_CAP - 1);
        if (got < 0) got = 0;
        t->out_len = (size_t)got;
        t->out[t->out_len] = '\0';
        char probe;
        if (read(fd, &probe, 1) == 1) t->out_truncated = 1;
        (void)close(fd);
    }
    unlink(tmp);
    return rc;
}

static const char* weave_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a "quoted string" into dst (escapes: \" \\ \n \t). Returns pointer
 * past the closing quote, or NULL on malformed input. */
static const char* weave_parse_str(const char* p, char* dst, size_t cap) {
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            if (*p == 'n') c = '\n';
            else if (*p == 't') c = '\t';
            else c = *p;
        }
        if (n + 1 < cap) dst[n++] = c;
        p++;
    }
    if (*p != '"') return NULL;
    dst[n] = '\0';
    return p + 1;
}

/* Parse a task name or command value: either a "quoted string" or a bare
 * run of characters up to `delims` (unquoted form — the shell has already
 * stripped quotes and split metachars, so pipes/redirects need the quoted
 * form; bare form is convenience for simple commands). Trims trailing ws. */
static const char* weave_parse_value(const char* p, char* dst, size_t cap,
                                     const char* delims) {
    if (*p == '"') return weave_parse_str(p, dst, cap);
    size_t n = 0;
    while (*p && !strchr(delims, *p)) {
        if (n + 1 < cap) dst[n++] = *p;
        p++;
    }
    while (n > 0 && (dst[n-1] == ' ' || dst[n-1] == '\t')) n--;
    dst[n] = '\0';
    return n ? p : NULL;
}

/* Emit s as a JSON string literal (with surrounding quotes). */
static void weave_json_str(jash_writer_t w, void* ctx, const char* s) {
    jash_writes(w, ctx, "\"");
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  jash_writes(w, ctx, "\\\""); break;
            case '\\': jash_writes(w, ctx, "\\\\"); break;
            case '\n': jash_writes(w, ctx, "\\n"); break;
            case '\r': jash_writes(w, ctx, "\\r"); break;
            case '\t': jash_writes(w, ctx, "\\t"); break;
            default:
                if (c < 0x20) jash_writef(w, ctx, "\\u%04x", c);
                else { char b[2] = { (char)c, 0 }; jash_writes(w, ctx, b); }
        }
    }
    jash_writes(w, ctx, "\"");
}

#include "weave_native.inc"

static void weave_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx,
        "usage: weave [--json] {\"name\": \"command\", ...}\n"
        "\n"
        "Run named shell commands and emit one unified result.\n"
        "\n"
        "Examples:\n"
        "  weave '{\"count\": \"find . -name '*.c' | wc -l\", \"done\": \"echo done\"}'\n"
        "  weave --json '{\"build\": \"make\", \"test\": \"make test\"}'\n"
        "  weave {count: echo 42, done: echo ok}     (bare form, no pipes)\n"
        "\n"
        "Single-quote the object when commands contain pipes, redirects or\n"
        "shell metachars — otherwise the shell interprets them first.\n"
    );
}

int jash_b_weave(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;

    if (!args || !args[0]) {
        weave_usage(w, ctx);
        return 0;
    }

    const char* a = weave_skip_ws(args);
    int json_out = 0;
    if (strncmp(a, "--json", 6) == 0 && (a[6] == ' ' || a[6] == '\t' || !a[6])) {
        json_out = 1;
        a = weave_skip_ws(a + 6);
    }

    if (*a != '{') {
        weave_usage(w, ctx);
        return 2;
    }
    a++;

    static weave_task_t tasks[WEAVE_MAX_TASKS];
    int ntasks = 0;
    int parse_err = 0;

    for (;;) {
        a = weave_skip_ws(a);
        if (*a == '}') { a++; break; }
        if (!*a) { parse_err = 1; break; }
        if (ntasks >= WEAVE_MAX_TASKS) { parse_err = 1; break; }

        weave_task_t* t = &tasks[ntasks];
        memset(t, 0, sizeof *t);

        a = weave_parse_value(a, t->name, sizeof t->name, ":");
        if (!a) { parse_err = 1; break; }
        a = weave_skip_ws(a);
        if (*a != ':') { parse_err = 1; break; }
        a = weave_skip_ws(a + 1);
        a = weave_parse_value(a, t->cmd, sizeof t->cmd, ",}");
        if (!a) { parse_err = 1; break; }
        ntasks++;

        a = weave_skip_ws(a);
        if (*a == ',') { a++; continue; }
        if (*a == '}') { a++; break; }
        parse_err = 1;
        break;
    }

    if (parse_err || ntasks == 0) {
        jash_writes(w, ctx, "weave: malformed task object\n");
        weave_usage(w, ctx);
        return 2;
    }

    int failed = 0;
    for (int i = 0; i < ntasks; i++) {
        tasks[i].rc = weave_run_task(s, w, ctx, &tasks[i]);
        if (tasks[i].rc != 0) failed++;
    }

    if (json_out) {
        jash_writes(w, ctx, "{\"status\":\"");
        jash_writes(w, ctx, failed ? "failed" : "completed");
        jash_writef(w, ctx, "\",\"tasks\":%d,\"failed\":%d,\"results\":[", ntasks, failed);
        for (int i = 0; i < ntasks; i++) {
            if (i) jash_writes(w, ctx, ",");
            jash_writes(w, ctx, "{\"name\":");
            weave_json_str(w, ctx, tasks[i].name);
            jash_writes(w, ctx, ",\"cmd\":");
            weave_json_str(w, ctx, tasks[i].cmd);
            jash_writef(w, ctx, ",\"rc\":%d,\"out\":", tasks[i].rc);
            weave_json_str(w, ctx, tasks[i].out);
            if (tasks[i].out_truncated) jash_writes(w, ctx, ",\"truncated\":true");
            jash_writes(w, ctx, "}");
        }
        jash_writes(w, ctx, "]}\n");
        return failed ? 1 : 0;
    }

    for (int i = 0; i < ntasks; i++) {
        jash_writef(w, ctx, "── %s (rc=%d) ──\n", tasks[i].name, tasks[i].rc);
        if (tasks[i].out_len) {
            jash_writes(w, ctx, tasks[i].out);
            if (tasks[i].out[tasks[i].out_len - 1] != '\n')
                jash_writes(w, ctx, "\n");
        }
        if (tasks[i].out_truncated)
            jash_writes(w, ctx, "[weave: output truncated]\n");
    }
    jash_writef(w, ctx, "weave: %d task%s, %d failed\n",
                ntasks, ntasks == 1 ? "" : "s", failed);
    return failed ? 1 : 0;
}
