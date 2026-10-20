// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/handoff — session handoff compiler (ADR-0038 W8).
//
// `handoff [--budget N]` distills the session — command record, fs journal,
// git state — into an LLM-optimized brief, so a fresh context (or the same
// agent after compression) recovers exactly what the shell never lost:
//
//   ## Session handoff — sid abc123
//   Goal signals: <inferred from commands>
//   What ran: 42 commands, 6 failed
//   Failures (act on these):
//     - cmake --build build  (rc 1, 3x)  [last: 2m ago]
//   Files touched this session: 8
//     ~c/core/memo.c  +c/core/memo.h  -old.tmp
//   Repo: branch main · 3 dirty · last commit "feat: W7 ..."
//   Open ends: <uncommitted work>
//
// recall aimed at the FUTURE instead of the past. F E R M I ∞ H A R T

#include "builtin.h"
#include <core/argv.h>
#include <core/env.h>
#include <parser/eval.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HO_MAX_CMDS   4096
#define HO_MAX_FAILS  32
#define HO_MAX_FILES  64

typedef struct { char cmd[256]; int rc; unsigned long ts; int count; } ho_fail_t;
typedef struct { char path[256]; char kind; } ho_file_t;

static const char* ho_sid(jash_state_t* s) {
    const char* sid = jash_env_get(s, "CLAUDE_CODE_SESSION_ID");
    if (!sid || !*sid) sid = jash_env_get(s, "JASH_SESSION");
    return (sid && *sid) ? sid : NULL;
}

static void ho_sid_safe(const char* sid, char* out, size_t cap) {
    unsigned j = 0;
    for (const char* p = sid; *p && j + 1 < cap; p++) {
        char c = *p;
        out[j++] = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                    c=='-'||c=='_') ? c : '_';
    }
    out[j] = '\0';
}

