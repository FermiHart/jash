// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: lint — static analysis for shell scripts
// Wave 4: Advanced Linting. Detects dangerous patterns, missing quotes,
// useless constructs, and performance anti-patterns in shell scripts.

#include "builtin.h"
#include <theme/io.h>
#include <core/state.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ── severity levels ── */
typedef enum {
    LINT_ERROR = 0,
    LINT_WARNING,
    LINT_STYLE,
    LINT_INFO
} lint_sev_t;

static const char* sev_label(lint_sev_t s) {
    switch (s) {
        case LINT_ERROR:   return "error";
        case LINT_WARNING: return "warning";
        case LINT_STYLE:   return "style";
        case LINT_INFO:    return "info";
    }
    return "unknown";
}

/* ── lint result ── */
typedef struct {
    unsigned   line;
    unsigned   col;
    lint_sev_t sev;
    const char* rule_id;
    const char* message;
    const char* fix_hint;
} lint_result_t;

#define MAX_RESULTS 256

/* ── single-line pattern matcher ── */
typedef struct {
    const char* rule_id;
    lint_sev_t  sev;
    const char* pattern;     /* substring to match (case-sensitive) */
    const char* message;
    const char* fix_hint;
    int         (*check)(const char* line, unsigned len);  /* optional custom check, NULL = substring match */
} lint_rule_t;

/* ── custom check helpers ── */

/* Detect unquoted $VAR in [ ] test expressions: [ $var = x ] */
static int check_unquoted_test_var(const char* line, unsigned len) {
    (void)len;
    int in_bracket = 0;
    int in_dquote = 0;
    for (const char* p = line; *p; p++) {
        if (*p == '"') { in_dquote = !in_dquote; continue; }
        if (in_dquote) continue;
        if (*p == '[' && p[1] != '[' && !in_bracket) { in_bracket = 1; continue; }
        if (*p == ']') { in_bracket = 0; continue; }
        if (in_bracket && *p == '$' && p[1] != '{' && p[1] != '(') {
            /* Unquoted $VAR inside [ ] — check it's not inside quotes */
            const char* q = p;
            int has_quote_before = 0;
            while (q > line && *q != '[') { q--; if (*q == '"') has_quote_before = 1; }
            if (!has_quote_before) return 1;
        }
    }
    return 0;
}

/* Detect cd without error check: `cd somewhere; cmd` without `||` */
static int check_cd_no_check(const char* line, unsigned len) {
    (void)len;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "cd ", 3) != 0) return 0;
    /* Skip cd argument */
    p += 3;
    while (*p && *p != ';' && *p != '|' && *p != '&') p++;
    if (*p == ';' || *p == '&') {
        /* Check for || after cd */
        const char* rest = p + 1;
        while (*rest == ' ') rest++;
        if (rest[0] == '|' && rest[1] == '|') return 0;
        if (strncmp(rest, "&&", 2) == 0) return 0;
        return 1;
    }
    return 0;
}

/* Detect `cat file | cmd` — useless cat */
static int check_useless_cat(const char* line, unsigned len) {
    (void)len;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "cat ", 4) != 0) return 0;
    /* Must contain a pipe before end */
    while (*p) { if (*p == '|' && p[1] != '|') return 1; p++; }
    return 0;
}

/* Detect `for i in $(ls)` (anywhere on line) */
static int check_for_in_ls(const char* line, unsigned len) {
    (void)len;
    const char* p = strstr(line, "for ");
    while (p) {
        const char* v = p + 4;
        while (*v == ' ' || *v == '\t') v++;
        while (*v && *v != ' ' && *v != '\t' && *v != ';') v++;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "in ", 3) == 0) {
            v += 3;
            while (*v == ' ' || *v == '\t') v++;
            if (strncmp(v, "$(ls", 4) == 0) return 1;
            if (strncmp(v, "`ls", 3) == 0) return 1;
        }
        p = strstr(p + 1, "for ");
    }
    return 0;
}

