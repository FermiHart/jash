// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inject failures only into env's allocations, not libc's mirror. */
static int allocation_calls, fail_allocation = -1;
static void* env_test_malloc(size_t size) {
    if (allocation_calls++ == fail_allocation) return NULL;
    return malloc(size);
}
static void* env_test_calloc(size_t count, size_t size) {
    if (allocation_calls++ == fail_allocation) return NULL;
    return calloc(count, size);
}
#define malloc env_test_malloc
#define calloc env_test_calloc
#include "../c/core/env.c"
#undef malloc
#undef calloc

/* Include the scanner to check stored hashes against full-name FNV, not just
 * two walks that could share the same hashing mistake. */
#include "../c/core/fschange.c"

static const char* ledger_path;
int jash_memo_ledger_lookup(const char* path, unsigned long* mt,
                            unsigned long* ts) {
    if (!ledger_path || strcmp(path, ledger_path) != 0) return 0;
    *mt = 1;
    *ts = 42;
    return 1;
}
void jash_world_scan_free(jash_world_scan_t* scan) {
    if (!scan) return;
    free(scan->entries);
    free(scan);
}

static jash_state_t* new_state(void) {
    jash_state_t* s = calloc(1, sizeof(*s));
    assert(s);
    for (int i = 0; i < JASH_ENV_HASH_SIZE; i++) s->env_hash[i] = -1;
    jash_env_array_index_init(s);
    return s;
}

static void check_value(jash_state_t* s, const char* key, const char* value) {
    const char* stored = jash_env_get(s, key);
    const char* mirrored = getenv(key);
    assert(stored && mirrored);
    assert(strcmp(stored, value) == 0 && strcmp(mirrored, value) == 0);
}

static void test_positionals(void) {
    jash_state_t* s = new_state();
    assert(jash_env_set(s, "0", "shell") == 0);
    assert(jash_env_set(s, "01", "not-canonical") == 0);
    assert(jash_env_set(s, "items[2]", "retained") == 0);
    const int counts[] = {0, 1, 16, 17, JASH_POS_MAX};
    char short_value[JASH_ENV_VAL_INLINE], long_value[JASH_ENV_VAL_INLINE + 1];
    memset(short_value, 's', sizeof(short_value) - 1);
    short_value[sizeof(short_value) - 1] = 0;
    memset(long_value, 'l', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = 0;
    const char* values[JASH_POS_MAX];
    for (int i = 0; i < JASH_POS_MAX; i++) values[i] = short_value;
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        int count = counts[c];
        assert(jash_env_set(s, "1", "old") == 0);
        assert(jash_env_set(s, "3", "old") == 0);
        assert(jash_env_set(s, "384", "old") == 0);
        /* An absent table key must not newly unset an unrelated libc value. */
        assert(setenv("2", "mirror-only", 1) == 0);
        uint64_t generation = s->env_generation;
        allocation_calls = 0;
        assert(jash_env_replace_positionals(s, values, count) == 0);
        assert(allocation_calls == (count > 16 ? 1 : 0));
        assert(s->env_generation == generation + 4);
        assert(s->env_count == (uint32_t)count + 3);
        for (int i = 1; i <= JASH_POS_MAX; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%d", i);
            if (i <= count) check_value(s, key, short_value);
            else {
                assert(!jash_env_get(s, key));
                if (i == 2) assert(strcmp(getenv(key), "mirror-only") == 0);
                else assert(!getenv(key));
            }
        }
        assert(jash_env_replace_positionals(s, NULL, 0) == 0);
        assert(unsetenv("2") == 0);
    }
    check_value(s, "0", "shell");
    check_value(s, "01", "not-canonical");
    jash_env_array_iter_t it;
    jash_env_array_iter_init(&it, s, "items");
    int idx = jash_env_array_iter_next(&it);
    assert(idx >= 0 && strcmp(s->env[idx].key, "items[2]") == 0);
    assert(jash_env_array_iter_next(&it) == -1);

    /* Every long value is prepared before clearing, on both vector paths. */
    for (int count = 16; count <= 17; count++) {
        for (int i = 0; i < count; i++) values[i] = long_value;
        assert(jash_env_set(s, "1", "old") == 0);
        assert(jash_env_set(s, "384", "sparse") == 0);
        uint64_t generation = s->env_generation;
        int allocations = count + (count > 16);
        for (int f = 0; f < allocations; f++) {
            allocation_calls = 0;
            fail_allocation = f;
            assert(jash_env_replace_positionals(s, values, count) == -1);
            fail_allocation = -1;
            assert(s->env_generation == generation && s->env_count == 5);
            check_value(s, "1", "old");
            check_value(s, "384", "sparse");
            assert(!jash_env_get(s, "2") && !getenv("2"));
        }
        assert(jash_env_replace_positionals(s, values, count) == 0);
        for (int i = 1; i <= count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%d", i);
            check_value(s, key, long_value);
        }
        assert(jash_env_replace_positionals(s, NULL, 0) == 0);
    }
    assert(jash_env_replace_positionals(s, NULL, 17) == 0);
    check_value(s, "17", "");
    assert(jash_env_replace_positionals(s, NULL, JASH_POS_MAX + 1) == -1);
    assert(jash_env_replace_positionals(s, NULL, 0) == 0);

    char* owned[JASH_POS_MAX];
    for (int i = 0; i < JASH_POS_MAX; i++) {
        owned[i] = strdup(i % 2 ? long_value : short_value);
        assert(owned[i]);
    }
    assert(jash_env_set(s, "3", "old") == 0);
    assert(jash_env_set(s, "384", "old") == 0);
    uint64_t generation = s->env_generation;
    assert(jash_env_replace_positionals_owned(s, owned, JASH_POS_MAX) == 0);
    assert(s->env_generation == generation + 3);
    for (int i = 0; i < JASH_POS_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", i + 1);
        assert(!owned[i]);
        check_value(s, key, i % 2 ? long_value : short_value);
    }
    assert(jash_env_replace_positionals_owned(s, owned, 0) == 0);
    while (s->env_count) {
        char key[JASH_ENV_KEY_LEN];
        strcpy(key, s->env[0].key);
        assert(jash_env_unset(s, key) == 0);
    }
    free(s);
}

