#include <core/fschange.h>
#include <core/world.h>
#include <core/chain_read.h>
#include <core/memo.h>

#include <stdio.h>
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
int jash_memo_ledger_lookup(const char* path, unsigned long* mt,
                            unsigned long* ts) {
    (void)path;
    (void)mt;
    (void)ts;
    return 0;
}

static int fail(const char* message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int write_file(const char* path, const char* value) {
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    fputs(value, f);
    return fclose(f) == 0;
}

int main(void) {
    char root[] = "/tmp/jash-shared-snapshot-XXXXXX";
    if (!mkdtemp(root)) return fail("temporary root");
    char dist[512], svn[512], root_file[512], dist_file[512], svn_file[512];
    snprintf(dist, sizeof(dist), "%s/dist", root);
    snprintf(svn, sizeof(svn), "%s/.svn", root);
    snprintf(root_file, sizeof(root_file), "%s/root.txt", root);
    snprintf(dist_file, sizeof(dist_file), "%s/dist/artifact.txt", root);
    snprintf(svn_file, sizeof(svn_file), "%s/.svn/meta.txt", root);
    if (mkdir(dist, 0700) != 0 || mkdir(svn, 0700) != 0 ||
        !write_file(root_file, "a") || !write_file(dist_file, "a") ||
        !write_file(svn_file, "a"))
        return fail("fixture setup");

    jash_world_scan_t* world = NULL;
    jash_fssnap_t* fs = jash_fssnap_take_with_world(root, 3, &world);
    if (!fs || !world) return fail("combined snapshot");
    int saw_root = 0, saw_dist = 0, saw_svn = 0;
    for (int i = 0; i < world->count; i++) {
        if (strcmp(world->entries[i].path, "root.txt") == 0) saw_root = 1;
        if (strcmp(world->entries[i].path, "dist/artifact.txt") == 0) saw_dist = 1;
        if (strcmp(world->entries[i].path, ".svn/meta.txt") == 0) saw_svn = 1;
    }
    if (!saw_root || !saw_dist || saw_svn)
        return fail("combined walk changed world exclusions");

    sleep(1);
    if (!write_file(root_file, "changed") ||
        !write_file(dist_file, "changed") ||
        !write_file(svn_file, "changed"))
        return fail("fixture mutation");
    jash_fsdiff_t delta;
    jash_fssnap_diff(fs, root, &delta);
    if (!delta.valid || delta.total != 2)
        return fail("combined walk changed fschange exclusions");

    jash_world_scan_free(world);
    unlink(root_file);
    unlink(dist_file);
    unlink(svn_file);
    rmdir(dist);
    rmdir(svn);
    rmdir(root);
    puts("PASS: one walk preserves independent world and fschange views");
    return 0;
}
