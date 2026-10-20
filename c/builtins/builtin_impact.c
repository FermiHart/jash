// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/impact — test impact map learned from the chain (ADR-0038 W5).
//
// `impact FILE...`        rank the test commands historically sensitive to
//                         these files (learned, not configured)
// `impact --run FILE...`  run the top-ranked tests (falls back to nothing
//                         but a clear message when the map is cold)
//
// Raw material: ~/.jash/fsjournal.tsv (W2 persisted deltas: ts sid kind path)
// correlated with the session command records (~/.jash_sessions/*.jash,
// ts|rc|dur|cmd). A test command that ran within the correlation window
// after a file changed earns an edge; a fail→pass transition in the window
// (the fix cycle) earns a stronger one. No other tool sits where this
// information is born. F E R M I ∞ H A R T

#include "builtin.h"
#include <core/argv.h>
#include <core/query_cancel.h>
#include <parser/eval.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMPACT_WINDOW_S   1800   /* file change → test run correlation window */
#define IMPACT_MAX_TESTS  64
#define IMPACT_MAX_EVENTS 8192
#define IMPACT_TOP        5

typedef struct {
    char cmd[512];
    long score;
    long runs;
    long fails;
} impact_test_t;

typedef struct {
    uint64_t ts;
    int rc;
    int test_idx;      /* into the tests table */
    unsigned source_order;
} test_run_t;

static int impact_run_compare(const void* left, const void* right) {
    const test_run_t *a = left, *b = right;
    if (a->ts != b->ts) return a->ts < b->ts ? -1 : 1;
    return a->source_order < b->source_order ? -1 : a->source_order > b->source_order;
}

/* Retained temporal association, not a dependency edge. The last failing
 * test keeps its historical bonus rule; chronological order makes the rule
 * independent of the order records were read. Subtraction avoids wrapping a
 * timestamp near UINT64_MAX. Native collectors establish bounded counts. */
static int impact_score_change(impact_test_t* tests, const test_run_t* runs,
        int count, uint64_t changed, jash_state_t* state) {
    int previous_failure = -1;
    for (int i = 0; i < count; i++) {
        if (state && (i % 1024) == 0 && jash_query_cancel_poll(state)) return -1;
        if (runs[i].ts < changed || runs[i].ts - changed > IMPACT_WINDOW_S) continue;
        tests[runs[i].test_idx].score += 2;
        if (runs[i].rc != 0) previous_failure = runs[i].test_idx;
        else if (previous_failure == runs[i].test_idx) tests[runs[i].test_idx].score += 5;
    }
    return 0;
}

/* A command is a "test command" when it plainly invokes a test runner. */
static int is_test_cmd(const char* cmd) {
    if (!cmd || !*cmd) return 0;
    if (strstr(cmd, "test")) {
        if (strncmp(cmd, "bash ", 5) == 0 || strncmp(cmd, "sh ", 3) == 0 ||
            strncmp(cmd, "./", 2) == 0 || strstr(cmd, "npm test") ||
            strstr(cmd, "pytest") || strstr(cmd, "ctest") ||
            strstr(cmd, "go test") || strstr(cmd, "cargo test"))
            return 1;
    }
    return 0;
}

static int test_intern(impact_test_t* tests, int* ntests, const char* cmd) {
    for (int i = 0; i < *ntests; i++)
        if (strcmp(tests[i].cmd, cmd) == 0) return i;
    if (*ntests >= IMPACT_MAX_TESTS) return -1;
    snprintf(tests[*ntests].cmd, sizeof(tests[0].cmd), "%s", cmd);
    tests[*ntests].score = 0;
    tests[*ntests].runs = 0;
    tests[*ntests].fails = 0;
    return (*ntests)++;
}

/* Load every recorded test run from every session record. */
static int load_test_runs(const char* home, impact_test_t* tests, int* ntests,
                          test_run_t* runs, int max_runs) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/.jash_sessions", home);
    DIR* d = opendir(dir);
    if (!d) return 0;
    int nr = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && nr < max_runs) {
        if (strncmp(de->d_name, "session_", 8) != 0) continue;
        char fp[800];
        snprintf(fp, sizeof(fp), "%s/%s", dir, de->d_name);
        FILE* f = fopen(fp, "r");
        if (!f) continue;
        char line[2048];
        while (fgets(line, sizeof(line), f) && nr < max_runs) {
            /* record.c format: ts|rc|duration_ns|cmd */
            char* p1 = strchr(line, '|');
            if (!p1) continue;
            char* p2 = strchr(p1 + 1, '|');
            if (!p2) continue;
            char* p3 = strchr(p2 + 1, '|');
            if (!p3) continue;
            char* nl = strchr(p3 + 1, '\n');
            if (nl) *nl = '\0';
            const char* cmd = p3 + 1;
            if (!is_test_cmd(cmd)) continue;
            int idx = test_intern(tests, ntests, cmd);
            if (idx < 0) continue;
            runs[nr].ts = strtoull(line, NULL, 10);
            runs[nr].rc = atoi(p1 + 1);
            runs[nr].test_idx = idx;
            runs[nr].source_order = (unsigned)nr;
            tests[idx].runs++;
            if (runs[nr].rc != 0) tests[idx].fails++;
            nr++;
        }
        fclose(f);
    }
    closedir(d);
    qsort(runs, (size_t)nr, sizeof(*runs), impact_run_compare);
    return nr;
}

/* Does journal path `jp` refer to queried file `q`? Journal paths are
 * root-relative; accept suffix matches so `impact c/core/memo.c` works
 * from anywhere. */
