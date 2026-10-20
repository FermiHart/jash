// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _POSIX_C_SOURCE 200809L
#include "core/gold.h"
#include "core/agent_json.h"
#include "core/output_budget.h"
#include "core/salience.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The persistence unit does not call the unrelated presentation facades. */
int jash_agent_enabled(void) { return 0; }
long jash_budget_limit(void) { return 0; }
int jash_salience_flush(const char* p, long l, const char* c, int a, jash_writer_t w, void* x) {
    (void)p; (void)l; (void)c; (void)a; (void)w; (void)x; return -1;
}
void jash_json_esc(const char* s, char* out, size_t n) { if (n) snprintf(out, n, "%s", s); }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "gold checked %d: %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
static int replace(const char* path, const void* bytes, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    ssize_t wrote = write(fd, bytes, size);
    int closed = close(fd);
    return wrote == (ssize_t)size && !closed ? 0 : -1;
}
static int cancel_at(void* context) { int* count = context; return --*count == 0; }

int main(void) {
    char home[] = "/tmp/jash-gold-checked-XXXXXX";
    CHECK(mkdtemp(home)); CHECK(setenv("HOME", home, 1) == 0);
    jash_gold_dent_t rows[4], readback[4]; memset(rows, 0, sizeof(rows));
    snprintf(rows[0].name, sizeof(rows[0].name), "a\twith\ncontrol"); rows[0].fp = -19; rows[0].size = 9007199254740993LL; rows[0].tag = 'f';
    snprintf(rows[1].name, sizeof(rows[1].name), "z\xff"); rows[1].fp = 7; rows[1].size = 3; rows[1].tag = 'l';
    jash_gold_delta_observation_t before, after;
    jash_gold_delta_publication_t published;
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &before) == 0);
    CHECK(before.complete && !before.present && before.count == 0);
    struct stat st; char parent[4096]; snprintf(parent, sizeof(parent), "%s/.jash", home);
    CHECK(lstat(parent, &st) == -1 && errno == ENOENT);
    int cancel = 1;
    CHECK(jash_gold_delta_store_checked("ls", "test", rows, 2, &before, cancel_at, &cancel, &published) == -1 && errno == ECANCELED);
    CHECK(!published.applied && !published.directories_created);
    CHECK(lstat(parent, &st) == -1 && errno == ENOENT);
    CHECK(jash_gold_delta_store_checked("ls", "test", rows, 2, &before, NULL, NULL, &published) == 0);
    CHECK(published.applied && published.durable && published.directories_created == 3 && published.bytes_written > 24);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &after) == 0);
    CHECK(after.complete && after.present && after.count == 2 && after.bytes == published.bytes_written);
    CHECK(!strcmp(readback[0].name, rows[0].name) && readback[0].fp == -19 && readback[0].size == 9007199254740993LL);
    CHECK(!strcmp(readback[1].name, rows[1].name) && readback[1].tag == 'l');
    long long age; CHECK(jash_gold_delta_load("ls", "test", readback, 4, &age) == 2);
    unsigned char encoded[1024];
    int fd = open(after.path, O_RDONLY); CHECK(fd >= 0);
    ssize_t encoded_size = read(fd, encoded, sizeof(encoded)); CHECK(encoded_size > 24 && encoded_size < (ssize_t)sizeof(encoded));
    CHECK(close(fd) == 0);
    for (size_t cut = 0; cut < (size_t)encoded_size; cut++) {
        CHECK(replace(after.path, encoded, cut) == 0);
        jash_gold_delta_observation_t invalid;
        CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &invalid) == -1);
        CHECK(!invalid.complete && invalid.count == 0);
    }
    unsigned char malformed[1024]; memcpy(malformed, encoded, (size_t)encoded_size);
    memset(malformed + 16, 0xff, 8);
    CHECK(replace(after.path, malformed, (size_t)encoded_size) == 0);
    jash_gold_delta_observation_t invalid;
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &invalid) == -1 && errno == E2BIG);
    memcpy(malformed, encoded, (size_t)encoded_size); malformed[encoded_size] = 0;
    CHECK(replace(after.path, malformed, (size_t)encoded_size + 1u) == 0);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &invalid) == -1 && errno == EINVAL);
    CHECK(replace(after.path, encoded, (size_t)encoded_size) == 0);
    CHECK(jash_gold_delta_load_checked("ls", "test", NULL, 0, &invalid) == -1 && errno == E2BIG);
    char lock_path[4096]; snprintf(lock_path, sizeof(lock_path), "%s/.jash/gold-delta/ls/.delta-lock", home);
    int ready[2], release[2]; CHECK(pipe(ready) == 0 && pipe(release) == 0);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        (void)close(ready[0]); (void)close(release[1]);
        int held = open(lock_path, O_RDWR); char signal = 'r';
        if (held < 0 || flock(held, LOCK_EX) || write(ready[1], &signal, 1) != 1 || read(release[0], &signal, 1) != 1) _exit(2);
        (void)close(held); _exit(0);
    }
    CHECK(close(ready[1]) == 0 && close(release[0]) == 0);
    char signal; CHECK(read(ready[0], &signal, 1) == 1);
    cancel = 2;
    CHECK(jash_gold_delta_store_checked("ls", "test", rows, 1, &after, cancel_at, &cancel, &published) == -1 && errno == ECANCELED);
    CHECK(!published.applied && !strcmp(published.phase, "lock"));
    CHECK(write(release[1], &signal, 1) == 1);
    CHECK(close(ready[0]) == 0 && close(release[1]) == 0);
    int child_status; CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(jash_gold_delta_store_checked("ls", "test", rows, 1, &before, NULL, NULL, &published) == -1 && errno == ESTALE);
    CHECK(!published.applied);
    cancel = 2;
    CHECK(jash_gold_delta_store_checked("ls", "test", rows, 1, &after, cancel_at, &cancel, &published) == -1 && errno == ECANCELED);
    CHECK(!published.applied && !published.durable);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &before) == 0 && before.count == 2);
    CHECK(!memcmp(before.digest, after.digest, sizeof(before.digest)));
    const char legacy[] = "#JGOLD1 12\na\t-1\t7\tf\nz\t9\t11\td\n";
    CHECK(replace(before.path, legacy, sizeof(legacy)-1u) == 0);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &before) == 0);
    CHECK(before.count == 2 && before.timestamp_seconds == 12 && readback[0].fp == -1);
    const char* invalid_text[] = {"#JGOLD1 12\na\t1\t7\tf", "#JGOLD1 12\nz\t1\t7\tf\na\t2\t8\tf\n", "#JGOLD1 12\na\t1\t7\tf\na\t2\t8\tf\n", "#JGOLD1 12\na\t1\t-7\tf\n", "#JGOLD1 12\nbroken\n", "#JGOLD1 12\na\t1\t7\tf\textra\n"};
    for (size_t i = 0; i < sizeof(invalid_text)/sizeof(invalid_text[0]); i++) {
        CHECK(replace(before.path, invalid_text[i], strlen(invalid_text[i])) == 0);
        CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &after) == -1);
        CHECK(!after.complete && after.count == 0 && after.error_number == EINVAL);
    }
    CHECK(unlink(before.path) == 0); CHECK(mkfifo(before.path, 0600) == 0);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &after) == -1 && !after.complete);
    CHECK(unlink(before.path) == 0); CHECK(symlink("missing", before.path) == 0);
    CHECK(jash_gold_delta_load_checked("ls", "test", readback, 4, &after) == -1 && !after.complete);
    CHECK(unlink(before.path) == 0);
    char lock[4096]; snprintf(lock, sizeof(lock), "%s/.jash/gold-delta/ls/.delta-lock", home); CHECK(unlink(lock) == 0);
    snprintf(parent, sizeof(parent), "%s/.jash/gold-delta/ls", home); CHECK(rmdir(parent) == 0);
    snprintf(parent, sizeof(parent), "%s/.jash/gold-delta", home); CHECK(rmdir(parent) == 0);
    snprintf(parent, sizeof(parent), "%s/.jash", home); CHECK(rmdir(parent) == 0); CHECK(rmdir(home) == 0);
    puts("gold checked: bytes, exact integers, legacy migration, CAS, cancellation, malformed rows and special files passed");
    return 0;
}
