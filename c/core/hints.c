// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/hints — see hints.h. F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "hints.h"
#include "nudge.h"            /* jash_nudge_due — shared cooldown stamps   */
#include "adopt.h"            /* jash_adopt_level                          */
#include "differentiators.h"  /* never hint an unregistered differentiator */
#include <module/sigil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define HINT_BUDGET_DEFAULT     5     /* hints per session, JASH_HINT_BUDGET */
#define HINT_COOLDOWN_FAILED  600     /* failure tip: at most 1 per 10 min   */
#define HINT_COOLDOWN_MOMENT 1800     /* moment rules: at most 1 per 30 min  */
#define BUDGET_CACHE_TTL      5       /* seconds between budget file re-reads */

/* ── gates ─────────────────────────────────────────────────────────────── */

static int hints_enabled(void) {
    const char* e = getenv("JASH_NUDGE");     /* one kill switch for ALL tips */
    if (e && e[0] == '0') return 0;
    /* Only in recording/agent context — never surprise an interactive human
     * who hasn't opted into the exocortex. */
    return jash_sigil_get() >= SIGIL_LOG;
}

static int hint_budget(void) {
    const char* e = getenv("JASH_HINT_BUDGET");
    if (!e || !*e) return HINT_BUDGET_DEFAULT;
    int v = atoi(e);
    return v < 0 ? 0 : v;
}

/* Session-scoped spend counter: ~/.jash/nudges/budget.<JASH_SESSION|default>.
 * -c calls are separate processes; the counter must survive them. */
static int budget_path(char* p, size_t n) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    const char* s = getenv("JASH_SESSION");
    if (!s || !*s) s = "default";
    /* mkdir only once per process, not on every budget check. */
    static int dirs_made = 0;
    if (!dirs_made) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/.jash", h);        mkdir(dir, 0700);
        snprintf(dir, sizeof dir, "%s/.jash/nudges", h); mkdir(dir, 0700);
        dirs_made = 1;
    }
    int r = snprintf(p, n, "%s/.jash/nudges/budget.%.64s", h, s);
    return r > 0 && (size_t)r < n;
}

/* In-memory mirror of the budget file: one fopen/fscanf per command was
 * per-keystroke waste. Writes still go straight through (other -c processes
 * share the file); reads are re-validated every BUDGET_CACHE_TTL seconds. */
static int    g_budget_cache = -1;  /* -1 = not read yet in this process */
static time_t g_budget_at = 0;

static int budget_used(void) {
    time_t now = time(NULL);
    if (g_budget_cache >= 0 && now - g_budget_at < BUDGET_CACHE_TTL)
        return g_budget_cache;
    char p[700]; if (!budget_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "r"); if (!f) return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    g_budget_cache = v < 0 ? 0 : v;
    g_budget_at = now;
    return g_budget_cache;
}

static void budget_bump(void) {
    char p[700]; if (!budget_path(p, sizeof p)) return;
    int v = budget_used() + 1;
    FILE* f = fopen(p, "w"); if (!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
    g_budget_cache = v;
    g_budget_at = time(NULL);
}

/* Dashboard introspection (ADR-0040 W6) */
int jash_hint_budget_used(void) { return budget_used(); }
int jash_hint_budget_cap(void)  { return hint_budget(); }

/* ── emission ──────────────────────────────────────────────────────────── */

static void emit(const char* msg) {
    jash_writes(jash_stderr_writer, 0, "\033[90m[jash] tip: ");
    jash_writes(jash_stderr_writer, 0, msg);
    jash_writes(jash_stderr_writer, 0, "  (JASH_NUDGE=0 to silence)\033[0m\n");
}

/* A rule only fires if the builtin it advertises still exists in the
 * canonical differentiator registry (ADR-0040 W1: no drift, ever). */
static int registered(const char* builtin_name) {
    return jash_diff_find(builtin_name) != NULL;
}

/* ── rules — first match wins, ONE hint max per command ────────────────── */

void jash_hint_post_exec(const char* name, const char* args, int rc,
                         jash_writer_t w, void* ctx) {
    (void)w; (void)ctx;
    if (!hints_enabled()) return;
    int cap = hint_budget();
    if (cap == 0 || budget_used() >= cap) return;
    const char* n = name ? name : "";
    const char* a = args ? args : "";

    /* 1. absorb: an external `bash|sh|zsh -c` actually ran un-absorbed —
     *    the agent just left the exocortex for one command. */
    if ((!strcmp(n, "bash") || !strcmp(n, "sh") || !strcmp(n, "zsh"))
        && strstr(a, "-c") && jash_adopt_level() < 2
        && registered("adopt")) {
        if (jash_nudge_due("absorb", HINT_COOLDOWN_MOMENT)) {
            emit("that `-c` one-liner ran OUTSIDE jash (no recording/undo). "
                 "`adopt 2` absorbs absorbable bash/sh -c into jash; "
                 "JASH_ABSORB=0 opts out");
            budget_bump();
        }
        return;
    }

    /* 2. recall: digging through history the bash way. */
    if ((!strcmp(n, "history") || (!strcmp(n, "grep") && strstr(a, "history")))
        && registered("recall")) {
        if (jash_nudge_due("recallhist", HINT_COOLDOWN_MOMENT)) {
            emit("`recall did \"X\"` / `recall failed` query attested command "
                 "memory — no history|grep archaeology");
            budget_bump();
        }
        return;
    }

    /* 3. rewind: a destructive rm just succeeded — teach the undo NOW,
     *    while the agent still remembers what it deleted. */
    if (rc == 0 && !strcmp(n, "rm")
        && (strstr(a, "-r") || strstr(a, "-f"))
        && registered("rewind")) {
        if (jash_nudge_due("rewind", HINT_COOLDOWN_MOMENT)) {
            emit("that rm was auto-checkpointed — `rewind` or `undo` restores "
                 "it if it turns out you were wrong");
            budget_bump();
        }
        return;
    }

    /* 4. gap: exit 2 is the usage/misuse convention — the tool just said
     *    "I don't have that option/behavior". That is a capability-gap
     *    signal at the exact moment the agent feels the wall. */
    if (rc == 2 && registered("gap")) {
        if (jash_nudge_due("gap-misuse", HINT_COOLDOWN_FAILED)) {
            char cmd[200];
            if (a[0]) snprintf(cmd, sizeof cmd, "%.40s %.150s", n, a);
            else      snprintf(cmd, sizeof cmd, "%.190s", n);
            char msg[512];
            snprintf(msg, sizeof msg,
                "exit 2 = usage/misuse — if an option or behavior is missing, "
                "`gap \"%s\" --expected \"what should happen\"` reports it to "
                "the backlog", cmd);
            emit(msg);
            budget_bump();
        }
        return;
    }

    /* 5. failure tip (the original nudge, now budget-capped too). */
    if (rc != 0 && registered("recall")) {
        if (jash_nudge_due("failed", HINT_COOLDOWN_FAILED)) {
            emit("`recall failed` reviews recent failures · `expect` makes a "
                 "wrong assumption stop loudly");
            budget_bump();
        }
    }
}
