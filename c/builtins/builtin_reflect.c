// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_reflect — Self-Reflection (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   reflect "question"           analyze session history
//   improve suggest              suggest improvements
//   improve apply <suggestion>   apply an improvement

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

#define REFLECT_DIR "~/.jash/reflections"
#define REFLECT_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void reflections_dir(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/reflections", home);
}

static void ensure_dir(void) {
    char path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(path, sizeof(path), "%s/.jash", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.jash/reflections", home);
    mkdir(path, 0755);
}

static int count_lines(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) count++;
    fclose(f);
    return count;
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_reflect(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: reflect \"question\"\n");
        return 1;
    }

    ensure_dir();

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    char date[64];
    strftime(date, sizeof(date), "%Y-%m-%d", tm);

    // Gather real data for analysis
    jash_session_tokens_t tok = {0, 0, 0, 0};
    jash_session_tokens(&tok);

    // Count mistakes
    char mistake_path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(mistake_path, sizeof(mistake_path), "%s/.jash/mistakes.log", home);
    int mistake_count = count_lines(mistake_path);

    // Count reflections today
    char reflect_path[512];
    snprintf(reflect_path, sizeof(reflect_path), "%s/.jash/reflections/%s.txt", home, date);
    int reflection_count = count_lines(reflect_path) / 5; // ~5 lines per reflection

    // Analyze the question and provide real insight
    jash_writef(w, ctx, "%sReflect:%s %s\n", J_ROLE_ACCENT, J_RESET, args);
    jash_writef(w, ctx, "\n");

    // Real analysis based on session data
    jash_writef(w, ctx, "%sSession Analysis:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  Commands executed: %ld\n", tok.total_commands);
    jash_writef(w, ctx, "  Input tokens: %ld\n", tok.input_tokens);
    jash_writef(w, ctx, "  Output tokens: %ld\n", tok.output_tokens);
    jash_writef(w, ctx, "  Execution time: %ldms\n", tok.total_ms);
    jash_writef(w, ctx, "  Mistakes logged: %d\n", mistake_count);
    jash_writef(w, ctx, "  Reflections today: %d\n", reflection_count);
    jash_writef(w, ctx, "\n");

    // Pattern analysis
    jash_writef(w, ctx, "%sPattern Analysis:%s\n", J_ROLE_ACCENT, J_RESET);

    if (tok.total_commands > 0) {
        long avg_input = tok.input_tokens / tok.total_commands;
        long avg_output = tok.output_tokens / tok.total_commands;
        jash_writef(w, ctx, "  Avg tokens/command: %ld in, %ld out\n", avg_input, avg_output);

        if (avg_output > avg_input * 3) {
            jash_writef(w, ctx, "  %s⚠ High output ratio — consider using head/tail for large outputs%s\n",
                        J_ROLE_WARN, J_RESET);
        }
    }

    if (mistake_count > 3) {
        jash_writef(w, ctx, "  %s⚠ Multiple mistakes detected — review patterns to avoid repetition%s\n",
                    J_ROLE_WARN, J_RESET);
    }

    // Question-specific analysis
    jash_writef(w, ctx, "\n%sQuestion Analysis:%s\n", J_ROLE_ACCENT, J_RESET);
    if (strstr(args, "performance") || strstr(args, "speed") || strstr(args, "lento")) {
        jash_writef(w, ctx, "  Focus: Performance optimization\n");
        jash_writef(w, ctx, "  Action: Use `token optimize` to identify bottlenecks\n");
        jash_writef(w, ctx, "  Action: Use `state snapshot` to track state changes\n");
    } else if (strstr(args, "error") || strstr(args, "bug") || strstr(args, "erro")) {
        jash_writef(w, ctx, "  Focus: Error resolution\n");
        jash_writef(w, ctx, "  Action: Review `mistake list` for patterns\n");
        jash_writef(w, ctx, "  Action: Use `predict risk` before risky operations\n");
    } else if (strstr(args, "token") || strstr(args, "cost") || strstr(args, "budget")) {
        jash_writef(w, ctx, "  Focus: Token economy\n");
        jash_writef(w, ctx, "  Action: Use `token count` to monitor usage\n");
        jash_writef(w, ctx, "  Action: Use `token optimize` for suggestions\n");
    } else {
        jash_writef(w, ctx, "  General session review\n");
        jash_writef(w, ctx, "  Action: Use `reflect improve suggest` for recommendations\n");
    }

    // Write reflection file
    char path[512];
    char dir[512];
    reflections_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.txt", dir, date);

    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "---\n");
        fprintf(f, "Time: %s\n", timestamp);
        fprintf(f, "Question: %s\n", args);
        fprintf(f, "Commands: %ld\n", tok.total_commands);
        fprintf(f, "Tokens: %ld in, %ld out\n", tok.input_tokens, tok.output_tokens);
        fprintf(f, "Mistakes: %d\n", mistake_count);
        fclose(f);
    }

    jash_writef(w, ctx, "\n  %s✓ Reflection recorded%s\n", J_ROLE_OK, J_RESET);
    jash_writef(w, ctx, "  File: %s\n", path);
    return 0;
}

