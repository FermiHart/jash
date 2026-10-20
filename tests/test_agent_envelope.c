// Stress the production serializer with bounded collectors at their limits.
#include <core/agent_json.h>
#include <core/timeline.h>
#include <core/claims.h>
#include <core/chain_read.h>
#include <float.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char paths[JASH_TIMELINE_MAX][1024];
static int acknowledged;
static size_t failed_bytes;
const char* const JASH_VERSION = "test";
const char* const JASH_CODENAME = "fixture";

int jash_timeline_peek(int index, int* kind, const char** a, const char** b) {
    static const int kinds[] = {JASH_UNDO_FILE_CREATE, JASH_UNDO_FILE_OVERWRITE,
        JASH_UNDO_TRASH, JASH_UNDO_MOVE, JASH_UNDO_DIR_CREATE, JASH_UNDO_DIR_REMOVE,
        JASH_UNDO_LINK_CREATE, JASH_UNDO_META};
    if (index >= JASH_TIMELINE_MAX) return 0;
    *kind = kinds[index % 8]; *a = paths[index]; *b = paths[(index + 1) % JASH_TIMELINE_MAX];
    return 1;
}
int jash_timeline_lost(void) { return 5; }
int jash_memo_delta_last(int* add, int* del, int* age) { *add = 2; *del = 1; *age = 3; return 1; }
int jash_jobs_bg_done_peek(int* ids, int max) { (void)max; ids[0] = 42; return 1; }
int jash_jobs_bg_done_ack(const int* ids, int n) { if (n == 1 && ids[0] == 42) acknowledged++; return 0; }
int jash_adopt_level(void) { return 2; }
int jash_adopt_source(void) { return 1; }
const char* jash_adopt_source_name(int source) { (void)source; return "test\"\\\n"; }
FILE* jash_chain_open(void) { return NULL; }
int jash_chain_next(FILE* file, jash_chain_entry_t* entry) { (void)file; (void)entry; return 0; }

static long fail_sink(void* context, const char* bytes, size_t n) {
    (void)context; (void)bytes;
    if (failed_bytes >= 41) { errno = ENOSPC; return -1; }
    if (n > 7) n = 7;
    failed_bytes += n;
    return (long)n;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "numbers")) {
        int fd = 1;
        jash_json_writer_t w;
        jash_jsonw_init(&w, jash_jsonw_fd_sink, &fd);
        jash_jsonw_raw(&w, "["); jash_jsonw_safe_i64(&w, INT64_MIN);
        jash_jsonw_raw(&w, ","); jash_jsonw_safe_u64(&w, UINT64_MAX);
        jash_jsonw_raw(&w, ","); jash_jsonw_safe_u64(&w, 9007199254740991ULL);
        jash_jsonw_raw(&w, ","); jash_jsonw_rounded(&w, DBL_MAX);
        jash_jsonw_raw(&w, "]\n");
        return jash_jsonw_finish(&w) != 0;
    }
    for (int i = 0; i < JASH_TIMELINE_MAX; i++) {
        snprintf(paths[i], sizeof(paths[i]), "path-%03d-", i);
        size_t start = strlen(paths[i]);
        memset(paths[i] + start, 'x', 800 - start);
        paths[i][400] = '\n'; paths[i][500] = '"'; paths[i][600] = '\\';
    }
    paths[0][12] = (char)0xff;
    for (int i = 0; i < 65; i++) {
        char claim[512];
        snprintf(claim, sizeof(claim), "claim-%03d-", i);
        memset(claim + strlen(claim), 'c', 400 - strlen(claim));
        claim[400] = '\0';
        jash_claims_note_conflict(claim);
    }
    jash_fsdiff_t fsd = {.n = 24, .total = 100, .valid = 1};
    for (int i = 0; i < fsd.n; i++) {
        memcpy(fsd.paths[i], paths[i], sizeof(fsd.paths[i]));
        fsd.kind[i] = "+~-"[i % 3]; fsd.stale[i] = 1;
    }
    jash_world_entry_t* old = calloc(40, sizeof(*old));
    jash_world_entry_t* now = calloc(40, sizeof(*now));
    jash_world_delta_t* world = calloc(1, sizeof(*world));
    CHECK(old && now && world);
    for (int i = 0; i < 40; i++) {
        snprintf(old[i].path, sizeof(old[i].path), "world-%03d-", i);
        snprintf(now[i].path, sizeof(now[i].path), "world-%03d-", i + 20);
        size_t start = strlen(old[i].path);
        memset(old[i].path + start, 'w', 380 - start);
        memset(now[i].path + start, 'w', 380 - start);
        old[i].path[50] = now[i].path[50] = '"';
        old[i].size = 1; now[i].size = 2;
    }
    jash_world_scan_t before = {.entries = old, .count = 40, .capacity = 40};
    jash_world_scan_t after = {.entries = now, .count = 40, .capacity = 40};
    jash_world_diff(&before, &after, world);
    struct { char out[64]; char guard[16]; } small;
    memset(&small, 'Q', sizeof(small));
    CHECK(jash_world_delta_json(world, small.out, sizeof(small.out)) == -1);
    CHECK(small.out[0] == 0);
    for (size_t i = 0; i < sizeof(small.guard); i++) CHECK(small.guard[i] == 'Q');
    CHECK(jash_agent_emit_to(NULL, 0, 1, JASH_TIMELINE_MAX, &fsd, world, fail_sink, NULL) == -1);
    CHECK(errno == ENOSPC && failed_bytes > 0 && acknowledged == 0);
    int fd = 1;
    CHECK(jash_agent_emit_to(NULL, 0, 1, JASH_TIMELINE_MAX, &fsd, world, jash_jsonw_fd_sink, &fd) == 0);
    CHECK(acknowledged == 1);
    free(old); free(now); free(world);
    return 0;
}
