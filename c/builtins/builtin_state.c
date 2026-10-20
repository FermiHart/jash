// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/state — live system state mirror for agents.
//
//   state              — human-readable state briefing
//   state --json       — machine-readable JSON envelope
//
// See Docs/ADR-0026-agent-experience-wave.md (Tier 0: --state-json)

#include "builtin.h"
#include "discovery_output.h"
#include "value_output.h"
#include <core/state.h>
#include <core/env.h>
#include <core/timeline.h>
#include <core/output_budget.h>
#include <core/structured_output.h>
#include <core/argv.h>
#include <core/jash_invoice.h>
#include <job_control.h>
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define STATE_MAX_ENV     256
#define STATE_MAX_GIT     64
#define STATE_MAX_TIMELINE 20
#define STATE_MAX_JOBS    32

/* ── helpers ────────────────────────────────────────────────────────────── */

static void state_cat_cmd(const char* shell_cmd, char* out, size_t outsz) {
    FILE* p = popen(shell_cmd, "r");
    if (!p) { out[0] = '\0'; return; }
    size_t n = fread(out, 1, outsz - 1, p);
    out[n] = '\0';
    /* strip trailing whitespace/newlines */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ' || out[n-1] == '\t'))
        out[--n] = '\0';
    pclose(p);
}

static int state_git_branch(char* out, size_t outsz) {
    state_cat_cmd("git rev-parse --abbrev-ref HEAD 2>/dev/null", out, outsz);
    return out[0] != '\0';
}

static int state_git_dirty(void) {
    char buf[256];
    state_cat_cmd("git status --porcelain 2>/dev/null | head -1", buf, sizeof(buf));
    return buf[0] != '\0';
}

/* ── human output ───────────────────────────────────────────────────────── */

static void state_emit_human(jash_state_t* s, jash_writer_t w, void* ctx) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), "%s", s->cwd);

    jash_writes(w, ctx, "\033[1;36m# jash state\033[0m\n\n");

    jash_writef(w, ctx, "\033[1mcwd:\033[0m %s\n\n", cwd);

    /* intent */
    const char* intent = jash_env_get(s, "JASH_INTENT");
    if (!intent || !intent[0]) {
        const char* home = getenv("HOME");
        static char ibuf[512];
        if (home) {
            char ip[600]; snprintf(ip, sizeof(ip), "%s/.jash/intent", home);
            FILE* fi = fopen(ip, "r");
            if (fi) { if (fgets(ibuf, sizeof(ibuf), fi)) intent = ibuf; fclose(fi); }
        }
    }
    if (intent && intent[0]) jash_writef(w, ctx, "\033[1mintent:\033[0m %s\n\n", intent);

    /* git */
    char branch[256];
    if (state_git_branch(branch, sizeof(branch))) {
        int dirty = state_git_dirty();
        jash_writef(w, ctx, "\033[1mgit branch:\033[0m %s%s\n\n", branch, dirty ? " (dirty)" : " (clean)");

        char status[4096];
        state_cat_cmd("git status --short 2>/dev/null | head -20", status, sizeof(status));
        if (status[0]) jash_writef(w, ctx, "\033[1mgit status:\033[0m\n%s\n\n", status);

        char diffstat[4096];
        state_cat_cmd("git diff --stat 2>/dev/null | tail -15", diffstat, sizeof(diffstat));
        if (diffstat[0]) jash_writef(w, ctx, "\033[1mgit diff --stat:\033[0m\n%s\n\n", diffstat);

        char recent[1024];
        state_cat_cmd("git log --oneline -5 2>/dev/null", recent, sizeof(recent));
        if (recent[0]) jash_writef(w, ctx, "\033[1mrecent commits:\033[0m\n%s\n\n", recent);
    }

    /* jobs */
    if (s->jobs) {
        jash_job_poll(s->jobs);
        /* job table internals are opaque; we can't enumerate without adding API.
         * For now, emit count and rely on `jobs` builtin for details. */
    }

    /* timeline */
    int tc = jash_timeline_count();
    if (tc > 0) {
        jash_writes(w, ctx, "\033[1mrecent mutations:\033[0m\n");
        int shown = tc < STATE_MAX_TIMELINE ? tc : STATE_MAX_TIMELINE;
        for (int i = 0; i < shown; i++) {
            int kind;
            const char *a, *b;
            if (!jash_timeline_peek(i, &kind, &a, &b)) break;
            jash_writef(w, ctx, "  %d: kind=%d a=%s b=%s\n", i+1, kind, a ? a : "", b ? b : "");
        }
        jash_writes(w, ctx, "\n");
    }

    /* last status */
    jash_writef(w, ctx, "\033[1mlast status:\033[0m rc=%d cmd=%s\n\n",
                s->last_status.code, s->last_status.cmd);

    /* env snapshot (JASH_* + PATH + HOME + USER + SHELL) */
    jash_writes(w, ctx, "\033[1mselected env:\033[0m\n");
    int emitted = 0;
    for (uint32_t i = 0; i < s->env_count && emitted < STATE_MAX_ENV; i++) {
        const char* k = s->env[i].key;
        const char* v = JASH_ENV_VAL(&s->env[i]);
        if (!k[0]) continue;
        int show = (strncmp(k, "JASH_", 5) == 0) ||
                   (strcmp(k, "PATH") == 0) ||
                   (strcmp(k, "HOME") == 0) ||
                   (strcmp(k, "USER") == 0) ||
                   (strcmp(k, "SHELL") == 0) ||
                   (strcmp(k, "PWD") == 0);
        if (show) {
            jash_writef(w, ctx, "  %s=%s\n", k, v);
            emitted++;
        }
    }
    jash_writes(w, ctx, "\n");
}

#include "state_native.inc"

/* ── dispatch ───────────────────────────────────────────────────────────── */

int jash_b_state(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    int native = semantic_dispatch(s, w, ctx, "state", "jash.state.v1", args, state_native_provider);
    if (native >= 0) return native;
    {
        const char* a = args ? args : "";
        uint64_t n = strlen(a) / 4;
        if (n < 1) n = 1;
        jash_invoice_account_command(a, 1);
        jash_invoice_add_saved(JASH_INVOICE_STATE, n);
    }

    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int mode = JASH_DISCOVERY_TEXT;
    int selected = jash_discovery_format(av.argc, av.argv, &mode);
    int invalid = av.argc && (av.argc != 1 || selected != 1);
    jash_argv_free(&av);
    if (invalid) return jash_discovery_error(w, ctx, "state", mode, "invalid_options", 2);

    /* Direct s->env[] reads below bypass jash_env_count()'s sync hook. */
    jash_env_sync_frame(s);

    jash_budget_wrap_t bw;
    jash_budget_wrap_init(&bw, w, ctx, "state");
    state_emit_human(s, jash_budget_wrap_writer(&bw), &bw);
    jash_budget_wrap_finish(&bw, w, ctx);
    return 0;
}
