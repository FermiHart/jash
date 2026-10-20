// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Compare the orchestration with its original stages, including error state.
 * The no-copy check is a deterministic performance gate, not a timing test. */
#include <parser/expand.h>
#include <core/env.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_alloc;
static unsigned allocations, comparisons;
static const char* home_value = "/home/fixture";
static void* checked_malloc(size_t n) {
    allocations++;
    return fail_alloc ? NULL : malloc(n);
}
const char* jash_env_get(const jash_state_t* s, const char* key) {
    (void)s;
    return !strcmp(key, "HOME") ? home_value : NULL;
}

#define malloc checked_malloc
#include "parser/expand.c"
#undef malloc

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static jash_state_t state;
static jash_expand_ctx_t reference, actual;

static const char* original_stages(const char* line, jash_expand_ctx_t* ctx) {
    if (strlen(line) + 1 >= sizeof(ctx->glob_out)) return line;
    const char* p = jash_expand_ansi_c(line, ctx);
    if (!p) p = line;
    p = jash_expand_tilde(&state, p, ctx);
    p = jash_expand_braces(p, ctx);
    if (!state.shell_opts.noglob) {
        ctx->nullglob = state.shell_opts.nullglob;
        p = jash_expand_glob(p, ctx);
    }
    return p;
}

static void seed_error(jash_expand_ctx_t* ctx) {
    memset(ctx, 0x5a, sizeof(*ctx));
    ctx->nullglob = 7;
    ctx->glob_error = 1;
    strcpy(ctx->glob_error_msg, "old context error");
    g_glob_error = 1;
    strcpy(g_glob_error_msg, "old global error");
}

static void compare(const char* line) {
    seed_error(&reference);
    const char* wanted = original_stages(line, &reference);
    int wanted_error = g_glob_error;
    char message[sizeof(g_glob_error_msg)];
    strcpy(message, g_glob_error_msg);
    seed_error(&actual);
    const char* got = jash_expand_word_layer(&state, line, &actual);
    CHECK(!strcmp(got, wanted));
    CHECK(g_glob_error == wanted_error && !strcmp(message, g_glob_error_msg));
    CHECK(actual.glob_error == reference.glob_error);
    CHECK(!strcmp(actual.glob_error_msg, reference.glob_error_msg));
    CHECK(actual.nullglob == reference.nullglob);
    comparisons++;
}

static void no_copy(void) {
    const char* line = "test 'alpha beta gamma' = 'alpha beta gamma'";
    seed_error(&actual);
    allocations = 0;
    fail_alloc = 1;
    const char* got = jash_expand_word_layer(&state, line, &actual);
    fail_alloc = 0;
    CHECK(!strcmp(got, line));
    CHECK(!g_glob_error && !actual.glob_error && !actual.glob_error_msg[0]);
    CHECK(!allocations);
    const unsigned char* bytes = (const unsigned char*)&actual;
    for (size_t i = 0; i < offsetof(jash_expand_ctx_t, nullglob); i++) CHECK(bytes[i] == 0x5a);
    puts("PASS inert text leaves all expansion scratch buffers untouched, even under OOM");
}

int main(int argc, char** argv) {
    CHECK(argc >= 2 && chdir(argv[1]) == 0);
    if (argc == 3) { no_copy(); return 0; }
    FILE* fixture = fopen("match-one", "w");
    CHECK(fixture && fclose(fixture) == 0);
    fixture = fopen("match two", "w");
    CHECK(fixture && fclose(fixture) == 0);
    const char* cases[] = {
        "", "test x = x", "echo '' \"\"", "echo 'a b' \"c d\"", "echo 'unclosed",
        "echo \"unclosed", "echo escaped\\ space", "echo \\*", "echo match*", "echo missing*",
        "echo [am]*", "echo match?one", "echo @(match-one|missing)", "echo {a,b}",
        "echo {1..4}", "echo '{a,b}'", "echo ~", "export V=~/bin:~/lib",
        "echo $'a\\tb'", "echo $'a\\x2ab'", "echo $'a\\'b'", "echo '$V'", "echo \"$V\"",
        "echo 'a\nb'", "echo a\tb", "echo \xc3\xa7", "echo a >out", "echo a | cat", NULL
    };
    for (int noglob = 0; noglob < 2; noglob++) {
        state.shell_opts.noglob = noglob;
        for (int nullglob = 0; nullglob < 2; nullglob++) {
            state.shell_opts.nullglob = nullglob;
            for (size_t i = 0; cases[i]; i++) compare(cases[i]);
            for (unsigned c = 1; c < 256; c++) {
                char line[] = "echo 'x_ y'";
                line[7] = (char)c;
                compare(line);
            }
        }
    }
    state.shell_opts.noglob = 0;
    state.shell_opts.nullglob = 0;
    char mutable[80];
    strcpy(mutable, "echo plain"); compare(mutable);
    strcpy(mutable, "echo match*"); compare(mutable);
    strcpy(mutable, "echo ~"); home_value = "/first"; compare(mutable);
    home_value = "/second"; compare(mutable);
    /* The fallback still refuses allocation failures, and a later inert call
     * clears the same glob error state as the original pipeline. */
    fail_alloc = 1;
    compare("echo match*");
    compare("echo plain");
    fail_alloc = 0;
    /* Existing size refusal precedes both expansion and error-state cleanup. */
    static char long_line[JASH_LINE_CAP_STACK + 1];
    memset(long_line, 'x', sizeof(long_line) - 1);
    compare(long_line);
    long_line[JASH_LINE_CAP_STACK - 1] = '\0'; compare(long_line);
    long_line[JASH_LINE_CAP_STACK - 2] = '\0'; compare(long_line);
    no_copy();
    printf("word layer: %u reference comparisons passed\n", comparisons);
    return 0;
}
