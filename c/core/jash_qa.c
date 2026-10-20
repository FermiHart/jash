/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* jash_qa.c — Self-Healing QA Engine for Jash
 * F E R M I  ∞  H A R T  ·  contact@fermihart.com
 *
 * The first AI-driven QA system built into a shell.
 * Scans source code, generates tests, runs them, and produces a maturity report.
 *
 * Usage: ? qa [scan|test|run|status]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>

#include "jash_qa.h"
#include "osif/osinfo.h"
#include <sys/syscalls.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CONFIGURATION
 * ═══════════════════════════════════════════════════════════════════════════ */

#define QA_DIR          ".jash_qa"
#define QA_REPORT       ".jash_qa/maturity_report.md"
#define QA_BUGS         ".jash_qa/bugs.md"
#define QA_TESTS        ".jash_qa/test_results.md"
#define QA_PATTERNS     ".jash_qa/patterns.md"
/* BUG-J1 (ADR-0016 §5.3): sandbox root honors $TMPDIR. */
#define QA_SANDBOX_NAME "jash_qa_sandbox"
static const char* qa_sandbox(void) {
    static char path[256];
    if (!path[0]) {
        const char *t = getenv("TMPDIR");
        if (!t || !*t) t = jash_tmpdir();
        snprintf(path, sizeof(path), "%s/%s", t, QA_SANDBOX_NAME);
    }
    return path;
}
#define QA_SANDBOX qa_sandbox()
#define MAX_BUGS        1024
#define MAX_TESTS       2048
#define MAX_LINE_LEN    4096
#define MAX_RISK_ITEMS  256

/* ═══════════════════════════════════════════════════════════════════════════
 * DATA STRUCTURES
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    SEV_CRITICAL,   /* Crash, data loss, security */
    SEV_HIGH,       /* Wrong output, missing error handling */
    SEV_MEDIUM,     /* Visual glitches, edge cases */
    SEV_LOW,        /* Cosmetic, minor issues */
    SEV_INFO        /* Informational, not a bug */
} severity_t;

typedef struct {
    char file[256];     /* source file */
    int  line;          /* line number */
    char pattern[64];   /* risk pattern found */
    char desc[256];     /* description */
    severity_t sev;     /* severity */
} risk_item_t;

typedef struct {
    char name[128];     /* test name */
    char command[1024]; /* command to execute */
    char expected[512]; /* expected output (substring) */
    severity_t sev;     /* expected severity if fails */
    int  pass;          /* 1=pass, 0=fail, -1=not run */
    char actual[1024];  /* actual output */
    int  exit_code;     /* actual exit code */
} test_case_t;

typedef struct {
    char test_name[128];
    char command[1024];
    char error[1024];
    severity_t sev;
    char file[256];
    int  line;
} bug_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * SOURCE READER — Scans .c files for vulnerability patterns
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ADR-0016 §4: QA tables are heap-allocated on first use, not static.
 * The fixed arrays reserved 8.2 MB of .bss in EVERY shell process for a
 * subsystem that only runs when the user invokes `jash qa`. Capacities
 * (MAX_RISK_ITEMS/MAX_TESTS/MAX_BUGS) are unchanged. */
static risk_item_t *g_risks;   /* [MAX_RISK_ITEMS] */
static int g_risk_count = 0;

static test_case_t *g_tests;   /* [MAX_TESTS] */
static int g_test_count = 0;

static bug_t *g_bugs;          /* [MAX_BUGS] */
static int g_bug_count = 0;

/* Allocate the QA context once. Returns 0 on success, -1 on OOM. */
static int qa_ensure(void) {
    if (g_risks && g_tests && g_bugs) return 0;
    if (!g_risks) g_risks = (risk_item_t*)calloc(MAX_RISK_ITEMS, sizeof(risk_item_t));
    if (!g_tests) g_tests = (test_case_t*)calloc(MAX_TESTS, sizeof(test_case_t));
    if (!g_bugs)  g_bugs  = (bug_t*)calloc(MAX_BUGS, sizeof(bug_t));
    if (g_risks && g_tests && g_bugs) return 0;
    /* OOM: free partial allocations; callers report and bail. */
    free(g_risks); g_risks = NULL;
    free(g_tests); g_tests = NULL;
    free(g_bugs);  g_bugs  = NULL;
    return -1;
}

/* Guard macro for the public entry points. */
#define QA_ENSURE_OR_FAIL(w, ctx)                                        \
    do {                                                                 \
        if (qa_ensure() != 0) {                                          \
            jash_writes((w), (ctx), "qa: out of memory\r\n");            \
            return;                                                      \
        }                                                                \
    } while (0)

/* Risk patterns to scan for */
typedef struct {
    const char *pattern;    /* regex-like substring to find */
    const char *desc;       /* description of the risk */
    severity_t sev;         /* severity level */
    const char *fix_hint;   /* how to fix it */
} risk_pattern_t;