static void test_identical_long_value(void) {
    jash_state_t* s = new_state();
    char value[JASH_ENV_VAL_INLINE + 80];
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = 0;
    const char* key = "JASH_PERF_ENV_MIRROR";
    assert(jash_env_set(s, key, value) == 0);
    const char* stored = jash_env_get(s, key);
    for (int alias = 0; alias < 2; alias++) {
        assert(setenv(key, "externally changed", 1) == 0);
        uint64_t generation = s->env_generation;
        allocation_calls = 0;
        fail_allocation = 0;
        assert(jash_env_set(s, key, alias ? stored : value) == 0);
        fail_allocation = -1;
        assert(allocation_calls == 0 && jash_env_get(s, key) == stored);
        assert(s->env_generation == generation + 1);
        check_value(s, key, value);
    }
    assert(jash_env_unset(s, key) == 0);
    free(s);
}

static void write_file(const char* path, const char* value) {
    FILE* f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0);
    assert(fclose(f) == 0);
}

static void join_path(char* out, size_t cap, const char* dir, const char* name) {
    size_t dl = strlen(dir), nl = strlen(name);
    assert(dl + nl + 2 <= cap);
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
}

static void check_hashes(jash_fssnap_t* snap) {
    assert(snap);
    for (int i = 0; i < snap->n; i++) {
        const unsigned char* p = (unsigned char*)snap->arena + snap->e[i].path_off;
        unsigned long h = 1469598103934665603UL;
        while (*p) { h ^= *p++; h *= 1099511628211UL; }
        assert(snap->e[i].h == h);
    }
}