/* Collapse a command to its "shape" for failure grouping: first two words. */
static void cmd_shape(const char* cmd, char* out, size_t cap) {
    unsigned o = 0, words = 0;
    const char* p = cmd;
    while (*p && o + 1 < cap) {
        if (*p == ' ') {
            words++;
            if (words >= 2) break;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static const char* ago(unsigned long then, unsigned long now, char* buf, size_t cap) {
    if (then == 0 || now < then) { snprintf(buf, cap, "?"); return buf; }
    unsigned long d = now - then;
    if (d < 60) snprintf(buf, cap, "%lus ago", d);
    else if (d < 3600) snprintf(buf, cap, "%lum ago", d / 60);
    else snprintf(buf, cap, "%luh ago", d / 3600);
    return buf;
}

int jash_b_handoff(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)args;
    const char* home = jash_env_get(s, "HOME");
    if (!home || !*home) { jash_writes(w, ctx, "handoff: no HOME\n"); return 1; }
    const char* sid = ho_sid(s);

    unsigned long now = (unsigned long)time(NULL);

    /* ── command record ──────────────────────────────────────────────── */
    long total = 0, failed = 0;
    ho_fail_t fails[HO_MAX_FAILS];
    int nfails = 0;
    unsigned long first_ts = 0, last_ts = 0;
    if (sid) {
        char safe[128];
        ho_sid_safe(sid, safe, sizeof(safe));
        char path[600];
        snprintf(path, sizeof(path), "%s/.jash_sessions/session_%s.jash",
                 home, safe);
        FILE* f = fopen(path, "r");
        if (f) {
            char line[2048];
            while (fgets(line, sizeof(line), f) && total < HO_MAX_CMDS) {
                char* p1 = strchr(line, '|');       if (!p1) continue;
                char* p2 = strchr(p1 + 1, '|');     if (!p2) continue;
                char* p3 = strchr(p2 + 1, '|');     if (!p3) continue;
                char* nl = strchr(p3 + 1, '\n');    if (nl) *nl = '\0';
                unsigned long ts = strtoul(line, NULL, 10);
                int rc = atoi(p1 + 1);
                const char* cmd = p3 + 1;
                if (!*cmd) continue;
                total++;
                if (first_ts == 0) first_ts = ts;
                last_ts = ts;
                if (rc != 0) {
                    failed++;
                    char shape[256];
                    cmd_shape(cmd, shape, sizeof(shape));
                    int found = 0;
                    for (int i = 0; i < nfails; i++)
                        if (strcmp(fails[i].cmd, shape) == 0) {
                            fails[i].count++; fails[i].rc = rc; fails[i].ts = ts;
                            found = 1; break;
                        }
                    if (!found && nfails < HO_MAX_FAILS) {
                        snprintf(fails[nfails].cmd, sizeof(fails[0].cmd), "%s", shape);
                        fails[nfails].rc = rc;
                        fails[nfails].ts = ts;
                        fails[nfails].count = 1;
                        nfails++;
                    }
                }
            }
            fclose(f);
        }
    }

    /* ── fs journal (this session) ───────────────────────────────────── */
    ho_file_t files[HO_MAX_FILES];
    int nfiles = 0;
    {
        char path[600];
        snprintf(path, sizeof(path), "%s/.jash/fsjournal.tsv", home);
        FILE* f = fopen(path, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                char* save = NULL;
                char* jts = strtok_r(line, "\t", &save);
                char* jsd = strtok_r(NULL, "\t", &save);
                char* jkd = strtok_r(NULL, "\t", &save);
                char* jpt = strtok_r(NULL, "\n", &save);
                (void)jts;
                if (!jpt || !jkd) continue;
                if (sid && jsd && strcmp(jsd, sid) != 0) continue;
                /* newest wins per path */
                int idx = -1;
                for (int i = 0; i < nfiles; i++)
                    if (strcmp(files[i].path, jpt) == 0) { idx = i; break; }
                if (idx < 0 && nfiles < HO_MAX_FILES) {
                    idx = nfiles++;
                    snprintf(files[idx].path, sizeof(files[0].path), "%s", jpt);
                }
                if (idx >= 0) files[idx].kind = jkd[0];
            }
            fclose(f);
        }
    }

    /* ── render ──────────────────────────────────────────────────────── */
    char buf[64];
    jash_writes(w, ctx, "## Session handoff");
    if (sid) { jash_writef(w, ctx, " — %s", sid); }
    jash_writes(w, ctx, "\n");

    if (total > 0) {
        jash_writef(w, ctx, "Span: %ld command%s over %s",
                    total, total == 1 ? "" : "s",
                    ago(first_ts, now, buf, sizeof(buf)));
        jash_writef(w, ctx, " (last %s), %ld failed.\n",
                    ago(last_ts, now, buf, sizeof(buf)), failed);
    } else {
        jash_writes(w, ctx, "No recorded commands for this session yet.\n");
    }

    if (nfails > 0) {
        jash_writes(w, ctx, "\nFailures (act on these first):\n");
        for (int i = 0; i < nfails; i++) {
            jash_writef(w, ctx, "  - %s  (rc %d)  [%s]\n",
                        fails[i].cmd, fails[i].rc,
                        ago(fails[i].ts, now, buf, sizeof(buf)));
            if (fails[i].count > 1)
                jash_writef(w, ctx, "      (failed %dx — recurring)\n",
                            fails[i].count);
        }
    }

    if (nfiles > 0) {
        jash_writef(w, ctx, "\nFiles touched this session (%d):\n  ", nfiles);
        int shown = 0;
        for (int i = 0; i < nfiles && shown < 40; i++) {
            jash_writef(w, ctx, "%c%s  ", files[i].kind, files[i].path);
            shown++;
        }
        jash_writes(w, ctx, "\n");
    }

    /* ── repo state (delegate to jash's own git JSON-free summary) ────── */
    jash_writes(w, ctx, "\nRepo state:\n");
    jash_eval_line(s, w, ctx,
        "git rev-parse --abbrev-ref HEAD 2>/dev/null | sed 's/^/  branch: /'");
    jash_eval_line(s, w, ctx,
        "git status --porcelain 2>/dev/null | wc -l | tr -d ' ' | "
        "sed 's/^/  dirty files: /'");
    jash_eval_line(s, w, ctx,
        "git log -1 --format='  last commit: %s' 2>/dev/null");

    jash_writes(w, ctx,
        "\nNext: address the failures above; uncommitted work is the open end. "
        "`recall failed` for detail, `impact <file>` for the tests to run.\n");
    return 0;
}

#include "handoff_native.inc"