static const risk_pattern_t RISK_PATTERNS[] = {
    /* Buffer risks */
    {"sprintf(",      "sprintf without bounds checking — use snprintf",      SEV_HIGH,    "Replace with snprintf(buf, sizeof(buf), ...)"},
    {"gets(",         "gets() is dangerous — never use it",                  SEV_CRITICAL,"Remove completely, use fgets()"},
    {"strcpy(",       "strcpy without bounds checking — use strncpy",        SEV_MEDIUM,  "Replace with strncpy or strlcpy"},
    {"strcat(",       "strcat without bounds checking — use strncat",        SEV_MEDIUM,  "Replace with strncat with size limit"},

    /* Memory risks */
    {"malloc(",       "malloc without NULL check",                           SEV_HIGH,    "Add if (!ptr) return error; after malloc"},
    {"realloc(",      "realloc without NULL check — old pointer leaks on failure", SEV_HIGH, "Add NULL check, save old pointer before reassign"},
    {"free(",         "free found — verify no double-free or use-after-free", SEV_LOW,    "Verify free is called once per allocation"},

    /* String risks */
    {"strlen(",       "strlen on potentially NULL string",                   SEV_MEDIUM,  "Add NULL check before strlen"},
    {"strcmp(",       "strcmp on potentially NULL string",                    SEV_MEDIUM,  "Add NULL checks for both strings"},

    /* File I/O risks */
    {"fopen(",        "fopen without NULL check",                            SEV_HIGH,    "Add if (!f) return error; after fopen"},
    {"popen(",        "popen without NULL check",                            SEV_HIGH,    "Add NULL check after popen"},
    {"fclose(",       "fclose — verify file pointer is valid",               SEV_LOW,    "Verify fopen succeeded before fclose"},

    /* Process risks */
    {"system(",       "system() call — potential command injection",         SEV_HIGH,    "Use execve with explicit argv instead"},
    {"execve(",       "execve call — verify all error paths",                SEV_MEDIUM,  "Check return value and handle errors"},

    /* Signal risks */
    {"signal(",       "signal() — consider using sigaction() instead",       SEV_LOW,    "Replace with sigaction for portable behavior"},

    /* Integer risks */
    {"atoi(",         "atoi() — no error checking, use strtol()",           SEV_MEDIUM,  "Replace with strtol() with error checking"},
    {"atol(",         "atol() — no error checking, use strtol()",           SEV_MEDIUM,  "Replace with strtol() with error checking"},

    /* Buffer size risks */
    {"char ",         "Stack-allocated char array — check size is sufficient", SEV_LOW,  "Verify buffer size is adequate for all inputs"},

    /* Race conditions */
    {"fork()",        "fork() — check for race conditions with signals",     SEV_MEDIUM,  "Block signals before fork, restore after"},

    /* Format string */
    {"printf(",       "printf with user-controlled format string",           SEV_HIGH,    "Use printf(\"%s\", user_str) instead of printf(user_str)"},
    {"fprintf(",      "fprintf with user-controlled format string",          SEV_HIGH,    "Use fprintf(f, \"%s\", user_str) instead"},

    {NULL, NULL, SEV_INFO, NULL}
};

static void scan_file_for_risks(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char line[MAX_LINE_LEN];
    int line_num = 0;

    while (fgets(line, sizeof(line), f) && g_risk_count < MAX_RISK_ITEMS) {
        line_num++;

        /* Skip comments (simple heuristic) */
        if (line[0] == '/' && line[1] == '*') continue;
        if (line[0] == '*' || (line[0] == ' ' && line[1] == '*')) continue;
        if (line[0] == '/' && line[1] == '/') continue;

        for (int p = 0; RISK_PATTERNS[p].pattern; p++) {
            if (strstr(line, RISK_PATTERNS[p].pattern)) {
                risk_item_t *r = &g_risks[g_risk_count++];
                strlcpy(r->file, filepath, sizeof(r->file));
                r->line = line_num;
                strlcpy(r->pattern, RISK_PATTERNS[p].pattern, sizeof(r->pattern));
                strlcpy(r->desc, RISK_PATTERNS[p].desc, sizeof(r->desc));
                r->sev = RISK_PATTERNS[p].sev;
                break; /* one risk per line */
            }
        }
    }
    fclose(f);
}

static void scan_directory(const char *dir, jash_writer_t w, void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        /* Q2 fix: d_type is unreliable (DT_UNKNOWN on MINIX mfs, XFS...).
         * osif_dirent_kind falls back to lstat. */
        int kind = osif_dirent_kind(dir, ent);
        if (kind == OSIF_KIND_DIR) {
            scan_directory(path, w, ctx);
        } else if (kind == OSIF_KIND_REG) {
            /* Scan .c and .h files */
            if (strstr(ent->d_name, ".c") || strstr(ent->d_name, ".h")) {
                char msg[sizeof path + 64];
                snprintf(msg, sizeof(msg), "\033[90m  scanning %s\033[0m\r\n", path);
                jash_writes(w, ctx, msg);
                scan_file_for_risks(path);
            }
        }
    }
    closedir(d);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TEST GENERATOR — Creates test cases from risk analysis + command map
 * ═══════════════════════════════════════════════════════════════════════════ */

static void add_test(const char *name, const char *cmd, const char *expected, severity_t sev) {
    if (g_test_count >= MAX_TESTS) return;
    test_case_t *t = &g_tests[g_test_count++];
    strlcpy(t->name, name, sizeof(t->name));
    strlcpy(t->command, cmd, sizeof(t->command));
    strlcpy(t->expected, expected, sizeof(t->expected));
    t->sev = sev;
    t->pass = -1;
    t->actual[0] = '\0';
    t->exit_code = -1;
}

