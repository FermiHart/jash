// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_predict — Failure Prediction (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   predict "command"          predict if command will fail
//   risk assess                show overall risk profile
//   risk avoid <pattern>       add pattern to avoid list

#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <liquid/jash_liquid.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define RISK_FILE "~/.jash/risk_patterns.yaml"
#define RISK_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void risk_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/risk_patterns.yaml", home);
}

static void ensure_dir(void) {
    char path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(path, sizeof(path), "%s/.jash", home);
    mkdir(path, 0755);
}

static int predict_yaml_file(const char* name) {
    size_t n = name ? strlen(name) : 0;
    return n > 5 && strcmp(name + n - 5, ".yaml") == 0;
}

static int predict_file_has_risk_pattern(const char* path, const char* cmd) {
    FILE* pf = fopen(path, "r");
    if (!pf) return 0;
    char pline[256];
    int found = 0;
    while (fgets(pline, sizeof(pline), pf)) {
        char* pat = strstr(pline, "pattern:");
        if (!pat) continue;
        pat += 8;
        while (*pat == ' ' || *pat == '"') pat++;
        char* end = strchr(pat, '"');
        if (end) *end = '\0';
        end = strchr(pat, '\n');
        if (end) *end = '\0';
        if (*pat && strstr(cmd, pat)) { found = 1; break; }
    }
    fclose(pf);
    return found;
}

static int risk_fixed_score(const char* cmd) {
    int risk = 0;

    // High risk patterns
    if (strstr(cmd, "rm -rf")) risk += 80;
    if (strstr(cmd, "rm -r")) risk += 60;
    if (strstr(cmd, "rm ")) risk += 40;
    if (strstr(cmd, "chmod 777")) risk += 70;
    if (strstr(cmd, "chmod -R")) risk += 50;
    if (strstr(cmd, "chown")) risk += 30;
    if (strstr(cmd, "kill -9")) risk += 60;
    if (strstr(cmd, "killall")) risk += 50;
    if (strstr(cmd, "shutdown")) risk += 90;
    if (strstr(cmd, "reboot")) risk += 90;
    if (strstr(cmd, "mkfs")) risk += 95;
    if (strstr(cmd, "dd if=")) risk += 85;
    if (strstr(cmd, "> /dev/")) risk += 80;
    if (strstr(cmd, "curl | sh")) risk += 90;
    if (strstr(cmd, "wget | sh")) risk += 90;
    if (strstr(cmd, "eval ")) risk += 40;
    if (strstr(cmd, "exec ")) risk += 30;

    // Medium risk patterns
    if (strstr(cmd, "git push --force")) risk += 60;
    if (strstr(cmd, "git reset --hard")) risk += 70;
    if (strstr(cmd, "git clean -f")) risk += 50;
    if (strstr(cmd, "npm install -g")) risk += 20;
    if (strstr(cmd, "pip install -U")) risk += 20;
    if (strstr(cmd, "cmake --build")) risk += 10;
    if (strstr(cmd, "make -j")) risk += 15;

    // Low risk patterns
    if (strstr(cmd, "ls")) risk += 5;
    if (strstr(cmd, "pwd")) risk += 2;
    if (strstr(cmd, "echo")) risk += 2;
    if (strstr(cmd, "cat")) risk += 5;
    if (strstr(cmd, "grep")) risk += 5;
    if (strstr(cmd, "find")) risk += 5;
    if (strstr(cmd, "git status")) risk += 3;
    if (strstr(cmd, "git log")) risk += 3;

    return risk;
}

static int calculate_risk(const char* cmd) {
    int risk = risk_fixed_score(cmd);

    // Check learned avoid patterns
    char avoid_path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(avoid_path, sizeof(avoid_path), "%s/.jash/risk_patterns.yaml", home);

    FILE* f = fopen(avoid_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // Simple YAML parsing: look for "pattern: ..." lines
            char* pat = strstr(line, "pattern:");
            if (pat) {
                pat += 8; // skip "pattern:"
                while (*pat == ' ' || *pat == '"') pat++;
                char* end = strchr(pat, '"');
                if (end) *end = '\0';
                end = strchr(pat, '\n');
                if (end) *end = '\0';
                if (strstr(cmd, pat)) {
                    risk += 25; // Additional risk for avoided patterns
                }
            }
        }
        fclose(f);
    }

    // Check learned patterns from learn command without turning paths into
    // shell syntax: HOME is data and may legitimately contain metacharacters.
    char patterns_dir[512];
    snprintf(patterns_dir, sizeof(patterns_dir), "%s/.jash/patterns", home);
    DIR* pdir = opendir(patterns_dir);
    if (pdir) {
        struct dirent* ent;
        while ((ent = readdir(pdir)) != NULL) {
            if (!predict_yaml_file(ent->d_name)) continue;
            char path[1024];
            int n = snprintf(path, sizeof(path), "%s/%s", patterns_dir, ent->d_name);
            if (n < 0 || (size_t)n >= sizeof(path)) continue;
            if (predict_file_has_risk_pattern(path, cmd))
                risk += 10; // Learn patterns add less risk but still relevant
        }
        closedir(pdir);
    }

    // Check mistake history for similar commands
    char mistake_path[512];
    snprintf(mistake_path, sizeof(mistake_path), "%s/.jash/mistakes.log", home);
    f = fopen(mistake_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            // Look for command references in mistake descriptions
            if (strstr(line, cmd) || strstr(cmd, line)) {
                risk += 15; // Increase risk if this command caused mistakes before
            }
        }
        fclose(f);
    }

    // Cap at 100
    if (risk > 100) risk = 100;

    return risk;
}