/* Detect `ls | wc -l` instead of find ... | wc -l */
static int check_ls_wc(const char* line, unsigned len) {
    (void)len;
    return (strstr(line, "ls | wc") != NULL);
}

/* Detect `[ $? -eq 0 ]` anti-pattern (anywhere on line) */
static int check_question_eq(const char* line, unsigned len) {
    (void)len;
    /* Find a single '[' (not '[[' ) */
    const char* br = strchr(line, '[');
    while (br) {
        if (br[1] != '[') {
            /* Check $? appears after this [ */
            const char* qmark = strstr(br, "$?");
            if (qmark && qmark < strchr(br, ']')) return 1;
        }
        br = strchr(br + 1, '[');
    }
    return 0;
}

/* Detect `export VAR=$(cmd)` or `declare VAR=$(cmd)` or `local VAR=$(cmd)` */
static int check_export_subshell(const char* line, unsigned len) {
    (void)len;
    static const char* prefixes[] = {"export ", "declare ", "local "};
    for (int i = 0; i < 3; i++) {
        const char* p = strstr(line, prefixes[i]);
        while (p) {
            const char* eq = strchr(p + strlen(prefixes[i]), '=');
            if (eq && eq[1] == '$' && eq[2] == '(') return 1;
            p = strstr(p + 1, prefixes[i]);
        }
    }
    return 0;
}

/* Detect `$(...)` used as a command (not in assignment) */
static int check_cmd_output_exec(const char* line, unsigned len) {
    (void)len;
    /* Check if line starts with $( */
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '$' && p[1] == '(') return 1;
    /* Check after ; && || | */
    const char* pstart = strstr(line, "; $(");
    if (pstart) return 1;
    pstart = strstr(line, "&& $(");
    if (pstart) return 1;
    pstart = strstr(line, "|| $(");
    if (pstart) return 1;
    pstart = strstr(line, "| $(");
    if (pstart) return 1;
    return 0;
}

/* Detect rm -rf / */
static int check_rm_rf_root(const char* line, unsigned len) {
    (void)len;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "rm ", 3) != 0 && strncmp(p, "rm\t", 3) != 0) return 0;
    p += 3;
    while (*p == ' ') p++;
    if (*p != '-') return 0;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '/' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0' || p[1] == '*' || p[1] == '.')) return 1;
    if (strncmp(p, "/*", 2) == 0) return 1;
    return 0;
}

/* Detect missing -r flag on read */
static int check_read_no_r(const char* line, unsigned len) {
    (void)len;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "read ", 5) != 0) return 0;
    p += 5;
    if (*p == '-') {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    /* If we see 'read var' without -r and it's in a while loop */
    return (*p != '\0');
}

