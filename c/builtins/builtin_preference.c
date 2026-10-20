// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_preference — User Preference Learning (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   preference learn "preference" [--category "cat"]   learn a preference
//   preference apply                                   show all preferences
//   preference recall "query"                          recall preferences matching query
//   preference forget <id>                             remove a preference

#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define PREF_FILE "~/.jash/preferences.yaml"
#define PREF_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void pref_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/preferences.yaml", home);
}

static void ensure_dir(void) {
    char path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(path, sizeof(path), "%s/.jash", home);
    mkdir(path, 0755);
}

static void trim_newline(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_learn(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: preference learn \"preference\" [--category \"cat\"]\n");
        return 1;
    }

    ensure_dir();

    // Parse preference and optional category
    char preference[PREF_MAX] = {0};
    char category[256] = "general";

    const char* p = args;
    if (*p == '"') p++;

    // Extract preference until closing quote or --category or end
    int i = 0;
    while (*p && *p != '"' && strncmp(p, " --category", 11) != 0 && i < PREF_MAX - 1) {
        preference[i++] = *p++;
    }
    preference[i] = '\0';
    if (*p == '"') p++;

    // Look for --category
    while (*p == ' ') p++;
    if (strncmp(p, "--category", 10) == 0) {
        p += 10;
        while (*p == ' ') p++;
        if (*p == '"') p++;
        i = 0;
        while (*p && *p != '"' && i < 255) {
            category[i++] = *p++;
        }
        category[i] = '\0';
    }

    // Write preference file
    char path[512];
    pref_path(path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (!f) {
        jash_writef(w, ctx, "error: cannot open %s\n", path);
        return 1;
    }

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(f, "- category: \"%s\"\n", category);
    fprintf(f, "  preference: \"%s\"\n", preference);
    fprintf(f, "  learned: %s\n", timestamp);
    fprintf(f, "  confidence: 1.0\n");
    fprintf(f, "  last_used: never\n");
    fclose(f);

    jash_writef(w, ctx, "✓ preference learned (category: %s): %s\n", category, preference);
    return 0;
}