static int path_matches(const char* jp, const char* q) {
    if (strcmp(jp, q) == 0) return 1;
    size_t jl = strlen(jp), ql = strlen(q);
    if (ql < jl && strcmp(jp + (jl - ql), q) == 0 && jp[jl - ql - 1] == '/')
        return 1;
    if (jl < ql && strcmp(q + (ql - jl), jp) == 0 && q[ql - jl - 1] == '/')
        return 1;
    return 0;
}

int jash_b_impact(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* JINV-046: flag desconhecida nao pode virar nome/alvo. */
    { static const char* const ok_flags[] = { "--run", 0 };
      if (jash_reject_unknown_flag("impact", args, ok_flags, w, ctx))
          return 2; }
    int do_run = 0;
    const char* files[16];
    int nfiles = 0;

    jash_argv_t av;
    if (jash_argv_build(args ? args : "", &av) < 0) {
        jash_writes(w, ctx, "impact: bad arguments\n");
        return 2;
    }
    for (int i = 0; i < av.argc; i++) {
        if (strcmp(av.argv[i], "--run") == 0) { do_run = 1; continue; }
        if (nfiles < 16) files[nfiles++] = av.argv[i];
    }
    if (nfiles == 0) {
        jash_writes(w, ctx,
            "usage: impact [--run] FILE...\n"
            "  ranks the test commands historically sensitive to FILE\n"
            "  (learned from the fs journal + session records; --run executes them)\n");
        jash_argv_free(&av);
        return 1;
    }

    const char* home = getenv("HOME");
    if (!home || !*home) { jash_argv_free(&av); return 1; }

    /* BUG-065: these were `static` — 162KB (runs 128KB + tests 34KB) of
     * always-resident BSS, the single largest BSS consumer in the whole
     * binary — purely to keep them off the stack. Heap-allocate on demand:
     * `impact` is a rarely-run builtin, so those 162KB must not sit resident
     * in every jash process. Freed at both exits below. */
    impact_test_t* tests = (impact_test_t*)calloc(IMPACT_MAX_TESTS, sizeof(*tests));
    test_run_t*    runs  = (test_run_t*)calloc(IMPACT_MAX_EVENTS, sizeof(*runs));
    if (!tests || !runs) { free(tests); free(runs); jash_argv_free(&av); return 1; }
    int ntests = 0;
    int nruns = load_test_runs(home, tests, &ntests, runs, IMPACT_MAX_EVENTS);

    /* Walk the fs journal; every queried-file change scores the test runs
     * inside the window after it. */
    char jpath[600];
    snprintf(jpath, sizeof(jpath), "%s/.jash/fsjournal.tsv", home);
    FILE* jf = fopen(jpath, "r");
    long touches = 0;
    if (jf && nruns > 0) {
        char line[1024];
        while (fgets(line, sizeof(line), jf)) {
            char* save = NULL;
            char* fts  = strtok_r(line, "\t", &save);
            char* fsid = strtok_r(NULL, "\t", &save);
            char* fknd = strtok_r(NULL, "\t", &save);
            char* fpat = strtok_r(NULL, "\n", &save);
            (void)fsid; (void)fknd;
            if (!fts || !fpat) continue;
            int hit = 0;
            for (int i = 0; i < nfiles && !hit; i++)
                hit = path_matches(fpat, files[i]);
            if (!hit) continue;
            touches++;
            uint64_t cts = strtoull(fts, NULL, 10);
            (void)impact_score_change(tests, runs, nruns, cts, NULL);
        }
        fclose(jf);
    }
    if (jf == NULL && nruns == 0) { /* nothing at all */ }

    /* Rank. */
    int order[IMPACT_MAX_TESTS];
    for (int i = 0; i < ntests; i++) order[i] = i;
    for (int i = 0; i < ntests; i++)
        for (int j = i + 1; j < ntests; j++)
            if (tests[order[j]].score > tests[order[i]].score) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    int hot = 0;
    for (int i = 0; i < ntests; i++) if (tests[i].score > 0) hot++;

    if (hot == 0) {
        jash_writef(w, ctx,
            "impact: map is COLD for %s%s (%ld change record%s, %d known test cmd%s) — "
            "run the full suite: bash tests/test_suite.sh\n",
            files[0], nfiles > 1 ? " (+)" : "",
            touches, touches == 1 ? "" : "s",
            ntests, ntests == 1 ? "" : "s");
        jash_argv_free(&av);
        free(tests); free(runs);
        return 0;
    }

    jash_writef(w, ctx, "impact for %s%s — learned from %ld change record%s:\n",
                files[0], nfiles > 1 ? " (+)" : "",
                touches, touches == 1 ? "" : "s");
    int shown = 0;
    for (int i = 0; i < ntests && shown < IMPACT_TOP; i++) {
        impact_test_t* t = &tests[order[i]];
        if (t->score <= 0) break;
        jash_writef(w, ctx, "  %2d. [score %ld · %ld run%s, %ld fail%s] %s\n",
                    shown + 1, t->score,
                    t->runs, t->runs == 1 ? "" : "s",
                    t->fails, t->fails == 1 ? "" : "s",
                    t->cmd);
        shown++;
    }

    int rc = 0;
    if (do_run) {
        int ran = 0;
        for (int i = 0; i < ntests && ran < 3; i++) {
            impact_test_t* t = &tests[order[i]];
            if (t->score <= 0) break;
            jash_writef(w, ctx, "impact: running [%d/%d] %s\n",
                        ran + 1, hot < 3 ? hot : 3, t->cmd);
            rc = jash_eval_line(s, w, ctx, t->cmd);
            ran++;
            if (rc != 0) {
                jash_writef(w, ctx, "impact: STOPPED — test failed (rc=%d)\n", rc);
                break;
            }
        }
    }
    jash_argv_free(&av);
    free(tests); free(runs);
    return rc;
}

#include "impact_native.inc"
