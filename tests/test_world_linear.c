#include <core/world.h>
#include <core/chain_read.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

FILE* jash_chain_open(void) { return NULL; }
int jash_chain_next(FILE* f, jash_chain_entry_t* e) {
    (void)f;
    (void)e;
    return 0;
}

static int fail(const char* message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    const int count = JASH_WORLD_MAX_ENTRIES;
    jash_world_scan_t old_scan = {0}, new_scan = {0};
    old_scan.entries = calloc((size_t)count, sizeof(*old_scan.entries));
    new_scan.entries = calloc((size_t)count, sizeof(*new_scan.entries));
    if (!old_scan.entries || !new_scan.entries) return fail("scan allocation");
    old_scan.count = new_scan.count = count;
    old_scan.capacity = new_scan.capacity = count;
    for (int i = 0; i < count; i++) {
        snprintf(old_scan.entries[i].path, sizeof(old_scan.entries[i].path),
                 "file-%05d", i);
        new_scan.entries[i] = old_scan.entries[i];
    }

    jash_world_delta_t delta;
    jash_world_diff(&old_scan, &new_scan, &delta);
    if (delta.n_created || delta.n_modified || delta.n_deleted)
        return fail("equal scans produced changes");
    if (delta.path_comparisons != (uint32_t)count)
        return fail("world diff is not linear");

    jash_world_entry_t old_small[3] = {
        {.path = "a", .mtime = 1, .size = 1},
        {.path = "b", .mtime = 1, .size = 1},
        {.path = "d", .mtime = 1, .size = 1},
    };
    jash_world_entry_t new_small[3] = {
        {.path = "b", .mtime = 2, .size = 1},
        {.path = "c", .mtime = 1, .size = 1},
        {.path = "d", .mtime = 1, .size = 1},
    };
    jash_world_scan_t old_changes = {.entries = old_small, .count = 3, .capacity = 3};
    jash_world_scan_t new_changes = {.entries = new_small, .count = 3, .capacity = 3};
    jash_world_diff(&old_changes, &new_changes, &delta);
    if (delta.n_created != 1 || strcmp(delta.created, "\"c\"") != 0 ||
        delta.n_modified != 1 || strcmp(delta.modified, "\"b\"") != 0 ||
        delta.n_deleted != 1 || strcmp(delta.deleted, "\"a\"") != 0)
        return fail("linear merge misclassified changes");

    char home[] = "/tmp/jash-world-linear-XXXXXX";
    if (!mkdtemp(home) || setenv("HOME", home, 1) != 0)
        return fail("temporary HOME");
    char jash_dir[512], world_dir[512], state_path[512];
    snprintf(jash_dir, sizeof(jash_dir), "%s/.jash", home);
    snprintf(world_dir, sizeof(world_dir), "%s/.jash/world", home);
    snprintf(state_path, sizeof(state_path), "%s/.jash/world/linear.scan", home);
    if (mkdir(jash_dir, 0700) != 0 || mkdir(world_dir, 0700) != 0)
        return fail("state directories");
    FILE* state = fopen(state_path, "w");
    if (!state) return fail("state file");
    for (int i = count - 1; i >= 0; i--)
        fprintf(state, "file-%05d\t1\t2\t33188\n", i);
    fclose(state);

    jash_world_scan_t* loaded = jash_world_load("linear");
    if (!loaded || loaded->count != count ||
        strcmp(loaded->entries[0].path, "file-00000") != 0 ||
        strcmp(loaded->entries[count - 1].path, "file-04999") != 0)
        return fail("world load truncated persisted entries");
    jash_world_scan_free(loaded);

    unlink(state_path);
    rmdir(world_dir);
    rmdir(jash_dir);
    rmdir(home);
    free(old_scan.entries);
    free(new_scan.entries);
    puts("PASS: world diff and load scale linearly to the configured cap");
    return 0;
}