static int cmd_apply(jash_writer_t w, void* ctx) {
    char path[512];
    pref_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writef(w, ctx, "no preferences learned yet\n");
        return 0;
    }

    jash_writef(w, ctx, "%sApplying preferences:%s\n", J_ROLE_ACCENT, J_RESET);

    char line[PREF_MAX];
    int count = 0;
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strlen(line) > 0) {
            // Parse "preference: " lines
            char* pref = strstr(line, "preference: ");
            if (pref) {
                pref += 12; // skip "preference: "
                while (*pref == ' ' || *pref == '"') pref++;
                char* end = strchr(pref, '"');
                if (end) *end = '\0';

                // Try to apply the preference
                int applied_this = 0;

                // Check for "key: value" format
                char* colon = strchr(pref, ':');
                if (colon) {
                    *colon = '\0';
                    char* key = pref;
                    char* value = colon + 1;
                    while (*value == ' ') value++;

                    // Apply known preferences
                    if (strcmp(key, "output") == 0 || strcmp(key, "output-format") == 0) {
                        setenv("JASH_OUTPUT_FORMAT", value, 1);
                        jash_writef(w, ctx, "  %s✓ output-format = %s%s\n", J_ROLE_OK, value, J_RESET);
                        applied_this = 1;
                    } else if (strcmp(key, "color") == 0) {
                        if (strcmp(value, "always") == 0 || strcmp(value, "on") == 0) {
                            setenv("JASH_COLOR", "1", 1);
                        } else if (strcmp(value, "never") == 0 || strcmp(value, "off") == 0) {
                            setenv("JASH_COLOR", "0", 1);
                        }
                        jash_writef(w, ctx, "  %s✓ color = %s%s\n", J_ROLE_OK, value, J_RESET);
                        applied_this = 1;
                    } else if (strcmp(key, "verbose") == 0) {
                        setenv("JASH_VERBOSE", value, 1);
                        jash_writef(w, ctx, "  %s✓ verbose = %s%s\n", J_ROLE_OK, value, J_RESET);
                        applied_this = 1;
                    } else if (strcmp(key, "safe") == 0) {
                        if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0) {
                            setenv("JASH_SAFE", "1", 1);
                        } else {
                            setenv("JASH_SAFE", "0", 1);
                        }
                        jash_writef(w, ctx, "  %s✓ safe = %s%s\n", J_ROLE_OK, value, J_RESET);
                        applied_this = 1;
                    } else if (strcmp(key, "trash") == 0) {
                        if (strcmp(value, "on") == 0 || strcmp(value, "true") == 0) {
                            setenv("JASH_TRASH", "1", 1);
                        } else {
                            setenv("JASH_TRASH", "0", 1);
                        }
                        jash_writef(w, ctx, "  %s✓ trash = %s%s\n", J_ROLE_OK, value, J_RESET);
                        applied_this = 1;
                    } else {
                        // Generic: set as environment variable
                        char env_name[256];
                        snprintf(env_name, sizeof(env_name), "JASH_PREF_%s", key);
                        // Convert hyphens to underscores
                        for (char* p = env_name; *p; p++) {
                            if (*p == '-') *p = '_';
                        }
                        setenv(env_name, value, 1);
                        jash_writef(w, ctx, "  %s✓ %s = %s (env: %s)%s\n", J_ROLE_OK, key, value, env_name, J_RESET);
                        applied_this = 1;
                    }
                }

                if (applied_this) applied++;
                count++;
            }
        }
    }
    fclose(f);

    if (count == 0) {
        jash_writef(w, ctx, "  (no preferences found)\n");
    } else {
        jash_writef(w, ctx, "\n  %s✓ %d/%d preferences applied%s\n", J_ROLE_OK, applied, count, J_RESET);
    }
    return 0;
}

static int cmd_recall(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: preference recall \"query\"\n");
        return 1;
    }

    char path[512];
    pref_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writef(w, ctx, "no preferences learned yet\n");
        return 0;
    }

    jash_writef(w, ctx, "%sPreferences matching '%s':%s\n", J_ROLE_ACCENT, args, J_RESET);

    char line[PREF_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (strlen(line) > 0 && strstr(line, args)) {
            jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, line, J_RESET);
            count++;
        }
    }
    fclose(f);

    if (count == 0) {
        jash_writef(w, ctx, "  (none found)\n");
    }
    return 0;
}

static int cmd_forget(jash_writer_t w, void* ctx, const char* args) {
    (void)args;
    char path[512];
    pref_path(path, sizeof(path));

    if (unlink(path) != 0) {
        jash_writef(w, ctx, "error: preferences file not found\n");
        return 1;
    }

    jash_writef(w, ctx, "✓ all preferences forgotten\n");
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

#include "preference_semantic.inc"

int jash_b_preference(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "preference", "jash.preference.v1", args, preference_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%spreference%s — user preference learning (ADR-0034)\n\n"
            "Commands:\n"
            "  preference learn \"pref\" [--category \"cat\"]   learn a preference\n"
            "  preference apply                                apply all preferences\n"
            "  preference recall \"query\"                       recall matching query\n"
            "  preference forget                                remove all preferences\n\n"
            "Preference format: \"key: value\" (e.g., \"output: compact\", \"color: always\")\n"
            "Known keys: output, color, verbose, safe, trash\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "learn") == 0) return cmd_learn(w, ctx, rest);
    if (strcmp(sub, "apply") == 0) return cmd_apply(w, ctx);
    if (strcmp(sub, "recall") == 0) return cmd_recall(w, ctx, rest);
    if (strcmp(sub, "forget") == 0) return cmd_forget(w, ctx, rest);

    jash_writef(w, ctx, "%serror:%s unknown preference subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(preference, "preference", "jash.preference.v1", preference_semantic)