/* Detect unquoted $@ */
static int check_unquoted_star_at(const char* line, unsigned len) {
    (void)len;
    int in_quote = 0;
    for (const char* p = line; p < line + len && *p; p++) {
        if (*p == '\'' || *p == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (p[0] == '$' && (p[1] == '@' || p[1] == '*') &&
            (p == line || p[-1] != '"'))
            return 1;
    }
    return 0;
}

/* ── performance anti-pattern checks ── */

/* Detect `for i in $(cat file)` — should use while read loop */
static int check_for_in_cat(const char* line, unsigned len) {
    (void)len;
    const char* p = strstr(line, "for ");
    while (p) {
        const char* v = p + 4;
        while (*v == ' ' || *v == '\t') v++;
        while (*v && *v != ' ' && *v != '\t' && *v != ';') v++;
        while (*v == ' ' || *v == '\t') v++;
        if (strncmp(v, "in ", 3) == 0) {
            v += 3;
            while (*v == ' ' || *v == '\t') v++;
            if (strncmp(v, "$(cat ", 6) == 0 || strncmp(v, "$(cat\t", 6) == 0) return 1;
            if (strncmp(v, "`cat ", 5) == 0 || strncmp(v, "`cat\t", 5) == 0) return 1;
        }
        p = strstr(p + 1, "for ");
    }
    return 0;
}

/* Detect `grep ... | awk` — awk can grep internally */
static int check_grep_awk(const char* line, unsigned len) {
    (void)len;
    const char* g = strstr(line, "grep ");
    while (g) {
        const char* pipe = strchr(g, '|');
        if (pipe) {
            const char* a = pipe + 1;
            while (*a == ' ') a++;
            if (strncmp(a, "awk", 3) == 0) return 1;
        }
        g = strstr(g + 1, "grep ");
    }
    return 0;
}

/* Detect `sort | uniq` — use sort -u instead */
static int check_sort_uniq(const char* line, unsigned len) {
    (void)len;
    return (strstr(line, "sort") && strstr(line, "| uniq"));
}

/* Detect `grep ... | wc -l` — use grep -c instead */
static int check_grep_wc(const char* line, unsigned len) {
    (void)len;
    const char* g = strstr(line, "grep ");
    while (g) {
        const char* pipe = strchr(g, '|');
        if (pipe) {
            const char* w = pipe + 1;
            while (*w == ' ') w++;
            if (strncmp(w, "wc", 2) == 0) return 1;
        }
        g = strstr(g + 1, "grep ");
    }
    return 0;
}

/* Detect `grep ... | head` — use grep -m instead */
static int check_grep_head(const char* line, unsigned len) {
    (void)len;
    const char* g = strstr(line, "grep ");
    while (g) {
        const char* pipe = strchr(g, '|');
        if (pipe) {
            const char* h = pipe + 1;
            while (*h == ' ') h++;
            if (strncmp(h, "head", 4) == 0) return 1;
        }
        g = strstr(g + 1, "grep ");
    }
    return 0;
}

static int check_assign_space(const char* line, unsigned len) {
    (void)len;
    int in_quote = 0;
    for (const char* p = line; *p; p++) {
        if (*p == '\'' || *p == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (isupper((unsigned char)*p) || *p == '_') {
            const char* s = p;
            while (isalnum((unsigned char)*s) || *s == '_') s++;
            if (*s == ' ' && s[1] == '=') return 1;
        }
    }
    return 0;
}

/* ── rule table ── */
static const lint_rule_t LINT_RULES[] = {
    /* Dangerous patterns */
    { .rule_id = "RM_ROOT",      .sev = LINT_ERROR,   .pattern = "",
      .message = "rm -rf /: dangerous recursive delete on root",
      .fix_hint = "Never run rm -rf /. Use --no-preserve-root only if you really mean it",
      .check = check_rm_rf_root },

    { .rule_id = "CD_NO_CHECK",  .sev = LINT_WARNING, .pattern = "",
      .message = "cd without error check: subsequent commands run in wrong directory if cd fails",
      .fix_hint = "Use: cd somewhere || exit 1",
      .check = check_cd_no_check },

    { .rule_id = "EVAL_VAR",     .sev = LINT_WARNING, .pattern = "eval $",
      .message = "eval with variable expansion: potential code injection",
      .fix_hint = "Avoid eval; use arrays or functions instead",
      .check = NULL },

    { .rule_id = "UNQUOTED_TEST", .sev = LINT_WARNING, .pattern = "",
      .message = "unquoted $VAR in [ ] test: breaks when variable is empty or contains spaces",
      .fix_hint = "Use: [ \"$var\" = \"x\" ]",
      .check = check_unquoted_test_var },

    /* Useless constructs (performance anti-patterns) */
    { .rule_id = "USELESS_CAT",  .sev = LINT_STYLE,   .pattern = "",
      .message = "useless cat: pipe from cat to command can be replaced with input redirection",
      .fix_hint = "Use: cmd < file instead of cat file | cmd",
      .check = check_useless_cat },

    { .rule_id = "CAT_SUBST",    .sev = LINT_STYLE,   .pattern = "$(cat ",
      .message = "$(cat file) can be replaced with $(<file)",
      .fix_hint = "Use: data=$(<file) instead of data=$(cat file)",
      .check = NULL },

    { .rule_id = "FOR_IN_LS",    .sev = LINT_STYLE,   .pattern = "",
      .message = "for i in $(ls) breaks on filenames with spaces; use glob instead",
      .fix_hint = "Use: for i in * instead of for i in $(ls)",
      .check = check_for_in_ls },

    { .rule_id = "LS_WC",        .sev = LINT_STYLE,   .pattern = "",
      .message = "ls | wc -l counts filenames with newlines incorrectly; use find -printf or count in shell",
      .fix_hint = "Use: count=0; for f in *; do ((count++)); done instead of ls | wc -l",
      .check = check_ls_wc },

    /* ShellCheck parity */
    { .rule_id = "DEPRECATED_ARITH", .sev = LINT_WARNING, .pattern = "$[",
      .message = "deprecated $[...] arithmetic syntax: use POSIX $((...)) instead",
      .fix_hint = "Use: $((expr)) instead of $[expr]",
      .check = NULL },

    { .rule_id = "QUESTION_EQ",  .sev = LINT_WARNING, .pattern = "",
      .message = "$? check with [ $? -eq 0 ]: use 'if cmd; then' instead",
      .fix_hint = "Use: if cmd; then ... fi instead of cmd; [ $? -eq 0 ] && ...",
      .check = check_question_eq },

    { .rule_id = "EXPORT_SUBSHELL", .sev = LINT_INFO, .pattern = "",
      .message = "export/declare with $(...) subshell: exit status of $(...) is lost",
      .fix_hint = "Split: cmd; export VAR=\"$output\" to preserve exit status",
      .check = check_export_subshell },

    { .rule_id = "CMD_OUTPUT_EXEC", .sev = LINT_STYLE, .pattern = "",
      .message = "executing command output: $(...) as a command on the line",
      .fix_hint = "Did you mean to assign the output? Use: var=$(cmd)",
      .check = check_cmd_output_exec },

    /* Quote issues */
    { .rule_id = "UNQUOTED_AT",  .sev = LINT_WARNING, .pattern = "",
      .message = "unquoted $@ or $*: splits on spaces, breaks arguments with spaces",
      .fix_hint = "Use: \"$@\" instead of $@",
      .check = check_unquoted_star_at },

    { .rule_id = "QUOTED_VAR",   .sev = LINT_INFO,    .pattern = "${",
      .message = "variable reference uses ${} with no brace extension: unnecessary braces",
      .fix_hint = "Use $var instead of ${var} for simple variable references",
      .check = NULL },

    /* Other issues */
    { .rule_id = "ASSIGN_SPACE", .sev = LINT_STYLE,   .pattern = "",
      .message = "variable assignment with space before =: not valid in shell",
      .fix_hint = "Use: VAR=value (no spaces around =)",
      .check = check_assign_space },

    { .rule_id = "READ_NO_R",    .sev = LINT_INFO,    .pattern = "",
      .message = "read without -r flag: backslashes in input are interpreted",
      .fix_hint = "Use: read -r var to treat backslashes literally",
      .check = check_read_no_r },

    /* Performance anti-patterns */
    { .rule_id = "FOR_IN_CAT",   .sev = LINT_STYLE,   .pattern = "",
      .message = "for i in $(cat file): reads entire file into memory; use while read loop",
      .fix_hint = "Use: while read -r line; do ...; done < file",
      .check = check_for_in_cat },

    { .rule_id = "GREP_AWK",     .sev = LINT_STYLE,   .pattern = "",
      .message = "grep | awk: awk can filter internally without a separate grep",
      .fix_hint = "Use: awk '/pattern/ {print $2}' instead of grep pattern | awk '{print $2}'",
      .check = check_grep_awk },

    { .rule_id = "SORT_UNIQ",    .sev = LINT_STYLE,   .pattern = "",
      .message = "sort | uniq: use sort -u instead of two commands",
      .fix_hint = "Use: sort -u file instead of sort file | uniq",
      .check = check_sort_uniq },

    { .rule_id = "GREP_WC",      .sev = LINT_STYLE,   .pattern = "",
      .message = "grep | wc -l: use grep -c to count matches directly",
      .fix_hint = "Use: grep -c pattern file instead of grep pattern | wc -l",
      .check = check_grep_wc },

    { .rule_id = "GREP_HEAD",    .sev = LINT_STYLE,   .pattern = "",
      .message = "grep | head -n: use grep -m N to limit matches directly",
      .fix_hint = "Use: grep -m N pattern file instead of grep pattern | head -n N",
      .check = check_grep_head },

    { .rule_id = NULL, .sev = 0, .pattern = NULL, .message = NULL, .fix_hint = NULL, .check = NULL }
};

/* ── check one line against all rules ── */
static unsigned lint_line(const char* line, unsigned line_num,
                          lint_result_t* results, unsigned max_results,
                          unsigned* count) {
    unsigned rcount = 0;
    unsigned len = (unsigned)strlen(line);

    for (int ri = 0; LINT_RULES[ri].rule_id && *count < max_results; ri++) {
        const lint_rule_t* rule = &LINT_RULES[ri];
        int match = 0;

        if (rule->check) {
            match = rule->check(line, len);
        } else if (rule->pattern) {
            match = (strstr(line, rule->pattern) != NULL);
        }

        if (match) {
            results[*count].line = line_num;
            results[*count].col = 1;
            results[*count].sev = rule->sev;
            results[*count].rule_id = rule->rule_id;
            results[*count].message = rule->message;
            results[*count].fix_hint = rule->fix_hint;
            (*count)++;
            rcount++;
        }
    }
    return rcount;
}

/* Check if the first line has a shebang */
static int has_shebang(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 1;
    char buf[8];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 1; }
    fclose(f);
    return (buf[0] == '#' && buf[1] == '!');
}

/* ── read file, lint all lines ── */
static unsigned lint_file(const char* path, lint_result_t* results, unsigned max_results) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    unsigned count = 0;
    char buf[4096];
    unsigned line_num = 0;

    while (fgets(buf, sizeof(buf), f) && count < max_results) {
        line_num++;
        size_t bl = strlen(buf);
        while (bl > 0 && (buf[bl-1] == '\n' || buf[bl-1] == '\r')) buf[--bl] = '\0';
        lint_line(buf, line_num, results, max_results, &count);
    }

    fclose(f);

    /* File-level checks (once, after all lines read) */
    if (count < max_results && !has_shebang(path)) {
        results[count].line = 1;
        results[count].col = 1;
        results[count].sev = LINT_INFO;
        results[count].rule_id = "SHEBANG_MISS";
        results[count].message = "script has no shebang line";
        results[count].fix_hint = "Add: #!/usr/bin/env bash at the top of the script";
        count++;
    }

    return count;
}

