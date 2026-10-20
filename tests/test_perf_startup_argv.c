/* Focused, isolated test: link with core/argv.c, builtins/registry.c and
 * parser/cmdsub.c using -ffunction-sections -fdata-sections -Wl,--gc-sections
 * -Wl,--wrap=malloc,--wrap=realloc,--wrap=free and -Ic. No shell build needed. */
#include <core/argv.h>
#include <builtins/builtin.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* __real_malloc(size_t n);
void* __real_realloc(void* p, size_t n);
void __real_free(void* p);
static int allocations, frees, live_allocations, fail_at;

void* __wrap_malloc(size_t n) {
    if (++allocations == fail_at) return NULL;
    void* p = __real_malloc(n);
    if (p) live_allocations++;
    return p;
}

void* __wrap_realloc(void* p, size_t n) {
    if (++allocations == fail_at) return NULL;
    int was_null = p == NULL;
    void* next = __real_realloc(p, n);
    if (next && was_null) live_allocations++;
    return next;
}

void __wrap_free(void* p) {
    frees++;
    if (p) live_allocations--;
    __real_free(p);
}

/* Only the dispatch diagnostic needs a writer; keep unrelated shell IO out. */
long jash_writef(jash_writer_t w, void* ctx, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return w(ctx, buf, (size_t)n);
}

typedef struct fixture {
    const char* line;
    const char* const* expected;
    int argc;
    struct fixture* nested;
    int calls, diagnostics;
} fixture_t;

static long diagnostic(void* ctx, const char* buf, size_t n) {
    fixture_t* f = ctx;
    assert(n && strstr(buf, "unterminated quote"));
    f->diagnostics++;
    return (long)n;
}

static void check_words(fixture_t* f, int argc, char** argv) {
    if (argc != f->argc)
        fprintf(stderr, "argc: expected %d, got %d for %s\n", f->argc, argc, f->line);
    assert(argc == f->argc);
    if (argv) assert(argv[argc] == NULL);
    for (int i = 0; i < argc; i++) assert(!strcmp(argv[i], f->expected[i]));
}

static const jash_builtin_t test_builtin;

static int check_builtin(jash_state_t* s, jash_writer_t w, void* ctx,
                         int argc, char** argv) {
    fixture_t* f = ctx;
    (void)s;
    (void)w;
    check_words(f, argc, argv);
    f->calls++;
    if (f->nested) {
        assert(jash_builtin_dispatch(&test_builtin, NULL, diagnostic,
                                    f->nested, f->nested->line) == 17);
        check_words(f, argc, argv);  /* Inner dispatch must not clobber outer. */
    }
    return 17;
}

static const jash_builtin_t test_builtin = {
    "argv-test", NULL, NULL, check_builtin
};

static void run_case(fixture_t* f, int expected_allocations) {
    allocations = 0;
    frees = 0;
    assert(jash_builtin_dispatch(&test_builtin, NULL, diagnostic,
                                f, f->line) == 17);
    assert(!live_allocations);
    if (!expected_allocations) assert(!frees);
    if (allocations != expected_allocations) {
        fprintf(stderr, "allocation count: expected %d, got %d\n",
                expected_allocations, allocations);
        abort();
    }
}

