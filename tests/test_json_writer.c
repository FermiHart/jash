// Fault-driven tests for the allocation-free JSON sink boundary.
#include "core/json_writer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { char data[16384]; size_t used; int calls, fail, interrupted, fail_after; } sink_t;
static long sink(void* context, const char* data, size_t n) {
    sink_t* s = context;
    s->calls++;
    if (!s->interrupted) { s->interrupted = 1; errno = EINTR; return -1; }
    if (s->fail && (!s->fail_after || s->calls > s->fail_after)) {
        if (s->fail == 1) { errno = ENOSPC; return -1; }
        if (s->fail == 2) return 0;
        return (long)n + 1;
    }
    if (n > 7) n = 7;
    if (s->used + n > sizeof(s->data)) { errno = ENOSPC; return -1; }
    memcpy(s->data + s->used, data, n);
    s->used += n;
    return (long)n;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed at line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static int faults(void) {
    sink_t s = {0};
    jash_json_writer_t w;
    jash_jsonw_init(&w, sink, &s);
    jash_jsonw_string(&w, "a\"b\\c\n\t");
    CHECK(jash_jsonw_finish(&w) == 0);
    const char* expected = "\"a\\\"b\\\\c\\u000a\\u0009\"";
    CHECK(s.used == strlen(expected) && !memcmp(s.data, expected, s.used));
    CHECK(s.calls > 2 && s.interrupted);
    for (int fail = 1; fail <= 3; fail++) {
        memset(&s, 0, sizeof(s)); s.fail = fail;
        jash_jsonw_init(&w, sink, &s);
        jash_jsonw_string(&w, "not committed");
        CHECK(jash_jsonw_finish(&w) == -1);
        CHECK(errno == (fail == 1 ? ENOSPC : EIO));
        int calls = s.calls;
        jash_jsonw_raw(&w, "false");
        CHECK(jash_jsonw_finish(&w) == -1 && s.calls == calls);
    }
    char long_text[6001];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;
    memset(&s, 0, sizeof(s)); s.fail = 1; s.fail_after = 12;
    jash_jsonw_init(&w, sink, &s);
    jash_jsonw_string(&w, long_text);
    CHECK(jash_jsonw_finish(&w) == -1 && errno == ENOSPC);
    CHECK(s.used > 0 && s.used < strlen(long_text));
    size_t written = s.used;
    jash_jsonw_raw(&w, "false");
    CHECK(jash_jsonw_finish(&w) == -1 && s.used == written);
    const char* invalid[] = {"\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f", "\x80", NULL};
    for (int i = 0; invalid[i]; i++) {
        char* exact = strdup(invalid[i]);
        CHECK(exact != NULL);
        memset(&s, 0, sizeof(s));
        jash_jsonw_init(&w, sink, &s);
        jash_jsonw_string(&w, exact);
        CHECK(jash_jsonw_finish(&w) == -1 && errno == EILSEQ && s.calls == 0);
        free(exact);
    }
    jash_jsonw_init(&w, NULL, NULL);
    CHECK(jash_jsonw_finish(&w) == -1 && errno == EINVAL);
    puts("json writer fault checks passed");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 1) return faults();
    static unsigned char data[1024 * 1024 + 1];
    size_t n = fread(data, 1, sizeof(data) - 1, stdin);
    if (ferror(stdin) || !feof(stdin)) return 2;
    data[n] = 0;
    int fd = STDOUT_FILENO;
    jash_json_writer_t w;
    jash_jsonw_init(&w, jash_jsonw_fd_sink, &fd);
    if (!strcmp(argv[1], "bytes")) jash_jsonw_bytes(&w, data, n);
    else if (!strcmp(argv[1], "text")) {
        if (memchr(data, 0, n)) return 2;
        jash_jsonw_string(&w, (const char*)data);
    } else if (!strcmp(argv[1], "u64")) jash_jsonw_u64(&w, UINT64_MAX);
    else return 2;
    jash_jsonw_raw(&w, "\n");
    return jash_jsonw_finish(&w) == 0 ? 0 : 1;
}