/* ── emit text output ── */
static void emit_text(const lint_result_t* results, unsigned count,
                      const char* filename,
                      jash_writer_t w, void* ctx) {
    unsigned by_sev[4] = {0};
    for (unsigned i = 0; i < count; i++) {
        by_sev[results[i].sev]++;
        jash_writef(w, ctx, "%s:%u:%u: %s: [%s] %s\n",
                    filename, results[i].line, results[i].col,
                    sev_label(results[i].sev), results[i].rule_id,
                    results[i].message);
        if (results[i].fix_hint) {
            jash_writef(w, ctx, "  fix: %s\n", results[i].fix_hint);
        }
    }
    unsigned total = count;
    jash_writef(w, ctx, "\n%d issues found (%d errors, %d warnings, %d style, %d info)\n",
                total, by_sev[0], by_sev[1], by_sev[2], by_sev[3]);
}

/* ── emit JSON output ── */
static void emit_json(const lint_result_t* results, unsigned count,
                      jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "{\"lint\":[");
    for (unsigned i = 0; i < count; i++) {
        if (i > 0) jash_writes(w, ctx, ",");
        jash_writef(w, ctx,
                    "{\"line\":%u,\"col\":%u,\"severity\":\"%s\","
                    "\"rule_id\":\"%s\",\"message\":\"%s\"",
                    results[i].line, results[i].col,
                    sev_label(results[i].sev),
                    results[i].rule_id, results[i].message);
        if (results[i].fix_hint) {
            jash_writef(w, ctx, ",\"fix_hint\":\"%s\"", results[i].fix_hint);
        }
        jash_writes(w, ctx, "}");
    }
    jash_writef(w, ctx, "],\"total\":%u}\n", count);
}

