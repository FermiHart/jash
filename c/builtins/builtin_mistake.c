// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_mistake — Memory of Own Errors (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   mistake log "error" [--context "ctx"]   log a mistake
//   mistake avoid                           show patterns to avoid
//   mistake list                            list all logged mistakes
//   mistake clear                           clear mistake log
//
// J1 fix: use jash_tmpdir() from sys/syscalls.h instead of hardcoded "/tmp"

#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define MISTAKE_LOG "~/.jash/mistakes.log"
#define MISTAKE_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void mistake_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/mistakes.log", home);
}

static void ensure_dir(void) {
    char path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(path, sizeof(path), "%s/.jash", home);
    mkdir(path, 0755);
}

/* mistake_log_exists is used in other builtins but not here yet */

static void trim_newline(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_log(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: mistake log \"error\" [--context \"ctx\"]\n");
        return 1;
    }

    ensure_dir();

    char path[512];
    mistake_path(path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (!f) {
        jash_writef(w, ctx, "error: cannot open %s\n", path);
        return 1;
    }

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    // Parse error and optional context
    char error[MISTAKE_MAX] = {0};
    char context[MISTAKE_MAX] = {0};

    // Skip leading quote
    const char* p = args;
    if (*p == '"') p++;

    // Extract error until closing quote or --context or end
    int i = 0;
    while (*p && *p != '"' && strncmp(p, " --context", 10) != 0 && i < MISTAKE_MAX - 1) {
        error[i++] = *p++;
    }
    error[i] = '\0';
    if (*p == '"') p++;

    // Look for --context
    while (*p == ' ') p++;
    if (strncmp(p, "--context", 9) == 0) {
        p += 9;
        while (*p == ' ') p++;
        if (*p == '"') p++;
        i = 0;
        while (*p && *p != '"' && i < MISTAKE_MAX - 1) {
            context[i++] = *p++;
        }
        context[i] = '\0';
    }

    fprintf(f, "%s | %s | %s\n", timestamp, error, context);
    fclose(f);

    jash_writef(w, ctx, "✓ mistake logged: %s\n", error);
    return 0;
}

static int cmd_avoid(jash_writer_t w, void* ctx) {
    char path[512];
    mistake_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writef(w, ctx, "no mistakes logged yet\n");
        return 0;
    }

    jash_writef(w, ctx, "%sPatterns to avoid:%s\n", J_ROLE_ACCENT, J_RESET);

    char line[MISTAKE_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strlen(line) > 0) {
            jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_WARN, line, J_RESET);
            count++;
        }
    }
    fclose(f);

    if (count == 0) {
        jash_writef(w, ctx, "  (none)\n");
    }
    return 0;
}

static int cmd_list(jash_writer_t w, void* ctx) {
    char path[512];
    mistake_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writef(w, ctx, "no mistakes logged yet\n");
        return 0;
    }

    jash_writef(w, ctx, "%sMistake log:%s\n", J_ROLE_ACCENT, J_RESET);

    char line[MISTAKE_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strlen(line) > 0) {
            jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, line, J_RESET);
            count++;
        }
    }
    fclose(f);

    jash_writef(w, ctx, "\nTotal: %d mistakes\n", count);
    return 0;
}

static int cmd_clear(jash_writer_t w, void* ctx) {
    char path[512];
    mistake_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) {
        jash_writef(w, ctx, "error: cannot open %s\n", path);
        return 1;
    }
    fclose(f);
    jash_writef(w, ctx, "✓ mistake log cleared\n");
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

#include "mistake_semantic.inc"

int jash_b_mistake(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "mistake", "jash.mistake.v1", args, mistake_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%smistake%s — memory of own errors (ADR-0034)\n\n"
            "Commands:\n"
            "  mistake log \"error\" [--context \"ctx\"]   log a mistake\n"
            "  mistake avoid                           show patterns to avoid\n"
            "  mistake list                            list all logged mistakes\n"
            "  mistake clear                           clear mistake log\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "log") == 0) return cmd_log(w, ctx, rest);
    if (strcmp(sub, "avoid") == 0) return cmd_avoid(w, ctx);
    if (strcmp(sub, "list") == 0) return cmd_list(w, ctx);
    if (strcmp(sub, "clear") == 0) return cmd_clear(w, ctx);

    jash_writef(w, ctx, "%serror:%s unknown mistake subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(mistake, "mistake", "jash.mistake.v1", mistake_semantic)