static const char* risk_label(int risk) {
    if (risk >= 80) return "CRITICAL";
    if (risk >= 60) return "HIGH";
    if (risk >= 40) return "MEDIUM";
    if (risk >= 20) return "LOW";
    return "MINIMAL";
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_predict(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: predict \"command\"\n");
        return 1;
    }

    int risk = calculate_risk(args);

    jash_writef(w, ctx, "%sPredict:%s %s\n", J_ROLE_ACCENT, J_RESET, args);
    jash_writef(w, ctx, "  Risk: %s%d%%%s\n",
                risk >= 60 ? J_ROLE_ERR : (risk >= 40 ? J_ROLE_WARN : J_ROLE_OK),
                risk, J_RESET);
    jash_writef(w, ctx, "  Level: %s\n", risk_label(risk));

    if (risk >= 60) {
        jash_writef(w, ctx, "  %s⚠ WARNING: High risk command!%s\n", J_ROLE_ERR, J_RESET);

        // Specific suggestions based on command type
        if (strstr(args, "rm")) {
            jash_writef(w, ctx, "  → Use `trash` instead of `rm` for recoverable deletion\n");
            jash_writef(w, ctx, "  → Use `ls` first to verify what will be deleted\n");
        } else if (strstr(args, "git push")) {
            jash_writef(w, ctx, "  → Use `git log --oneline -5` to review commits first\n");
            jash_writef(w, ctx, "  → Use `git diff` to verify changes\n");
        } else if (strstr(args, "chmod") || strstr(args, "chown")) {
            jash_writef(w, ctx, "  → Use `ls -la` to check current permissions first\n");
            jash_writef(w, ctx, "  → Apply minimum necessary permissions\n");
        } else if (strstr(args, "kill")) {
            jash_writef(w, ctx, "  → Use `ps aux | grep <name>` to verify process\n");
            jash_writef(w, ctx, "  → Try `kill <pid>` without -9 first\n");
        } else {
            jash_writef(w, ctx, "  → Consider using `change verify` before executing\n");
            jash_writef(w, ctx, "  → Use `explain \"%s\"` to see blast radius\n", args);
        }
    } else if (risk >= 40) {
        jash_writef(w, ctx, "  %s⚠ CAUTION: Moderate risk — review before executing%s\n", J_ROLE_WARN, J_RESET);
        if (strstr(args, "git")) {
            jash_writef(w, ctx, "  → Use `git status` to check state first\n");
        }
    }

    return 0;
}

static int predict_history_high_risk_count(void) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) return 0;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/.bash_history", home);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "rm -rf") || strstr(line, "rm -r") ||
            strstr(line, "chmod 777") || strstr(line, "kill -9"))
            count++;
    }
    fclose(f);
    return count;
}

static int cmd_risk_assess(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx, "%sRisk profile:%s\n", J_ROLE_ACCENT, J_RESET);

    int high_risk_count = predict_history_high_risk_count();

    jash_writef(w, ctx, "  High-risk commands in history: %d\n", high_risk_count);

    if (high_risk_count > 10) {
        jash_writef(w, ctx, "  %s⚠ WARNING: Excessive high-risk commands detected%s\n", J_ROLE_ERR, J_RESET);
    } else if (high_risk_count > 5) {
        jash_writef(w, ctx, "  %s⚠ CAUTION: Moderate high-risk commands detected%s\n", J_ROLE_WARN, J_RESET);
    } else {
        jash_writef(w, ctx, "  %s✓ Normal risk profile%s\n", J_ROLE_OK, J_RESET);
    }

    return 0;
}

static int cmd_risk_avoid(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: risk avoid <pattern>\n");
        return 1;
    }

    ensure_dir();

    char path[512];
    risk_path(path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (!f) {
        jash_writef(w, ctx, "error: cannot open %s\n", path);
        return 1;
    }

    fprintf(f, "- pattern: \"%s\"\n", args);
    fclose(f);

    jash_writef(w, ctx, "✓ pattern added to avoid list: %s\n", args);
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

#include "predict_semantic.inc"

int jash_b_predict(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "predict", "jash.predict.v1", args, predict_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%spredict%s — failure prediction (ADR-0034)\n\n"
            "Commands:\n"
            "  predict \"command\"          predict if command will fail\n"
            "  risk assess                show overall risk profile\n"
            "  risk avoid <pattern>       add pattern to avoid list\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "risk") == 0) {
        char sub2[64], rest2[1024];
        rest2[0] = '\0';
        n = sscanf(rest, "%63s %1023[^\n]", sub2, rest2);
        (void)n;

        if (strcmp(sub2, "assess") == 0) return cmd_risk_assess(w, ctx);
        if (strcmp(sub2, "avoid") == 0) return cmd_risk_avoid(w, ctx, rest2);
    }

    // Default: treat as "predict <command>"
    return cmd_predict(w, ctx, args);

    jash_writef(w, ctx, "%serror:%s unknown predict subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

// risk is a separate builtin that delegates to predict
int jash_b_risk(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "risk", "jash.risk.v1", args, predict_semantic);
    if (selected >= 0) return selected;
    (void)s;
    // Prepend "risk " to args and call predict
    char full_args[1024];
    if (args && *args) {
        snprintf(full_args, sizeof(full_args), "risk %s", args);
    } else {
        snprintf(full_args, sizeof(full_args), "risk");
    }
    return jash_b_predict(s, w, ctx, full_args);
}

SEMANTIC_ARGV_WRAPPER(predict, "predict", "jash.predict.v1", predict_semantic)
SEMANTIC_ARGV_WRAPPER(risk, "risk", "jash.risk.v1", predict_semantic)
