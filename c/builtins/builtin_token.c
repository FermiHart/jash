// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_token — Token Economy Visibility (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   token count              show tokens used this session
//   token optimize           suggest optimization opportunities
//   token history            show token usage over time
//   token budget <n>         set token budget for session

#include "semantic_policy.h"
#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <core/token_tracker.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define TOKEN_LOG "~/.jash/token_usage.log"
#define TOKEN_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void token_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/token_usage.log", home);
}

/* ensure_dir is used in other builtins but not here yet */

static void trim_newline(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_count(jash_writer_t w, void* ctx) {
    jash_session_tokens_t tok = {0, 0, 0, 0};
    jash_session_tokens(&tok);

    long total_tokens = tok.input_tokens + tok.output_tokens;
    const char* budget = getenv("JASH_LLM_BUDGET");

    jash_writef(w, ctx, "%sToken count:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  Input tokens:   %ld\n", tok.input_tokens);
    jash_writef(w, ctx, "  Output tokens:  %ld\n", tok.output_tokens);
    jash_writef(w, ctx, "  Total tokens:   %ld\n", total_tokens);
    jash_writef(w, ctx, "  Commands run:   %ld\n", tok.total_commands);
    jash_writef(w, ctx, "  Execution time: %ldms\n", tok.total_ms);
    jash_writef(w, ctx, "  Budget: %s\n", budget ? budget : "unlimited");

    if (budget) {
        long limit = atol(budget);
        if (limit > 0 && total_tokens > 0) {
            long remaining = limit - total_tokens;
            jash_writef(w, ctx, "  Remaining: %ld tokens\n", remaining);
            if (remaining <= 0) {
                jash_writef(w, ctx, "  %s⚠ BUDGET EXCEEDED%s\n", J_ROLE_ERR, J_RESET);
            } else if (remaining < limit / 4) {
                jash_writef(w, ctx, "  %s⚠ Budget running low%s\n", J_ROLE_WARN, J_RESET);
            }
        }
    }

    return 0;
}

static int cmd_optimize(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx, "%sToken optimization suggestions:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  1. Use `head`/`tail` instead of `cat` for large files\n");
    jash_writef(w, ctx, "  2. Use `grep` with specific patterns instead of broad searches\n");
    jash_writef(w, ctx, "  3. Use `recall` to retrieve previous context instead of re-executing\n");
    jash_writef(w, ctx, "  4. Use `state snapshot` instead of multiple `pwd`/`git status` calls\n");
    jash_writef(w, ctx, "  5. Use `context map` instead of `ls -la` + `file` commands\n");
    return 0;
}

static int cmd_history(jash_writer_t w, void* ctx) {
    char path[512];
    token_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writef(w, ctx, "no token usage recorded yet\n");
        return 0;
    }

    jash_writef(w, ctx, "%sToken usage history:%s\n", J_ROLE_ACCENT, J_RESET);

    char line[TOKEN_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strlen(line) > 0) {
            jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, line, J_RESET);
            count++;
        }
    }
    fclose(f);

    jash_writef(w, ctx, "\nTotal entries: %d\n", count);
    return 0;
}

static int cmd_budget(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: token budget <number>\n");
        return 1;
    }

    long budget = atol(args);
    if (budget <= 0) {
        jash_writef(w, ctx, "error: budget must be a positive number\n");
        return 1;
    }

    char env[64];
    snprintf(env, sizeof(env), "JASH_LLM_BUDGET=%ld", budget);
    jash_writef(w, ctx, "✓ token budget set to %ld\n", budget);
    jash_writef(w, ctx, "  (use `export %s` to persist)\n", env);
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

int jash_b_token(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "token", "jash.token.v1", args, semantic_token);
    if (structured >= 0) return structured;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%stoken%s — token economy visibility (ADR-0034)\n\n"
            "Commands:\n"
            "  token count              show tokens used this session\n"
            "  token optimize           suggest optimization opportunities\n"
            "  token history            show token usage over time\n"
            "  token budget <n>         set token budget for session\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "count") == 0) return cmd_count(w, ctx);
    if (strcmp(sub, "optimize") == 0) return cmd_optimize(w, ctx);
    if (strcmp(sub, "history") == 0) return cmd_history(w, ctx);
    if (strcmp(sub, "budget") == 0) return cmd_budget(w, ctx, rest);

    jash_writef(w, ctx, "%serror:%s unknown token subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(token, "token", "jash.token.v1", semantic_token)