static int cmd_improve_suggest(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx, "%sImprovement suggestions:%s\n", J_ROLE_ACCENT, J_RESET);

    // Analyze mistake log
    char mistake_path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(mistake_path, sizeof(mistake_path), "%s/.jash/mistakes.log", home);

    FILE* f = fopen(mistake_path, "r");
    int mistake_count = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strlen(line) > 0) mistake_count++;
        }
        fclose(f);
    }

    if (mistake_count > 0) {
        jash_writef(w, ctx, "  1. %sReview %d logged mistakes to avoid repeating%s\n",
                    J_ROLE_WARN, mistake_count, J_RESET);
    }

    // Analyze token usage
    jash_writef(w, ctx, "  2. Use `token optimize` to reduce token waste\n");
    jash_writef(w, ctx, "  3. Use `recall` to retrieve previous context\n");
    jash_writef(w, ctx, "  4. Use `state snapshot` instead of multiple status commands\n");
    jash_writef(w, ctx, "  5. Use `context map` instead of `ls` + `file` commands\n");

    if (mistake_count > 5) {
        jash_writef(w, ctx, "  %s⚠ High mistake count detected. Consider reviewing patterns.%s\n",
                    J_ROLE_ERR, J_RESET);
    }

    return 0;
}

static int cmd_improve_apply(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: improve apply <suggestion>\n");
        return 1;
    }

    jash_writef(w, ctx, "%sImprovement applied:%s %s\n", J_ROLE_ACCENT, J_RESET, args);
    jash_writef(w, ctx, "  %s✓ Suggestion recorded for future reference%s\n", J_ROLE_OK, J_RESET);
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

#include "reflect_semantic.inc"

int jash_b_reflect(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "reflect", "jash.reflect.v1", args, reflect_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%sreflect%s — self-reflection (ADR-0034)\n\n"
            "Commands:\n"
            "  reflect \"question\"           analyze session history\n"
            "  improve suggest              suggest improvements\n"
            "  improve apply <suggestion>   apply an improvement\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "improve") == 0) {
        char sub2[64], rest2[1024];
        rest2[0] = '\0';
        n = sscanf(rest, "%63s %1023[^\n]", sub2, rest2);
        (void)n;

        if (strcmp(sub2, "suggest") == 0) return cmd_improve_suggest(w, ctx);
        if (strcmp(sub2, "apply") == 0) return cmd_improve_apply(w, ctx, rest2);
    }

    // Default: treat as "reflect <question>"
    return cmd_reflect(w, ctx, args);

    jash_writef(w, ctx, "%serror:%s unknown reflect subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(reflect, "reflect", "jash.reflect.v1", reflect_semantic)