static void test_scanner(const char* tmp) {
    char root[1024], dirs[9][1024], files[9][1024];
    snprintf(root, sizeof(root), "%s/tree", tmp);
    assert(mkdir(root, 0700) == 0);
    strcpy(dirs[0], root);
    for (int i = 0; i <= 8; i++) {
        if (i) {
            char part[16];
            snprintf(part, sizeof(part), "part%d", i);
            join_path(dirs[i], sizeof(dirs[i]), dirs[i - 1], part);
            assert(mkdir(dirs[i], 0700) == 0);
        }
        join_path(files[i], sizeof(files[i]), dirs[i], "item");
        write_file(files[i], "old");
    }
    char outside[1024], symlink_path[1024];
    snprintf(outside, sizeof(outside), "%s/outside-link", tmp);
    assert(link(files[8], outside) == 0);
    join_path(symlink_path, sizeof(symlink_path), root, "file-link");
    assert(symlink("item", symlink_path) == 0);
    jash_world_scan_t* world = NULL;
    jash_fssnap_t* snap = jash_fssnap_take_with_world(root, 5, &world);
    check_hashes(snap);
    assert(snap->n == 10 && world && world->count == 7);
    for (int i = 0; i < world->count; i++) {
        char path[2048];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", root, world->entries[i].path);
        assert(stat(path, &st) == 0);
        assert(world->entries[i].size == st.st_size);
        assert(world->entries[i].mtime == st.st_mtime);
        assert(world->entries[i].mode == (int)st.st_mode);
        if (i) assert(strcmp(world->entries[i - 1].path, world->entries[i].path) < 0);
    }
    jash_world_scan_free(world);
    jash_fsdiff_t diff;
    jash_fssnap_diff(snap, root, &diff);
    assert(diff.valid && diff.total == 0 && diff.n == 0);
    snap = jash_fssnap_take(root);
    assert(snap);
    struct stat before, after, file_st;
    assert(stat(dirs[8], &before) == 0 && stat(outside, &file_st) == 0);
    write_file(outside, "new"); /* Same size, unchanged parent directory. */
    struct timespec times[2] = {BEAR_ST_MTIM(file_st), BEAR_ST_MTIM(file_st)};
    times[1].tv_sec++;
    assert(utimensat(AT_FDCWD, outside, times, 0) == 0);
    assert(stat(dirs[8], &after) == 0);
    assert(fs_ts(&BEAR_ST_MTIM(before)) == fs_ts(&BEAR_ST_MTIM(after)));
    assert(unlink(files[0]) == 0);
    char created[1024];
    join_path(created, sizeof(created), root, "added");
    write_file(created, "created");
    ledger_path = files[8];
    jash_fssnap_diff(snap, root, &diff);
    assert(diff.valid && diff.total == 3 && diff.n == 3);
    assert(strcmp(diff.paths[0], "added") == 0 && diff.kind[0] == '+');
    assert(strcmp(diff.paths[1], "item") == 0 && diff.kind[1] == '-');
    assert(strcmp(diff.paths[2], files[8] + strlen(root) + 1) == 0);
    assert(diff.kind[2] == '~' && diff.stale_total == 1);
    assert(diff.stale[2] && diff.stale_read_ts[2] == 42);
    ledger_path = NULL;
    char too_deep[2048];
    snprintf(too_deep, sizeof(too_deep), "%s/deeper", dirs[8]);
    assert(mkdir(too_deep, 0700) == 0);
    assert(!jash_fssnap_take(root));
}

static void test_path_guard(const char* tmp) {
    char root[1024], child[2048], file[2048], padded[1024];
    snprintf(root, sizeof(root), "%s/guard", tmp);
    snprintf(child, sizeof(child), "%s/x", root);
    snprintf(file, sizeof(file), "%s/x/leaf", root);
    assert(mkdir(root, 0700) == 0 && mkdir(child, 0700) == 0);
    write_file(file, "leaf");
    size_t len = strlen(root);
    memcpy(padded, root, len);
    memset(padded + len, '/', sizeof(padded) - len);
    padded[1021] = 0; /* root + '/' + 'x' is exactly 1023 bytes. */
    jash_fssnap_t* snap = jash_fssnap_take(padded);
    check_hashes(snap);
    assert(snap->n == 1);
    jash_fsdiff_t diff;
    jash_fssnap_diff(snap, padded, &diff);
    assert(diff.valid && !diff.total);
    snap = jash_fssnap_take(padded);
    padded[1021] = '/';
    padded[1022] = 0; /* The already-opened child must still reject 1024. */
    assert(!jash_fssnap_take(padded));
    jash_fssnap_diff(snap, padded, &diff);
    assert(!diff.valid);
    assert(unlink(file) == 0 && rmdir(child) == 0);
    padded[1022] = '/';
    padded[1023] = 0;
    assert(!jash_fssnap_take(padded)); /* Empty root still counts the slash. */
}

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(unsetenv("JASH_STALE_GUARD") == 0);
    test_positionals();
    test_identical_long_value();
    test_scanner(argv[1]);
    test_path_guard(argv[1]);
    puts("PASS: positional allocation/mirror boundaries and scanner hash/path/metadata parity");
    return 0;
}