static void generate_tests(void) {
    /* ── Basic command tests ── */
    add_test("pwd_returns_path", "pwd", "/", SEV_HIGH);
    add_test("echo_basic", "echo hello", "hello", SEV_MEDIUM);
    add_test("echo_empty", "echo", "", SEV_LOW);
    add_test("echo_flags", "echo -n test", "test", SEV_LOW);
    add_test("ls_root", "ls /", "tmp", SEV_MEDIUM);
    add_test("ls_cwd", "ls", "", SEV_LOW);
    add_test("ls_nonexistent", "ls /nonexistent_$$$", "No such file", SEV_LOW);
    add_test("cd_home", "cd ~ && pwd", "/", SEV_HIGH);
    add_test("cd_nonexistent", "cd /nonexistent_$$$ 2>&1; echo $?", "1", SEV_MEDIUM);
    add_test("cd_dash", "cd /tmp && cd - && pwd", "jash_qa_sandbox", SEV_MEDIUM);
    add_test("cat_file", "echo test > /tmp/jash_qa_test && cat /tmp/jash_qa_test", "test", SEV_MEDIUM);
    add_test("cat_nonexistent", "cat /nonexistent_$$$", "No such file", SEV_LOW);
    add_test("grep_basic", "echo -e 'a\\nb\\nc' | grep b", "b", SEV_MEDIUM);
    add_test("grep_no_match", "echo hello | grep xyz; echo $?", "1", SEV_LOW);
    add_test("wc_lines", "echo -e 'a\\nb\\nc' | wc -l", "3", SEV_MEDIUM);
    add_test("whoami", "whoami", "", SEV_LOW);
    add_test("date_format", "date +%Y", "", SEV_LOW);
    add_test("uname", "uname", "", SEV_LOW);
    add_test("which_ls", "which ls", "/ls", SEV_LOW);

    /* ── Edge case tests ── */
    add_test("empty_command", "", "", SEV_HIGH);
    add_test("semicolon_chain", "true; true", "", SEV_MEDIUM);
    add_test("pipe_chain", "echo hello | cat | cat", "hello", SEV_MEDIUM);
    add_test("env_set_get", "export JASH_QA_VAR=42 && echo $JASH_QA_VAR", "42", SEV_HIGH);
    add_test("env_unset", "export JASH_QA_DEL=1 && unset JASH_QA_DEL && echo $JASH_QA_DEL", "", SEV_MEDIUM);
    add_test("alias_basic", "echo aliased", "aliased", SEV_MEDIUM);
    add_test("exit_code_success", "true; echo $?", "0", SEV_MEDIUM);
    add_test("exit_code_failure", "false; echo $?", "1", SEV_MEDIUM);

    /* ── Stress tests ── */
    add_test("long_output", "seq 1 1000 | wc -l", "1000", SEV_MEDIUM);
    add_test("deep_pipe", "echo x | cat | cat | cat | cat | cat", "x", SEV_MEDIUM);
    add_test("empty_pipe", "echo | cat", "", SEV_LOW);
    add_test("quotes_single", "echo 'hello world'", "hello world", SEV_LOW);
    add_test("quotes_double", "echo \"hello world\"", "hello world", SEV_LOW);
    add_test("backtick_sub", "echo `echo nested`", "nested", SEV_MEDIUM);
    add_test("dollar_paren", "echo $(echo nested)", "nested", SEV_MEDIUM);

    /* ── Special characters ── */
    add_test("spaces_in_name", "mkdir -p '/tmp/jash_qa test' && ls '/tmp/jash_qa test' && rmdir '/tmp/jash_qa test'", "", SEV_MEDIUM);
    add_test("special_chars", "touch '/tmp/jash_qa_!@#' && ls '/tmp/jash_qa_!@#' && rm '/tmp/jash_qa_!@#'", "", SEV_MEDIUM);
    add_test("unicode_name", "touch /tmp/jash_qa_日本語 && ls /tmp/jash_qa_日本語 && rm /tmp/jash_qa_日本語", "", SEV_HIGH);

    /* ── Builtins-specific ── */
    add_test("export_list", "export | grep PATH", "PATH", SEV_LOW);
    add_test("history_cmd", "history | tail -1", "", SEV_LOW);
    add_test("type_cmd", "type cd", "builtin", SEV_LOW);
    add_test("help_cmd", "help | head -1", "", SEV_LOW);
    add_test("version_cmd", "version", "jash", SEV_LOW);
    add_test("clear_cmd", "clear", "", SEV_LOW);

    /* ── Job control (basic) ── */
    add_test("bg_fg", "sleep 0.01 & wait", "", SEV_MEDIUM);

    /* ── Redirect tests ── */
    add_test("redirect_stdout", "echo test > /tmp/jash_qa_redir && cat /tmp/jash_qa_redir", "test", SEV_MEDIUM);
    add_test("redirect_append", "echo a > /tmp/jash_qa_redir && echo b >> /tmp/jash_qa_redir && cat /tmp/jash_qa_redir", "a\nb", SEV_MEDIUM);
    add_test("redirect_stdin", "cat < /tmp/jash_qa_redir", "a\nb", SEV_MEDIUM);

    /* ── Arithmetic ── */
    add_test("let_arith", "echo $((5+3))", "8", SEV_MEDIUM);
    add_test("calc_cmd", "echo $((2+2))", "4", SEV_MEDIUM);

    /* ── Source/special ── */
    add_test("eval_cmd", "eval echo hello", "hello", SEV_MEDIUM);
    add_test("trap_cmd", "trap 'echo caught' INT; true", "", SEV_LOW);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * AGGRESSIVE TESTS — Heavy battery for dev mode
 * Tests Jash builtins specifically, including edge cases that break shells.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void generate_aggressive_tests(void) {
    /* Start with all basic tests */
    generate_tests();

    /* ═══ JASH BUILTINS: cd ═══ */
    add_test("ag_cd_home", "cd && pwd", "/", SEV_HIGH);
    add_test("ag_cd_dash", "cd /tmp && cd - && pwd", "/tmp", SEV_HIGH);
    add_test("ag_cd_dotdot", "mkdir -p /tmp/jash_qa/x/y && cd /tmp/jash_qa/x/y && cd ../.. && pwd", "/tmp/jash_qa", SEV_HIGH);
    add_test("ag_cd_nonexistent", "cd /nonexistent_$$$ 2>&1 | head -1", "", SEV_HIGH);

    /* ═══ JASH BUILTINS: export/unset ═══ */
    add_test("ag_export_basic", "export JASH_T1=hello && echo $JASH_T1", "hello", SEV_HIGH);
    add_test("ag_export_many", "export A=1 B=2 C=3 && echo $A$B$C", "123", SEV_HIGH);
    add_test("ag_export_overwrite", "export JASH_T2=old && export JASH_T2=new && echo $JASH_T2", "new", SEV_HIGH);
    add_test("ag_export_space_val", "export JASH_T3='hello world' && echo $JASH_T3", "hello world", SEV_HIGH);
    add_test("ag_unset_works", "export JASH_T4=delme && unset JASH_T4 && echo ${JASH_T4:-gone}", "gone", SEV_HIGH);
    add_test("ag_env_lists", "env | grep JASH_T1 | head -1", "JASH_T1=hello", SEV_MEDIUM);

    /* ═══ JASH BUILTINS: alias ═══ */
    add_test("ag_alias_basic", "alias jash_ta='echo aliased' && jash_ta", "aliased", SEV_HIGH);
    add_test("ag_alias_chain", "alias jash_tb='echo a' && alias jash_tc='jash_tb b' && jash_tc", "a b", SEV_HIGH);
    add_test("ag_unalias", "alias jash_td=delme && unalias jash_td && jash_td 2>&1 | head -1", "", SEV_HIGH);

    /* ═══ JASH BUILTINS: echo/printf ═══ */
    add_test("ag_echo_n", "echo -n test", "test", SEV_HIGH);
    add_test("ag_echo_backslash", "echo -e 'a\\nb'", "a\nb", SEV_HIGH);
    add_test("ag_echo_many", "echo a b c d e f g h i j k l m n o p", "a b c d e f g h i j k l m n o p", SEV_MEDIUM);
    add_test("ag_printf_format", "printf '%05d\\n' 42", "00042", SEV_HIGH);
    add_test("ag_printf_hex", "printf '%x\\n' 255", "ff", SEV_MEDIUM);

    /* ═══ JASH BUILTINS: type/help/version ═══ */
    add_test("ag_type_cd", "type cd", "builtin", SEV_HIGH);
    add_test("ag_type_ls", "type ls", "", SEV_MEDIUM);
    add_test("ag_help_output", "help | head -1", "", SEV_MEDIUM);
    add_test("ag_version_output", "version | head -1", "jash", SEV_HIGH);

    /* ═══ JASH BUILTINS: let/eval ═══ */
    add_test("ag_let_basic", "let x=5+3; echo $x", "8", SEV_HIGH);
    add_test("ag_let_string", "let y='hello'; echo $y", "hello", SEV_HIGH);
    add_test("ag_eval_echo", "eval echo evaluated", "evaluated", SEV_HIGH);
    add_test("ag_eval_concat", "eval echo he\"llo\"", "hello", SEV_HIGH);

    /* ═══ JASH BUILTINS: source/eval ═══ */
    add_test("ag_source_file", "echo 'export JASH_SRC=1' > /tmp/jash_qa_src.sh && source /tmp/jash_qa_src.sh && echo $JASH_SRC", "1", SEV_HIGH);

    /* ═══ JASH BUILTINS: read ═══ */
    add_test("ag_read_var", "echo input | read JASH_RD && echo $JASH_RD", "input", SEV_HIGH);

    /* ═══ JASH BUILTINS: shift ═══ */
    add_test("ag_shift", "set -- a b c; shift; echo $1 $2", "b c", SEV_HIGH);

    /* ═══ JASH BUILTINS: pushd/popd/dirs ═══ */
    add_test("ag_pushd_popd", "pushd /tmp > /dev/null && popd > /dev/null && pwd", "/", SEV_MEDIUM);
    add_test("ag_dirs", "dirs | head -1", "", SEV_LOW);

    /* ═══ JASH BUILTINS: jobs/fg/bg ═══ */
    add_test("ag_jobs_empty", "jobs | wc -l", "0", SEV_MEDIUM);

    /* ═══ PIPE stress ═══ */
    add_test("ag_pipe_chain", "echo hello | cat | cat | cat", "hello", SEV_HIGH);
    add_test("ag_pipe_grep", "echo -e 'a\\nb\\nc' | grep b", "b", SEV_HIGH);
    add_test("ag_pipe_wc", "echo -e 'a\\nb\\nc' | wc -l", "3", SEV_HIGH);
    add_test("ag_pipe_sort", "echo -e 'c\\na\\nb' | sort", "a\nb\nc", SEV_HIGH);
    add_test("ag_pipe_uniq", "echo -e 'a\\na\\nb' | sort | uniq", "a\nb", SEV_HIGH);

    /* ═══ REDIRECT stress ═══ */
    add_test("ag_redirect_out", "echo test > /tmp/jash_qa_ro && cat /tmp/jash_qa_ro", "test", SEV_HIGH);
    add_test("ag_redirect_append", "echo a > /tmp/jash_qa_ra && echo b >> /tmp/jash_qa_ra && cat /tmp/jash_qa_ra", "a\nb", SEV_HIGH);
    add_test("ag_redirect_in", "echo data > /tmp/jash_qa_ri && cat < /tmp/jash_qa_ri", "data", SEV_HIGH);
    add_test("ag_redirect_stderr", "ls /nonexistent_$$$ 2>&1 | head -1", "", SEV_HIGH);
    add_test("ag_redirect_null", "ls /nonexistent_$$$ 2>/dev/null; echo $?", "2", SEV_HIGH);

    /* ═══ SUBSTITUTION stress ═══ */
    add_test("ag_subst_dollar", "echo $(echo nested)", "nested", SEV_HIGH);
    add_test("ag_subst_backtick", "echo `echo backtick`", "backtick", SEV_MEDIUM);
    add_test("ag_subst_nested", "echo $(echo $(echo deep))", "deep", SEV_HIGH);
    add_test("ag_subst_arith", "echo $((2+3))", "5", SEV_HIGH);

    /* ═══ QUOTE stress ═══ */
    add_test("ag_quote_single", "echo 'hello world'", "hello world", SEV_HIGH);
    add_test("ag_quote_double", "echo \"hello world\"", "hello world", SEV_HIGH);
    add_test("ag_quote_mixed", "echo \"it's here\"", "it's here", SEV_HIGH);
    add_test("ag_quote_escape", "echo \"hello\\nworld\"", "hello\nworld", SEV_HIGH);
    add_test("ag_quote_dollar_in_single", "echo '$HOME'", "$HOME", SEV_HIGH);

    /* ═══ FILENAME edge cases ═══ */
    add_test("ag_file_dot", "touch /tmp/jash_qa_dot. && ls /tmp/jash_qa_dot. && rm /tmp/jash_qa_dot.", "", SEV_HIGH);
    add_test("ag_file_hidden", "touch /tmp/.jash_qa_hidden && ls -a /tmp/.jash_qa_hidden && rm /tmp/.jash_qa_hidden", "", SEV_HIGH);
    add_test("ag_file_long_name", "touch /tmp/jash_$(printf 'q%.0s' {1..200}) && ls /tmp/jash_q* | head -1 && rm /tmp/jash_q*", "", SEV_HIGH);
    add_test("ag_file_spaces", "touch '/tmp/jash file' && ls '/tmp/jash file' && rm '/tmp/jash file'", "", SEV_HIGH);

    /* ═══ CHAIN operators ═══ */
    add_test("ag_chain_semicolon", "true; true; echo ok", "ok", SEV_HIGH);
    add_test("ag_chain_and", "true && echo ok", "ok", SEV_HIGH);
    add_test("ag_chain_or", "false || echo ok", "ok", SEV_HIGH);
    add_test("ag_chain_mixed", "false && echo no || echo yes", "yes", SEV_HIGH);

    /* ═══ STRESS ═══ */
    add_test("ag_stress_seq_1k", "seq 1 1000 | wc -l", "1000", SEV_HIGH);
    add_test("ag_stress_seq_10k", "seq 1 10000 | wc -l", "10000", SEV_HIGH);
    add_test("ag_stress_cat_chain", "echo x | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat", "x", SEV_HIGH);
    add_test("ag_stress_deep_sub", "echo $(echo $(echo $(echo $(echo $(echo deep)))))", "deep", SEV_HIGH);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SANDBOX RUNNER — Executes tests in isolation
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Q3 fix (ADR-0017 wave 3): pure syscalls — no system("rm -rf"), no
 * shell quoting surface, works on OSes without /bin/sh. */
static void setup_sandbox(void) {
    osif_rmtree(QA_SANDBOX);
    mkdir(QA_SANDBOX, 0755);
}

static void cleanup_sandbox(void) {
    osif_rmtree(QA_SANDBOX);
}

static int run_single_test(test_case_t *t) {
    /* Build the test script */
    char script[2048];
    snprintf(script, sizeof(script),
        "#!/bin/sh\n"
        "cd %s\n"
        "export HOME=%s\n"
        "export PATH=/usr/local/bin:/usr/bin:/bin\n"
        "%s\n"
        "exit $?",
        QA_SANDBOX, QA_SANDBOX, t->command);

    /* Write script to temp file */
    char script_path[512]; /* qa_sandbox() returns at most 255 path bytes. */
    snprintf(script_path, sizeof(script_path), "%s/test.sh", QA_SANDBOX);
    FILE *sf = fopen(script_path, "w");
    if (!sf) return -1;
    fputs(script, sf);
    fclose(sf);
    chmod(script_path, 0755);

    /* Execute and capture output via OSIF spawn_capture: argv-exec,
     * no popen, no command-string interpolation, 10s hard timeout.
     * stdout+stderr are merged by spawn_capture itself. */
    const char* sh_argv[] = { "/bin/sh", script_path, NULL };
    char output[1024] = "";
    int rc = -1;
    int spawn_rc = bear_osinfo_spawn_capture(sh_argv, QA_SANDBOX,
                                             output, sizeof(output),
                                             10000, &rc);
    if (spawn_rc == -2) rc = 124;        /* timeout, GNU convention */
    else if (spawn_rc != 0) return -1;

    /* Store results — rc is the DIRECT exit code from spawn_capture
     * (not a wait status), so no WEXITSTATUS unwrap. */
    strlcpy(t->actual, output, sizeof(t->actual));
    t->exit_code = rc;

    /* Check if test passed */
    if (t->expected[0] == '\0') {
        /* No expected output — just check exit code */
        t->pass = (t->exit_code == 0) ? 1 : 0;
    } else {
        /* Check if expected substring is in actual output */
        t->pass = (strstr(t->actual, t->expected) != NULL) ? 1 : 0;
    }

    return t->pass;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JASH RUNNER — Runs tests INSIDE jash (child process)
 * Spawns jash as a child, feeds commands via stdin, captures stdout.
 * If jash crashes or hangs, parent kills it and reports the failure.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int run_jash_child(const char *jash_bin, const char *command, char *output, int outsz, int timeout_sec) {
    /* Q1 fix (ADR-0017 wave 3): exec jash DIRECTLY via the OSIF
     * spawn_capture — no /bin/sh middleman. Kills the quoting-injection
     * surface ('%s' interpolation) and works on OSes without a Bourne
     * shell. Sandbox env is inherited via setenv before spawn. */
    setenv("JASH_QA_HOME", QA_SANDBOX, 1);
    setenv("TERM", "dumb", 1);
    setenv("NO_COLOR", "1", 1);

    const char* argv[] = { jash_bin, "-c", command, NULL };
    int ec = -1;
    int rc = bear_osinfo_spawn_capture(argv, QA_SANDBOX,
                                       output, outsz,
                                       timeout_sec * 1000, &ec);
    if (rc == -2) return -2;   /* timeout */
    if (rc != 0)  return -1;
    return ec;
}

static int run_single_jash_test(test_case_t *t, const char *jash_bin) {
    char output[4096] = "";
    int rc = run_jash_child(jash_bin, t->command, output, sizeof(output), 10);

    strlcpy(t->actual, output, sizeof(t->actual));

    if (rc == -2) {
        /* Timeout */
        strlcpy(t->actual, "TIMEOUT after 10s", sizeof(t->actual));
        t->pass = 0;
        t->exit_code = -1;
        return 0;
    }
    if (rc == -3) {
        /* Crash */
        char msg[256];
        snprintf(msg, sizeof(msg), "CRASH (signal %d)", WTERMSIG(rc));
        strlcpy(t->actual, msg, sizeof(t->actual));
        t->pass = 0;
        t->exit_code = -1;
        return 0;
    }

    t->exit_code = rc;

    if (t->expected[0] == '\0') {
        t->pass = (rc == 0) ? 1 : 0;
    } else {
        t->pass = (strstr(t->actual, t->expected) != NULL) ? 1 : 0;
    }

    return t->pass;
}

static void run_all_tests(jash_writer_t w, void *ctx) {
    setup_sandbox();

    jash_writes(w, ctx, "\033[1;36m[QA] Rodando ");
    {
        char num[32];
        snprintf(num, sizeof(num), "%d", g_test_count);
        jash_writes(w, ctx, num);
    }
    jash_writes(w, ctx, " testes...\033[0m\r\n");

    int passed = 0, failed = 0, errors = 0;

    for (int i = 0; i < g_test_count; i++) {
        int result = run_single_test(&g_tests[i]);

        if (result == 1) {
            passed++;
            /* Show progress quietly */
        } else if (result == 0) {
            failed++;
            /* Record as bug */
            if (g_bug_count < MAX_BUGS) {
                bug_t *b = &g_bugs[g_bug_count++];
                strlcpy(b->test_name, g_tests[i].name, sizeof(b->test_name));
                strlcpy(b->command, g_tests[i].command, sizeof(b->command));
                snprintf(b->error, sizeof(b->error),
                    "Expected: '%.300s'\nActual: '%.650s'\nExit: %d",
                    g_tests[i].expected, g_tests[i].actual, g_tests[i].exit_code);
                b->sev = g_tests[i].sev;
            }

            char msg[512];
            snprintf(msg, sizeof(msg), "\033[31m  FAIL\033[0m %s\r\n", g_tests[i].name);
            jash_writes(w, ctx, msg);
        } else {
            errors++;
        }

        /* Progress indicator every 10 tests */
        if ((i + 1) % 10 == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "\033[90m  ... %d/%d\033[0m\r\n", i + 1, g_test_count);
            jash_writes(w, ctx, msg);
        }
    }

    cleanup_sandbox();

    /* Summary */
    jash_writes(w, ctx, "\r\n");
    {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "\033[1;32m  PASS: %d\033[0m  "
            "\033[1;31m  FAIL: %d\033[0m  "
            "\033[90m  ERR: %d\033[0m  "
            "TOTAL: %d\r\n",
            passed, failed, errors, g_test_count);
        jash_writes(w, ctx, msg);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MATURITY REPORT — Generates final QA report
 * ═══════════════════════════════════════════════════════════════════════════ */

static void generate_report(jash_writer_t w, void *ctx) {
    /* Ensure QA directory exists */
    mkdir(QA_DIR, 0755);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", tm);

    /* Calculate maturity score */
    int total_risks = g_risk_count;
    int critical = 0, high = 0, medium = 0, low = 0;
    for (int i = 0; i < g_risk_count; i++) {
        switch (g_risks[i].sev) {
            case SEV_CRITICAL: critical++; break;
            case SEV_HIGH:     high++; break;
            case SEV_MEDIUM:   medium++; break;
            case SEV_LOW:      low++; break;
            default: break;
        }
    }

    int total_tests = g_test_count;
    int passed = 0, failed = 0;
    for (int i = 0; i < g_test_count; i++) {
        if (g_tests[i].pass == 1) passed++;
        else if (g_tests[i].pass == 0) failed++;
    }

    int total_bugs = g_bug_count;
    int bugs_critical = 0, bugs_high = 0, bugs_medium = 0, bugs_low = 0;
    for (int i = 0; i < g_bug_count; i++) {
        switch (g_bugs[i].sev) {
            case SEV_CRITICAL: bugs_critical++; break;
            case SEV_HIGH:     bugs_high++; break;
            case SEV_MEDIUM:   bugs_medium++; break;
            case SEV_LOW:      bugs_low++; break;
            default: break;
        }
    }

    /* Maturity formula:
     * Start at 100, subtract for issues:
     * - Critical bug: -20
     * - High bug: -10
     * - Medium bug: -5
     * - Low bug: -1
     * - High risk pattern: -2
     * - Medium risk pattern: -1
     * Bonus for test coverage: +min(20, passed/total_tests * 20)
     */
    /* Maturity score: based ONLY on bugs found and test coverage.
     * Risk patterns are informational — they don't affect the score. */
    int score = 100;
    score -= bugs_critical * 25;
    score -= bugs_high * 10;
    score -= bugs_medium * 3;
    score -= bugs_low * 1;
    /* Bonus for test coverage */
    if (total_tests > 0) {
        int coverage = (passed * 100) / total_tests;
        score = score * coverage / 100;
    }
    if (score < 0) score = 0;
    if (score > 100) score = 100;

    /* Write report */
    FILE *f = fopen(QA_REPORT, "w");
    if (f) {
        fprintf(f, "# Jash Maturity Report\n\n");
        fprintf(f, "Generated: %s\n\n", date);
        fprintf(f, "## Maturity Score: %d/100\n\n", score);

        if (score >= 90) fprintf(f, "**Status: READY FOR LAUNCH** 🚀\n\n");
        else if (score >= 70) fprintf(f, "**Status: MOSTLY STABLE** — fix remaining issues\n\n");
        else if (score >= 50) fprintf(f, "**Status: NEEDS WORK** — significant issues remain\n\n");
        else fprintf(f, "**Status: NOT READY** — critical issues must be fixed\n\n");

        fprintf(f, "## Risk Analysis\n\n");
        fprintf(f, "| Severity | Count |\n|----------|-------|\n");
        fprintf(f, "| 🔴 Critical | %d |\n", critical);
        fprintf(f, "| 🟠 High | %d |\n", high);
        fprintf(f, "| 🟡 Medium | %d |\n", medium);
        fprintf(f, "| ⚪ Low | %d |\n", low);
        fprintf(f, "| **Total** | **%d** |\n\n", total_risks);

        fprintf(f, "## Test Results\n\n");
        fprintf(f, "| Metric | Value |\n|--------|-------|\n");
        fprintf(f, "| Tests Run | %d |\n", total_tests);
        fprintf(f, "| ✅ Passed | %d |\n", passed);
        fprintf(f, "| ❌ Failed | %d |\n", failed);
        fprintf(f, "| Coverage | %d%% |\n\n", total_tests > 0 ? (passed * 100 / total_tests) : 0);

        fprintf(f, "## Bugs Found\n\n");
        fprintf(f, "| Severity | Count |\n|----------|-------|\n");
        fprintf(f, "| 🔴 Critical | %d |\n", bugs_critical);
        fprintf(f, "| 🟠 High | %d |\n", bugs_high);
        fprintf(f, "| 🟡 Medium | %d |\n", bugs_medium);
        fprintf(f, "| ⚪ Low | %d |\n", bugs_low);
        fprintf(f, "| **Total** | **%d** |\n\n", total_bugs);

        if (g_bug_count > 0) {
            fprintf(f, "## Bug Details\n\n");
            for (int i = 0; i < g_bug_count; i++) {
                const char *sev_str = "LOW";
                switch (g_bugs[i].sev) {
                    case SEV_CRITICAL: sev_str = "🔴 CRITICAL"; break;
                    case SEV_HIGH:     sev_str = "🟠 HIGH"; break;
                    case SEV_MEDIUM:   sev_str = "🟡 MEDIUM"; break;
                    case SEV_LOW:      sev_str = "⚪ LOW"; break;
                    default: break;
                }
                fprintf(f, "### %s [%s]\n", g_bugs[i].test_name, sev_str);
                fprintf(f, "**Command:** `%s`\n\n", g_bugs[i].command);
                fprintf(f, "**Error:**\n```\n%s\n```\n\n", g_bugs[i].error);
            }
        }

        if (g_risk_count > 0) {
            fprintf(f, "## Risk Patterns Found\n\n");
            fprintf(f, "| File | Line | Pattern | Severity | Description |\n");
            fprintf(f, "|------|------|---------|----------|-------------|\n");
            for (int i = 0; i < g_risk_count && i < 50; i++) {
                const char *sev_str = "LOW";
                switch (g_risks[i].sev) {
                    case SEV_CRITICAL: sev_str = "🔴"; break;
                    case SEV_HIGH:     sev_str = "🟠"; break;
                    case SEV_MEDIUM:   sev_str = "🟡"; break;
                    case SEV_LOW:      sev_str = "⚪"; break;
                    default: break;
                }
                fprintf(f, "| %s | %d | `%s` | %s | %s |\n",
                    g_risks[i].file, g_risks[i].line,
                    g_risks[i].pattern, sev_str, g_risks[i].desc);
            }
        }

        fclose(f);
    }

    /* Print summary to terminal */
    jash_writes(w, ctx, "\r\n\033[1;36m═══ QA MATURITY REPORT ═══\033[0m\r\n\r\n");

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "  \033[1mMaturity Score:\033[0m \033[1;36m%d/100\033[0m\r\n\r\n", score);
        jash_writes(w, ctx, msg);
    }

    if (score >= 90) {
        jash_writes(w, ctx, "  \033[1;32mREADY FOR LAUNCH\033[0m\r\n\r\n");
    } else if (score >= 70) {
        jash_writes(w, ctx, "  \033[1;33mMOSTLY STABLE\033[0m — fix remaining issues\r\n\r\n");
    } else {
        jash_writes(w, ctx, "  \033[1;31mNEEDS WORK\033[0m — significant issues remain\r\n\r\n");
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "  Risks: %d (🔴%d 🟠%d 🟡%d ⚪%d)\r\n"
            "  Tests: %d (✅%d ❌%d)\r\n"
            "  Bugs:  %d (🔴%d 🟠%d 🟡%d ⚪%d)\r\n"
            "  Report: %s\r\n",
            total_risks, critical, high, medium, low,
            total_tests, passed, failed,
            total_bugs, bugs_critical, bugs_high, bugs_medium, bugs_low,
            QA_REPORT);
        jash_writes(w, ctx, msg);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════════════════ */

void jash_qa_scan(jash_writer_t w, void *ctx, const char *src_dir) {
    QA_ENSURE_OR_FAIL(w, ctx);
    g_risk_count = 0;
    jash_writes(w, ctx, "\033[1;36m[QA] Scanning source code for risk patterns...\033[0m\r\n");
    scan_directory(src_dir, w, ctx);

    char msg[256];
    snprintf(msg, sizeof(msg), "\033[32m[QA] Found %d risk patterns\033[0m\r\n", g_risk_count);
    jash_writes(w, ctx, msg);
}

void jash_qa_test(jash_writer_t w, void *ctx) {
    QA_ENSURE_OR_FAIL(w, ctx);
    g_test_count = 0;
    g_bug_count = 0;
    jash_writes(w, ctx, "\033[1;36m[QA] Generating test cases...\033[0m\r\n");
    generate_tests();

    char msg[256];
    snprintf(msg, sizeof(msg), "\033[32m[QA] Generated %d test cases\033[0m\r\n", g_test_count);
    jash_writes(w, ctx, msg);

    run_all_tests(w, ctx);
}

void jash_qa_aggressive(jash_writer_t w, void *ctx, const char *jash_bin) {
    QA_ENSURE_OR_FAIL(w, ctx);
    g_test_count = 0;
    g_bug_count = 0;

    jash_writes(w, ctx, "\033[1;31m╔══════════════════════════════════════╗\r\n");
    jash_writes(w, ctx, "║  JASH AGGRESSIVE QA — DEV MODE ONLY  ║\r\n");
    jash_writes(w, ctx, "║  Runs jash inside jash with timeout   ║\r\n");
    jash_writes(w, ctx, "╚══════════════════════════════════════╝\033[0m\r\n\r\n");

    jash_writes(w, ctx, "\033[1;36m[QA] Generating aggressive test cases...\033[0m\r\n");
    generate_aggressive_tests();

    char msg[256];
    snprintf(msg, sizeof(msg), "\033[32m[QA] Generated %d aggressive tests\033[0m\r\n", g_test_count);
    jash_writes(w, ctx, msg);

    /* Find jash binary */
    char jash_path[512];
    if (jash_bin[0] == '/') {
        strlcpy(jash_path, jash_bin, sizeof(jash_path));
    } else {
        /* Try to find jash in build directory */
        const char *home = getenv("HOME");
        if (home) {
            snprintf(jash_path, sizeof(jash_path), "%s/refatorados/Jash/build/jash-release", home);
        } else {
            strlcpy(jash_path, "./build/jash-release", sizeof(jash_path));
        }
    }

    /* Check if jash binary exists */
    struct stat st;
    if (stat(jash_path, &st) != 0) {
        jash_writes(w, ctx, "\033[31m[QA] Jash binary not found at: ");
        jash_writes(w, ctx, jash_path);
        jash_writes(w, ctx, "\033[0m\r\n");
        jash_writes(w, ctx, "\033[90m[QA] Run 'make' first, or specify path: ? qa aggressive /path/to/jash\033[0m\r\n");
        return;
    }

    setup_sandbox();

    jash_writes(w, ctx, "\033[1;36m[QA] Running ");
    snprintf(msg, sizeof(msg), "%d", g_test_count);
    jash_writes(w, ctx, msg);
    jash_writes(w, ctx, " tests inside jash child process...\033[0m\r\n");
    jash_writes(w, ctx, "\033[90m[QA] Binary: ");
    jash_writes(w, ctx, jash_path);
    jash_writes(w, ctx, "\033[0m\r\n\r\n");

    int passed = 0, failed = 0, crashed = 0, timed_out = 0;

    for (int i = 0; i < g_test_count; i++) {
        int result = run_single_jash_test(&g_tests[i], jash_path);

        if (result == 1) {
            passed++;
        } else {
            failed++;
            /* Record as bug */
            if (g_bug_count < MAX_BUGS) {
                bug_t *b = &g_bugs[g_bug_count++];
                strlcpy(b->test_name, g_tests[i].name, sizeof(b->test_name));
                strlcpy(b->command, g_tests[i].command, sizeof(b->command));
                snprintf(b->error, sizeof(b->error),
                    "Expected: '%.300s'\nActual: '%.650s'\nExit: %d",
                    g_tests[i].expected, g_tests[i].actual, g_tests[i].exit_code);
                b->sev = g_tests[i].sev;
            }

            /* Count crash vs fail */
            if (strstr(g_tests[i].actual, "CRASH")) crashed++;
            else if (strstr(g_tests[i].actual, "TIMEOUT")) timed_out++;

            char fail_msg[512];
            snprintf(fail_msg, sizeof(fail_msg), "\033[31m  FAIL\033[0m %.127s \033[90m(%.300s)\033[0m\r\n",
                g_tests[i].name, g_tests[i].actual);
            jash_writes(w, ctx, fail_msg);
        }

        /* Progress */
        if ((i + 1) % 10 == 0) {
            char prog[128];
            snprintf(prog, sizeof(prog), "\033[90m  ... %d/%d (✅%d ❌%d)\033[0m\r\n",
                i + 1, g_test_count, passed, failed);
            jash_writes(w, ctx, prog);
        }
    }

    cleanup_sandbox();

    /* Summary */
    jash_writes(w, ctx, "\r\n");
    {
        char sum[512];
        snprintf(sum, sizeof(sum),
            "\033[1;32m  PASS: %d\033[0m  "
            "\033[1;31m  FAIL: %d\033[0m  "
            "\033[1;31m  CRASH: %d\033[0m  "
            "\033[1;33m  TIMEOUT: %d\033[0m  "
            "TOTAL: %d\r\n",
            passed, failed, crashed, timed_out, g_test_count);
        jash_writes(w, ctx, sum);
    }

    generate_report(w, ctx);
}

void jash_qa_run(jash_writer_t w, void *ctx, const char *src_dir) {
    QA_ENSURE_OR_FAIL(w, ctx);
    jash_writes(w, ctx, "\033[1;36m╔══════════════════════════════════╗\r\n");
    jash_writes(w, ctx, "║   JASH SELF-HEALING QA ENGINE    ║\r\n");
    jash_writes(w, ctx, "╚══════════════════════════════════╝\033[0m\r\n\r\n");

    /* Phase 1: Source scan */
    jash_qa_scan(w, ctx, src_dir);

    /* Phase 2-3: Generate and run tests */
    jash_qa_test(w, ctx);

    /* Phase 4: Generate report */
    generate_report(w, ctx);
}

void jash_qa_status(jash_writer_t w, void *ctx) {
    /* Check if report exists */
    FILE *f = fopen(QA_REPORT, "r");
    if (f) {
        jash_writes(w, ctx, "\033[90m[QA] Last report: ");
        char line[256];
        int line_num = 0;
        while (fgets(line, sizeof(line), f) && line_num < 5) {
            /* Print first few lines of report */
            int len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            jash_writes(w, ctx, line);
            jash_writes(w, ctx, "\r\n");
            line_num++;
        }
        fclose(f);
    } else {
        jash_writes(w, ctx, "\033[90m[QA] No report yet. Run '? qa' to start.\033[0m\r\n");
    }
}