int main(void) {
    fixture_t empty = { .line = "" };
    run_case(&empty, 0);
    empty.line = NULL;
    run_case(&empty, 0);
    empty.line = " \t\n\036";
    run_case(&empty, 0);

    const char* words[] = { "", "", "a b", "c d", "prepost", "a b", "a\nb" };
    fixture_t quoted = {
        .line = "'' \"\" 'a b' \"c d\" pre\"\"post a\\ b a\\\nb",
        .expected = words, .argc = 7
    };
    run_case(&quoted, 0);
    const char* escaped[] = { "$\"\\", "ab", "\\q", "a'\"b" };
    fixture_t escapes = {
        .line = "\"\\$\\\"\\\\\" \"a\\\nb\" \"\\q\" \"a'\"'\"b'",
        .expected = escaped, .argc = 4
    };
    run_case(&escapes, 0);

    const char* array_words[] = { "", "a b", "", "c", "", "tail" };
    fixture_t array = {
        .line = "\"\036a b\036\036c\036\" tail",
        .expected = array_words, .argc = 6
    };
    run_case(&array, 0);
    const char* shielded_words[] = { "\"'\\ \t\n|<>&;()~" };
    fixture_t shielded = {
        .line = "\003\004\005\006\016\017\022\023\024\025\026\027\030\031",
        .expected = shielded_words, .argc = 1
    };
    run_case(&shielded, 0);

    char long_word[513];
    memset(long_word, 'x', sizeof long_word - 1);
    long_word[511] = '\0';
    const char* long_words[] = { long_word };
    fixture_t long_arg = { .line = long_word, .expected = long_words, .argc = 1 };
    run_case(&long_arg, 0);  /* 511 bytes plus NUL fit the local backing. */
    long_word[511] = 'x';
    long_word[512] = '\0';
    run_case(&long_arg, 1);  /* Only backing spills, not the pointer vector. */

    char many[1024];
    const char* many_words[128];
    for (int n = 1; n <= 128; n++) {
        many[2 * n - 2] = 'x';
        many[2 * n - 1] = '\0';
        many_words[n - 1] = "x";
        fixture_t f = { .line = many, .expected = many_words, .argc = n };
        if (n == 31 || n == 32 || n == 63 || n == 64 || n == 128)
            run_case(&f, n < 32 ? 0 : 1);
        many[2 * n - 1] = ' ';
    }

    char both[1024];
    memcpy(both, long_word, 512);
    both[512] = ' ';
    many[255] = '\0';
    strcpy(both + 513, many);
    const char* both_words[129];
    both_words[0] = long_word;
    for (int i = 1; i < 129; i++) both_words[i] = "x";
    fixture_t both_case = { .line = both, .expected = both_words, .argc = 129 };
    run_case(&both_case, 2);  /* Both buffers spill, still only two allocations. */

    /* Dense quoted RS boundaries each produce an empty word. */
    char dense[132];
    dense[0] = '"';
    memset(dense + 1, '\036', 128);
    dense[129] = '"';
    dense[130] = '\0';
    const char* dense_words[129];
    for (int i = 0; i < 129; i++) dense_words[i] = "";
    fixture_t dense_case = { .line = dense, .expected = dense_words, .argc = 129 };
    run_case(&dense_case, 2);
    jash_argv_t legacy;
    assert(jash_argv_build(dense, &legacy) == 129);
    check_words(&dense_case, legacy.argc, legacy.argv);
    jash_argv_free(&legacy);
    jash_argv_free(&legacy);
    assert(!legacy.argc && !legacy.argv && !legacy.backing && !live_allocations);

    quoted.nested = &long_arg;
    long_arg.nested = &array;
    run_case(&quoted, 1);  /* stack -> heap -> stack, all argv remain live. */
    quoted.nested = NULL;
    long_arg.nested = NULL;

    fixture_t invalid = { .line = "'unterminated" };
    assert(jash_builtin_dispatch(&test_builtin, NULL, diagnostic,
                                &invalid, invalid.line) == 2);
    assert(!invalid.calls && invalid.diagnostics == 1 && !live_allocations);
    dense[129] = '\0';
    dense_case.argc = 128;
    run_case(&dense_case, 2);  /* Preserve existing handling of a final RS. */
    dense[128] = 'x';
    invalid.line = dense;
    assert(jash_builtin_dispatch(&test_builtin, NULL, diagnostic,
                                &invalid, invalid.line) == 2);
    assert(!invalid.calls && invalid.diagnostics == 2 && !live_allocations);

    /* Fail backing allocation, vector spill and later realloc independently. */
    for (int i = 0; i < 4; i++) {
        invalid.line = i == 0 ? long_word : i == 1 ? many : i == 2 ? both : dense;
        for (int nth = 1; nth <= (i < 2 ? 1 : 2); nth++) {
            allocations = 0;
            fail_at = nth;
            assert(jash_builtin_dispatch(&test_builtin, NULL, diagnostic,
                                        &invalid, invalid.line) == 2);
            assert(allocations == nth && !live_allocations && !invalid.calls);
            fail_at = 0;
        }
    }
    for (int nth = 1; nth <= 2; nth++) {
        allocations = 0;
        fail_at = nth;
        assert(jash_argv_build(both, &legacy) == -1);
        assert(!legacy.argc && !legacy.argv && !legacy.backing && !live_allocations);
        jash_argv_free(&legacy);
        fail_at = 0;
    }
    puts("PASS: argv allocation boundaries, quotes, RS, sentinels, nested dispatch and cleanup");
    return 0;
}
