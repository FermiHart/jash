// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: exec — replace the shell with an external command

#include "builtin.h"
#include <parser/eval.h>
#include <parser/exec.h>
#include <parser/builtin_redir.h>
#include <core/env.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>   /* snprintf for the -a/-l argv[0] rewrite */
#include <theme/io.h>

int jash_b_exec(jash_state_t *s, jash_writer_t w, void *ctx,
                 const char *args) {
    (void)w; (void)ctx;
    if (!args || !*args) {
        /* `exec` alone applies pending redirects permanently (handled by
         * the redir-only path) — there's no command to swap in. */
        return 0;
    }

    while (*args == ' ' || *args == '\t') args++;

    /* Redirect-only form `exec >log 2>&1`: no command after the redirects.
     * Apply them permanently to the running shell, then return. */
    if (*args == '<' || *args == '>') {
        size_t size = strlen(args) + sizeof("exec ");
        char* line = malloc(size);
        if (!line) return 1;
        snprintf(line, size, "exec %s", args);
        int rc = jash_exec_apply_redirs_permanently(s, line);
        free(line);
        if (rc >= 0) return rc;
    }

    /* Parse argv from the args string (no shell expansion beyond what
     * jash_unquote already did upstream). Build envp from jash's env. */
    char argv_buf[8192];
    char* argv[256];
    int argc = 0;
    unsigned ab = 0;
    const char* p = args;
    while (*p && argc < 255 && ab + 1 < sizeof(argv_buf)) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = &argv_buf[ab];
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == quote) { quote = 0; p++; continue; }
                argv_buf[ab++] = *p++;
                if (ab + 1 >= sizeof(argv_buf)) break;
                continue;
            }
            if (*p == '\'' || *p == '"') { quote = *p++; continue; }
            if (*p == ' ' || *p == '\t') break;
            argv_buf[ab++] = *p++;
            if (ab + 1 >= sizeof(argv_buf)) break;
        }
        argv_buf[ab++] = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) return 0;

    /* ── bash option surface: exec [-cl] [-a NAME] [COMMAND [ARG...]] ──────
     *
     * -a NAME  run COMMAND with argv[0] set to NAME (the file that gets
     *          executed is still COMMAND — only what the process sees as its
     *          own name changes)
     * -c       run with an EMPTY environment
     * -l       put a '-' in front of argv[0], the login-shell convention
     *
     * These were not parsed at all: `-a` fell through as the program name,
     * so `exec -a ugrep /path/to/bin` reported "exec: -a: not found" and
     * exited 127. That is not theoretical — Claude Code's shell snapshot
     * routes `grep` and `find` through
     *     (exec -a ugrep "$_cc_bin" ...)
     * so with jash as the shell EVERY grep and find died at 127 before the
     * real command ran, and the failure looked like the builtin was missing.
     */
    int   ai = 0;
    const char* argv0_name = NULL;
    int   clear_env = 0, login_dash = 0;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        if (argv[ai][1] == '-' && argv[ai][2] == '\0') { ai++; break; }  /* -- */
        const char* f = argv[ai] + 1;
        int recognised = 1;
        for (; *f; f++) {
            if      (*f == 'c') clear_env = 1;
            else if (*f == 'l') login_dash = 1;
            else if (*f == 'a') {
                if (f[1]) {                      /* -aNAME */
                    argv0_name = f + 1;
                    while (f[1]) f++;            /* consume the rest */
                } else if (ai + 1 < argc) {      /* -a NAME */
                    argv0_name = argv[++ai];
                } else {
                    jash_writef(jash_stderr_writer, 0,
                                "jash: exec: -a: option requires an argument\n");
                    return 2;
                }
            } else { recognised = 0; break; }
        }
        if (!recognised) break;   /* not ours — treat as part of the command */
        ai++;
    }
    /* Flags only (`exec -c`): nothing to swap in. */
    if (ai >= argc) return 0;

    char** cargv = &argv[ai];

    /* Resolve path: absolute/relative used as-is, otherwise PATH lookup.
     * Resolution uses the REAL command, never the -a alias. */
    const char* prog = cargv[0];
    char fullpath[1024];
    if (prog[0] != '/' && !(prog[0] == '.' && (prog[1] == '/' ||
            (prog[1] == '.' && prog[2] == '/')))) {
        if (jash_path_lookup(s, prog, fullpath, sizeof(fullpath))) {
            prog = fullpath;
        }
    }

    /* Now, and only now, rewrite what the child sees as argv[0]. */
    char argv0_buf[1024];
    const char* shown = argv0_name ? argv0_name : cargv[0];
    if (login_dash) {
        snprintf(argv0_buf, sizeof(argv0_buf), "-%s", shown);
        cargv[0] = argv0_buf;
    } else if (argv0_name) {
        snprintf(argv0_buf, sizeof(argv0_buf), "%s", shown);
        cargv[0] = argv0_buf;
    }

    /* Build envp from the in-shell env table. */
    static char* envp_buf = NULL;
    static char* envp[1024];
    if (!envp_buf) {
        envp_buf = malloc(65536);
        if (!envp_buf) exit(127);
    }
    unsigned eb = 0;
    int envc = 0;
    uint32_t nenv = clear_env ? 0u : jash_env_count(s);   /* -c: empty env */
    for (uint32_t i = 0; i < nenv && envc < 1023; i++) {
        const char *k = 0, *v = 0;
        if (!jash_env_at(s, i, &k, &v)) continue;
        unsigned kl = (unsigned)strlen(k);
        unsigned vl = (unsigned)strlen(v);
        if (eb + kl + 1 + vl + 1 >= 65536) break;
        envp[envc++] = &envp_buf[eb];
        memcpy(envp_buf + eb, k, kl); eb += kl;
        envp_buf[eb++] = '=';
        memcpy(envp_buf + eb, v, vl); eb += vl;
        envp_buf[eb++] = '\0';
    }
    envp[envc] = NULL;

    /* Replace the shell. On success this does NOT return. */
    execve(prog, cargv, envp);

    /* execve failed: report and exit (bash parity — exec failure terminates
     * the shell with 127, NOT a fall-through to the next command). Name the
     * command that could not be run, not the -a alias, or the message points
     * at something that was never a path. */
    jash_writef(jash_stderr_writer, 0,
                "jash: exec: %s: not found\n", argv[ai]);
    exit(127);
}
