// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Execute real native handlers with deterministic result-channel failures.
#include <builtins/builtin.h>
#include <core/timeline.h>
#include <core/data_value.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

void jash_budget_unwrap(jash_writer_t* w, void** ctx) { (void)w; (void)ctx; }
void jash_redir_stream_writer(jash_writer_t w, void* ctx) { (void)w; (void)ctx; }
void jash_timeline_record(jash_undo_kind_t kind, const char* desc, const char* a, const char* b) {
    (void)kind; (void)desc; (void)a; (void)b;
}
int jash_timeline_record_ref(jash_undo_kind_t kind, const char* desc, const char* a, const char* b,
                              jash_recovery_ref_t* out) {
    (void)kind; (void)desc; (void)a; (void)b;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}
long jash_stderr_writer(void* ctx, const char* text, size_t n) { (void)ctx; (void)text; return (long)n; }
long jash_writef(jash_writer_t w, void* ctx, const char* fmt, ...) { (void)w; (void)ctx; (void)fmt; return 0; }

static int calls;
static long failed_result(void* ctx, const char* text, size_t n) {
    (void)ctx; (void)text;
    if (++calls > 1) { errno = ENOSPC; return -1; }
    return (long)n;
}

typedef struct { int magic, header, action; unsigned tag; size_t payload; } native_consumer_t;
static long failed_native_action(void* context, const char* text, size_t n) {
    native_consumer_t* consumer = context;
    if (!consumer->magic) {
        assert(n == 4 && !memcmp(text, "JTV2", 4)); consumer->magic = 1; return (long)n;
    }
    if (!consumer->payload) {
        assert(n == 9);
        consumer->tag = (unsigned char)text[0];
        for (size_t i = 1; i < n; i++) consumer->payload = (consumer->payload << 8) | (unsigned char)text[i];
        assert(consumer->payload > 0); return (long)n;
    }
    assert(n == consumer->payload); consumer->payload = 0;
    if (consumer->tag == 'I') {
        assert(n > 12 && !memcmp(text + 8, "JDV1", 4));
        unsigned char storage[16384]; jdv_arena_t arena; jdv_arena_init(&arena, storage, sizeof(storage));
        jdv_value_t row; size_t consumed = 0;
        assert(jdv_decode_exact(&arena, text + 8, n - 8, &row, &consumed) == 0 && consumed == n - 8);
        assert(row.kind == JDV_RECORD);
        const jdv_value_t* kind = NULL;
        for (size_t i = 0; i < row.as.record.len; i++)
            if (row.as.record.fields[i].key.len == 4 && !memcmp(row.as.record.fields[i].key.data, "kind", 4))
                kind = &row.as.record.fields[i].value;
        assert(kind && kind->kind == JDV_SYMBOL);
        if (kind->as.slice.len == 6 && !memcmp(kind->as.slice.data, "action", 6)) {
            consumer->action++; errno = EPIPE; return -1;
        }
        assert(kind->as.slice.len == 6 && !memcmp(kind->as.slice.data, "header", 6));
        consumer->header++;
    }
    return (long)n;
}

int main(void) {
    char root[] = "/tmp/jash-meta-unit-XXXXXX";
    assert(mkdtemp(root) && chdir(root) == 0);
    char* dirs[] = {"--json", "-p", "first/child", "second", NULL};
    assert(jash_b_mkdir(NULL, failed_result, NULL, 4, dirs) == 125);
    struct stat st;
    assert(stat("first", &st) == 0 && S_ISDIR(st.st_mode));
    assert(lstat("first/child", &st) != 0 && errno == ENOENT);
    assert(lstat("second", &st) != 0 && errno == ENOENT);
    assert(mkdir("first/child", 0700) == 0);
    char* chmod_args[] = {"--json", "-R", "0755", "first", NULL};
    calls = 0;
    assert(jash_b_chmod(NULL, failed_result, NULL, 4, chmod_args) == 125);
    assert(stat("first", &st) == 0 && (st.st_mode & 0777) == 0755);
    assert(stat("first/child", &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(rmdir("first/child") == 0 && rmdir("first") == 0);
    char* native_dirs[] = {"--data", "-p", "native/child", "later", NULL};
    calls = 1; /* Admission sink fails before any requested effect. */
    assert(jash_b_mkdir(NULL, failed_result, NULL, 4, native_dirs) == 125);
    assert(lstat("native", &st) != 0 && errno == ENOENT);
    native_consumer_t consumer = {0};
    assert(jash_b_mkdir(NULL, failed_native_action, &consumer, 4, native_dirs) == 125);
    assert(consumer.magic && consumer.header == 1 && consumer.action == 1);
    assert(stat("native", &st) == 0 && S_ISDIR(st.st_mode));
    assert(lstat("native/child", &st) != 0 && errno == ENOENT);
    assert(lstat("later", &st) != 0 && errno == ENOENT);
    assert(rmdir("native") == 0);
    assert(chdir("/") == 0 && rmdir(root) == 0);
    return 0;
}
