// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_learn — Code Pattern Learning (ADR-0034)
// F E R M I ∞ H A R T
//
// Commands:
//   learn "pattern" [--repo "name"]   learn a pattern
//   learn list                        list all learned patterns
//   learn recall "query"              recall patterns matching query
//   learn forget <id>                 remove a pattern

#include <builtins/builtin.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define LEARN_DIR "~/.jash/patterns"
#define LEARN_MAX 4096

// ── Helpers ──────────────────────────────────────────────────────────────────

static void patterns_dir(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/patterns", home);
}

static void ensure_dir(void) {
    char path[512];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(path, sizeof(path), "%s/.jash", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.jash/patterns", home);
    mkdir(path, 0755);
}

static int learn_yaml_file(const char* name) {
    size_t n = name ? strlen(name) : 0;
    return n > 5 && strcmp(name + n - 5, ".yaml") == 0;
}

static int learn_repo_name_valid(const char* name) {
    if (!name || !*name) return 0;
    for (const char* p = name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')
            continue;
        return 0;
    }
    return 1;
}

static const char* get_repo_name(void) {
    /* Security: never shell out for presentation metadata. The repository name
     * is only a filename component; the cwd basename is deterministic and
     * contains no command interpreter in the path. */
    static char repo[256] = "general";
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        const char* base = strrchr(cwd, '/');
        base = base && base[1] ? base + 1 : cwd;
        if (learn_repo_name_valid(base)) snprintf(repo, sizeof(repo), "%s", base);
    }
    return repo;
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_learn(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: learn \"pattern\" [--repo \"name\"]\n");
        return 1;
    }

    ensure_dir();

    // Parse pattern and optional repo
    char pattern[LEARN_MAX] = {0};
    char repo[256] = {0};

    const char* p = args;
    if (*p == '"') p++;

    // Extract pattern until closing quote or --repo or end
    int i = 0;
    while (*p && *p != '"' && strncmp(p, " --repo", 7) != 0 && i < LEARN_MAX - 1) {
        pattern[i++] = *p++;
    }
    pattern[i] = '\0';
    if (*p == '"') p++;

    // Look for --repo
    while (*p == ' ') p++;
    if (strncmp(p, "--repo", 6) == 0) {
        p += 6;
        while (*p == ' ') p++;
        if (*p == '"') p++;
        i = 0;
        while (*p && *p != '"' && i < 255) {
            repo[i++] = *p++;
        }
        repo[i] = '\0';
    }

    if (repo[0] == '\0') {
        snprintf(repo, sizeof(repo), "%s", get_repo_name());
    }
    if (!learn_repo_name_valid(repo)) {
        jash_writef(w, ctx, "error: invalid repository name '%s'\n", repo);
        return 1;
    }

    // Write pattern file
    char path[512];
    char dir[512];
    patterns_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.yaml", dir, repo);

    FILE* f = fopen(path, "a");
    if (!f) {
        jash_writef(w, ctx, "error: cannot open %s\n", path);
        return 1;
    }

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(f, "- pattern: \"%s\"\n", pattern);
    fprintf(f, "  learned: %s\n", timestamp);
    fprintf(f, "  confidence: 1.0\n");
    fprintf(f, "  last_used: never\n");
    fclose(f);

    jash_writef(w, ctx, "✓ pattern learned for repo '%s': %s\n", repo, pattern);
    return 0;
}

static int cmd_list(jash_writer_t w, void* ctx) {
    char dir[512];
    patterns_dir(dir, sizeof(dir));

    jash_writef(w, ctx, "%sLearned patterns:%s\n", J_ROLE_ACCENT, J_RESET);

    DIR* d = opendir(dir);
    if (!d) {
        jash_writef(w, ctx, "  (none)\n");
        return 0;
    }

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (!learn_yaml_file(ent->d_name)) continue;
        char basename[256];
        snprintf(basename, sizeof(basename), "%s", ent->d_name);
        char* ext = strrchr(basename, '.');
        if (ext) *ext = '\0';
        jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, basename, J_RESET);
        count++;
    }
    closedir(d);

    if (count == 0) {
        jash_writef(w, ctx, "  (none)\n");
    }
    return 0;
}

static int learn_file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static int cmd_recall(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: learn recall \"query\"\n");
        return 1;
    }

    char dir[512];
    patterns_dir(dir, sizeof(dir));

    jash_writef(w, ctx, "%sPatterns matching '%s':%s\n", J_ROLE_ACCENT, args, J_RESET);

    DIR* d = opendir(dir);
    if (!d) {
        jash_writef(w, ctx, "  (none found)\n");
        return 0;
    }

    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (!learn_yaml_file(ent->d_name)) continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        if (!learn_file_contains(path, args)) continue;

        char basename[256];
        snprintf(basename, sizeof(basename), "%s", ent->d_name);
        char* ext = strrchr(basename, '.');
        if (ext) *ext = '\0';
        jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, basename, J_RESET);
        count++;
    }
    closedir(d);

    if (count == 0) {
        jash_writef(w, ctx, "  (none found)\n");
    }
    return 0;
}

static int cmd_forget(jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: learn forget <repo>\n");
        return 1;
    }
    if (!learn_repo_name_valid(args)) {
        jash_writef(w, ctx, "error: invalid repository name '%s'\n", args);
        return 1;
    }

    char path[512];
    char dir[512];
    patterns_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s.yaml", dir, args);

    if (unlink(path) != 0) {
        jash_writef(w, ctx, "error: pattern file not found: %s\n", args);
        return 1;
    }

    jash_writef(w, ctx, "✓ patterns for repo '%s' forgotten\n", args);
    return 0;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

#include "learn_semantic.inc"

int jash_b_learn(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int selected = semantic_dispatch(s, w, ctx, "learn", "jash.learn.v1", args, learn_semantic);
    if (selected >= 0) return selected;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx,
            "%slearn%s — code pattern learning (ADR-0034)\n\n"
            "Commands:\n"
            "  learn \"pattern\" [--repo \"name\"]   learn a pattern\n"
            "  learn list                        list all learned patterns\n"
            "  learn recall \"query\"              recall patterns matching query\n"
            "  learn forget <repo>               remove patterns for repo\n",
            J_ROLE_BANNER, J_RESET);
        return 0;
    }

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    (void)n;

    if (strcmp(sub, "list") == 0) return cmd_list(w, ctx);
    if (strcmp(sub, "recall") == 0) return cmd_recall(w, ctx, rest);
    if (strcmp(sub, "forget") == 0) return cmd_forget(w, ctx, rest);
    // Default: treat as "learn <pattern>"
    return cmd_learn(w, ctx, args);

    jash_writef(w, ctx, "%serror:%s unknown learn subcommand '%s'\n",
                J_ROLE_ERR, J_RESET, sub);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(learn, "learn", "jash.learn.v1", learn_semantic)
