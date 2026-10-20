// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/cost — estimate command cost before/after running (ADR-0026 Tier 2).
//
// `cost <command>` runs the command, captures its combined stdout+stderr,
// and reports: wall time, output bytes, estimated tokens (bytes/4), and a
// conservative USD estimate assuming GPT-4-class pricing (~$30/M input tokens
// + $60/M output tokens; output here is treated as both for a worst-case bound).
//
// Flags: --json, --dry-run (estimate without executing).

#include "builtin.h"
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define COST_BUF_SIZE (1 << 20)   /* 1 MiB capture cap */

static int has_arg(int argc, char** argv, const char* f) {
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], f)) return 1;
    return 0;
}

static int cost_run_capture(char* const argv[], long* bytes_out, int* rc_out) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);

    char buf[8192];
    long total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) total += n;
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    *rc_out = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    *bytes_out = total;
    return 0;
}

static void cost_estimate(long bytes, double secs, jash_writer_t w, void* ctx, int json) {
    long tokens = bytes / 4;                  /* rough byte-per-token heuristic */
    double input_cost  = tokens * 30.0 / 1e6; /* $30/M tokens input  */
    double output_cost = tokens * 60.0 / 1e6; /* $60/M tokens output */
    double total_usd   = input_cost + output_cost;

    if (json) {
        jash_writef(w, ctx, "{\"bytes\":%ld,\"tokens_estimated\":%ld,\"seconds\":%.4f,"
                            "\"input_usd\":%.6f,\"output_usd\":%.6f,\"total_usd\":%.6f}\n",
                    bytes, tokens, secs, input_cost, output_cost, total_usd);
    } else {
        jash_writes(w, ctx, "\033[1;36m═══ cost estimate ═══\033[0m\r\n");
        jash_writef(w, ctx, "  output bytes        %ld\r\n", bytes);
        jash_writef(w, ctx, "  tokens (est.)       %ld\r\n", tokens);
        jash_writef(w, ctx, "  runtime             %.4fs\r\n", secs);
        jash_writef(w, ctx, "  cost (GPT-4 class)  $%.6f total\r\n", total_usd);
    }
}

#include "cost_semantic.inc"

int jash_b_cost(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "cost", "jash.cost.v1", args, cost_semantic);
    if (selected >= 0) return selected;
    (void)s;
    char* tokv[256];
    int tokc = 0;
    char* buf = args ? strdup(args) : strdup("");
    if (!buf) return 1;

    char* p = buf;
    char* t;
    while ((t = strsep(&p, " \t")) != NULL && tokc < 255) {
        if (*t) tokv[tokc++] = t;
    }

    int json = has_arg(tokc, tokv, "--json");
    int dry  = has_arg(tokc, tokv, "--dry-run");

    int a0 = 0;
    while (a0 < tokc && (tokv[a0][0] == '-' || !tokv[a0][0])) a0++;
    if (a0 >= tokc) {
        free(buf);
        jash_writes(w, ctx,
            "cost — estimate command cost\n"
            "Usage: cost [--json] [--dry-run] <command> [args...]\n");
        return 1;
    }
    tokv[tokc] = NULL;

    char** cmd = &tokv[a0];

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

    long bytes = 0;
    int rc = 0;
    if (dry) {
        /* dry-run: estimate based on command name length as a silly lower bound */
        for (int i = 0; cmd[i]; i++) bytes += strlen(cmd[i]) + 1;
    } else {
        if (cost_run_capture(cmd, &bytes, &rc) < 0) {
            free(buf);
            jash_writes(w, ctx, "cost: failed to run command\n");
            return 1;
        }
    }

    gettimeofday(&t1, NULL);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
    if (dry) secs = 0.0;

    cost_estimate(bytes, secs, w, ctx, json);
    free(buf);
    return dry ? 0 : rc;
}

SEMANTIC_ARGV_WRAPPER(cost, "cost", "jash.cost.v1", cost_semantic)
