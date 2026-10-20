// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: source/. — execute script in current shell context

#include "builtin.h"
#include <parser/eval.h>
#include <sys/syscalls.h>
#include <string.h>

/* Defined in main.c — does heredoc preprocessing + compound-statement
 * (if/for/while/case/select/function) accumulation across newlines.
 * Required for sourcing realistic profile/init scripts. */
extern int jash_run_multiline(jash_state_t* state, const char* sp);

int jash_b_source(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writes(jash_stderr_writer, 0, "source: usage: source FILENAME\n");
        return 1;
    }

    const char* path = args;
    while (*path == ' ' || *path == '\t') path++;

    long fd = sys_open(path, 0, 0);
    if (fd < 0) {
        jash_writes(jash_stderr_writer, 0, "source: ");
        jash_writes(jash_stderr_writer, 0, path);
        jash_writes(jash_stderr_writer, 0, ": No such file or directory\n");
        return 1;
    }

    char buf[65536];
    unsigned len = 0;
    long nread;
    while ((nread = sys_read(fd, buf + len, sizeof(buf) - len - 1)) > 0)
        len += (unsigned)nread;
    sys_close(fd);
    buf[len] = '\0';

    const char* sp = buf;
    if (sp[0] == '#' && sp[1] == '!') {
        while (*sp && *sp != '\n') sp++;
        if (*sp == '\n') sp++;
    }

    (void)w; (void)ctx;
    /* Isolate the sourced script's failures from the caller. A syntax error
     * inside the script (R2-18 sets s->exit_requested) must NOT propagate
     * past `source file || true` — the caller asked to ignore failures.
     * Hand the whole body to jash_run_multiline so heredocs, function
     * definitions, and if/for/while/case across newlines work like in
     * script mode. F E R M I ∞ H A R T */
    int saved_exit_requested = s->exit_requested;
    int saved_status = s->last_status.code;

    int rc = jash_run_multiline(s, sp);

    /* Always swallow the exit_requested leak (the sourced script may have
     * hit syntax errors, but we don't tear down the calling shell). */
    s->exit_requested = saved_exit_requested;
    if (rc == 0) s->last_status.code = saved_status;
    return rc;
}

__attribute__((used)) int jash_b_dot(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    return jash_b_source(s, w, ctx, args);
}