/* ── lint stdin or inline code ── */
static unsigned lint_string(const char* code, lint_result_t* results, unsigned max_results) {
    unsigned count = 0;
    unsigned line_num = 0;
    const char* p = code;
    const char* line_start = code;

    while (*p && count < max_results) {
        if (*p == '\n' || *p == '\0') {
            line_num++;
            unsigned len = (unsigned)(p - line_start);
            if (len > 0) {
                char buf[4096];
                unsigned clen = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
                memcpy(buf, line_start, clen);
                buf[clen] = '\0';
                lint_line(buf, line_num, results, max_results, &count);
            }
            line_start = p + 1;
        }
        p++;
    }
    if (line_start < p) {
        line_num++;
        unsigned len = (unsigned)(p - line_start);
        if (len > 0) {
            char buf[4096];
            unsigned clen = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
            memcpy(buf, line_start, clen);
            buf[clen] = '\0';
            lint_line(buf, line_num, results, max_results, &count);
        }
    }
    return count;
}

/* ── main lint builtin ── */
int jash_b_lint(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    if (!args || !*args) {
        jash_writes(w, ctx, "usage: lint <script.sh> | -c 'code' | --json <file>\n");
        return 1;
    }

    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;

    int json_mode = 0;
    int is_inline = 0;

    /* Parse flags: --json, -c — may appear in any order */
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '-') break;
        const char* sp = p;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        size_t flen = (size_t)(sp - p);
        if (flen == 6 && memcmp(p, "--json", 6) == 0) {
            json_mode = 1;
            p = sp;
        } else if (flen == 2 && memcmp(p, "-c", 2) == 0) {
            is_inline = 1;
            p = sp;
        } else {
            jash_writef(w, ctx, "lint: unknown flag. use lint <file> | lint -c 'code' | lint --json <file>\n");
            return 1;
        }
    }

    lint_result_t results[MAX_RESULTS];
    unsigned count = 0;
    const char* fname = is_inline ? "(stdin)" : p;

    if (is_inline) {
        count = lint_string(p, results, MAX_RESULTS);
    } else {
        FILE* test = fopen(p, "r");
        if (!test) {
            jash_writef(w, ctx, "lint: cannot open %s\n", p);
            return 1;
        }
        fclose(test);
        count = lint_file(p, results, MAX_RESULTS);
    }

    if (count == 0) {
        if (json_mode) {
            jash_writes(w, ctx, "{\"lint\":[],\"total\":0}\n");
        } else {
            jash_writes(w, ctx, "no issues found\n");
        }
        return 0;
    }

    if (json_mode) {
        emit_json(results, count, w, ctx);
    } else {
        emit_text(results, count, fname, w, ctx);
    }

    /* Count errors/warnings vs info/style */
    unsigned serious = 0;
    for (unsigned i = 0; i < count; i++) {
        if (results[i].sev <= LINT_WARNING) serious++;
    }
    return serious > 0 ? 1 : 0;
}

#include "lint_native.inc"
