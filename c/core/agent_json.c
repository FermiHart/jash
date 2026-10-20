// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — bounded, byte-preserving agent envelope (ADR-0056 W02).
// F E R M I ∞ H A R T
#define _GNU_SOURCE
#include "agent_json.h"
#include "json_writer.h"
#include <core/adopt.h>
#include <core/memo.h>
#include <core/claims.h>
#include <core/jobs_bg.h>
#include <core/timeline.h>
#include <theme/banner.h>
#include <module/modjash.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

#ifndef JASH_GITHASH
#define JASH_GITHASH "unknown"
#endif

int jash_agent_enabled(void) {
    const char* v = getenv("JASH_AGENT");
    return v && v[0] && !(v[0] == '0' && v[1] == '\0');
}

static int g_spool_owner;
static int g_last_ab_n, g_last_ab_failed;
void jash_agent_spool_mark_owner(void) { g_spool_owner = 1; }
int jash_agent_spool_is_owner(void) { return g_spool_owner; }
void jash_agent_last_absorbed(int* count, int* failed) {
    if (count) *count = g_last_ab_n;
    if (failed) *failed = g_last_ab_failed;
}

/* Legacy bounded escaper, retained for callers migrating in W03–W05. This
 * void API cannot signal truncation; the envelope itself no longer uses it. */
void jash_json_esc(const char* s, char* out, size_t cap) {
    if (!out || cap == 0) return;
    size_t o = 0;
    for (size_t i = 0; s && s[i] && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (c < 32) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
                else out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

typedef struct { int kind; const char* a; const char* b; } aj_mutation_t;
typedef struct {
    int count, failed, complete;
    uint64_t tl;
    double ms;
    const char* path;
    struct stat identity;
    int opened;
} aj_absorbed_t;

static int aj_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int aj_absorbed_record(char* line, long* rc, double* ms, long* tl) {
    char* p = line;
    char* end;
    errno = 0;
    *rc = strtol(p, &end, 10);
    if (p == end || errno || *rc < INT_MIN || *rc > INT_MAX || !aj_space(*end)) return -1;
    p = end;
    while (aj_space(*p)) p++;
    errno = 0;
    *ms = strtod(p, &end);
    if (p == end || errno || !isfinite(*ms) || *ms < 0 || !aj_space(*end)) return -1;
    p = end;
    while (aj_space(*p)) p++;
    errno = 0;
    *tl = strtol(p, &end, 10);
    if (p == end || errno || *tl < 0 || *tl > INT_MAX) return -1;
    while (aj_space(*end)) end++;
    return *end ? -1 : 0;
}

static aj_absorbed_t aj_absorbed_collect(void) {
    aj_absorbed_t a = {.complete = 1};
    const char* path = getenv("JASH_ABSORB_SPOOL");
    if (!g_spool_owner || !path || !*path) return a;
    a.path = path;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    /* The path is registered lazily; no child append means no spool file. */
    if (fd < 0) { a.complete = errno == ENOENT; return a; }
    if (fstat(fd, &a.identity) != 0 || !S_ISREG(a.identity.st_mode)) {
        a.complete = 0; close(fd); return a;
    }
    FILE* file = fdopen(fd, "r");
    if (!file) { a.complete = 0; close(fd); return a; }
    a.opened = 1;
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        long rc, tl;
        double ms;
        if (!strchr(line, '\n') || aj_absorbed_record(line, &rc, &ms, &tl) != 0 ||
            a.count == INT_MAX || UINT64_MAX - a.tl < (uint64_t)tl || !isfinite(a.ms + ms)) {
            a.complete = 0;
            break;
        }
        a.count++;
        if (rc != 0) a.failed++;
        a.tl += (uint64_t)tl;
        a.ms += ms;
    }
    if (ferror(file)) a.complete = 0;
    if (fclose(file) != 0) a.complete = 0;
    return a;
}

static void aj_absorbed_release(const aj_absorbed_t* a) {
    /* Preserve unconsumed or concurrently replaced data. Kernel write success
     * is the cleanup boundary here, not an acknowledgement from the client. */
    if (!a->opened || !a->complete) return;
    struct stat st;
    if (lstat(a->path, &st) == 0 && st.st_dev == a->identity.st_dev &&
        st.st_ino == a->identity.st_ino && st.st_size == a->identity.st_size &&
        st.st_mtime == a->identity.st_mtime)
        (void)unlink(a->path);
}

static void aj_move(jash_json_writer_t* w, const aj_mutation_t* m, int legacy) {
    const char* a = m->a ? m->a : "";
    const char* b = m->b ? m->b : "";
    if (legacy && jash_json_utf8_valid(a) && jash_json_utf8_valid(b)) {
        const char* parts[] = {a, " -> ", b};
        jash_jsonw_string_parts(w, parts, 3);
    } else {
        jash_jsonw_raw(w, "{\"from\":"); jash_jsonw_string_or_bytes(w, a);
        jash_jsonw_raw(w, ",\"to\":"); jash_jsonw_string_or_bytes(w, b);
        jash_jsonw_raw(w, "}");
    }
}

static void aj_mutations(jash_json_writer_t* w, const aj_mutation_t* mutations,
                         int count, int kind, int legacy) {
    int first = 1;
    jash_jsonw_raw(w, "[");
    for (int i = 0; i < count; i++) {
        if (mutations[i].kind != kind && !(kind == JASH_UNDO_META && mutations[i].kind == JASH_UNDO_TIMES)) continue;
        if (!first) jash_jsonw_raw(w, ",");
        first = 0;
        if (kind == JASH_UNDO_MOVE) aj_move(w, &mutations[i], legacy);
        else jash_jsonw_string_or_bytes(w, mutations[i].a ? mutations[i].a : "");
    }
    jash_jsonw_raw(w, "]");
}

static void aj_fs_paths(jash_json_writer_t* w, const jash_fsdiff_t* fsd, char kind) {
    int first = 1;
    jash_jsonw_raw(w, "[");
    if (fsd && fsd->valid) {
        for (int i = 0; i < fsd->n && i < JASH_FSDIFF_MAX_REPORT; i++) {
            if (kind ? fsd->kind[i] != kind : !fsd->stale[i]) continue;
            if (!first) jash_jsonw_raw(w, ",");
            first = 0;
            jash_jsonw_string_or_bytes(w, fsd->paths[i]);
        }
    }
    jash_jsonw_raw(w, "]");
}

int jash_agent_emit_to(jash_state_t* s, int rc, double ms, int tl,
                       const jash_fsdiff_t* fsd, const jash_world_delta_t* world,
                       jash_json_sink_t sink, void* context) {
    (void)s;
    aj_mutation_t mutations[JASH_TIMELINE_MAX];
    int count = 0;
    unsigned kinds = 0;
    while (count < JASH_TIMELINE_MAX &&
           jash_timeline_peek(count, &mutations[count].kind, &mutations[count].a,
                              &mutations[count].b)) {
        if (mutations[count].kind > 0 && mutations[count].kind < 32)
            kinds |= 1u << mutations[count].kind;
        count++;
    }
    char cwd[4096];
    int cwd_ok = getcwd(cwd, sizeof(cwd)) != NULL;
    int cwd_error = cwd_ok ? 0 : errno;
    struct rusage self = {0}, child = {0};
    int usage_ok = getrusage(RUSAGE_SELF, &self) == 0 && getrusage(RUSAGE_CHILDREN, &child) == 0;
    double cpu_ms = ((double)self.ru_utime.tv_sec + (double)self.ru_stime.tv_sec +
                     (double)child.ru_utime.tv_sec + (double)child.ru_stime.tv_sec) * 1000.0 +
                    ((double)self.ru_utime.tv_usec + (double)self.ru_stime.tv_usec +
                     (double)child.ru_utime.tv_usec + (double)child.ru_stime.tv_usec) / 1000.0;
    long rss = self.ru_maxrss > child.ru_maxrss ? self.ru_maxrss : child.ru_maxrss;
#ifdef __APPLE__
    rss /= 1024;
#endif
    aj_absorbed_t absorbed = aj_absorbed_collect();
    g_last_ab_n = absorbed.count; g_last_ab_failed = absorbed.failed;
    const char* conflicts[64];
    int nconflicts = jash_claims_conflicts(conflicts, 64);
    int done_ids[32];
    int ndone = jash_jobs_bg_done_peek(done_ids, 32);

    jash_json_writer_t w;
    jash_jsonw_init(&w, sink, context);
    jash_jsonw_raw(&w, "{\"jash_agent\":1,\"schema\":\"jash.agent-envelope.v2\",\"rc\":");
    jash_jsonw_i64(&w, rc);
    jash_jsonw_raw(&w, ",\"ms\":");
    if (isfinite(ms) && ms >= 0) jash_jsonw_rounded(&w, ms); else jash_jsonw_raw(&w, "null");
    jash_jsonw_raw(&w, ",\"tl\":"); jash_jsonw_i64(&w, tl);
    jash_jsonw_raw(&w, ",\"cwd\":");
    if (cwd_ok) jash_jsonw_string_or_bytes(&w, cwd); else jash_jsonw_raw(&w, "null");
    if (!cwd_ok) { jash_jsonw_raw(&w, ",\"cwd_errno\":"); jash_jsonw_i64(&w, cwd_error); }
    jash_jsonw_raw(&w, ",\"cpu_ms\":");
    if (usage_ok) jash_jsonw_rounded(&w, cpu_ms); else jash_jsonw_raw(&w, "null");
    jash_jsonw_raw(&w, ",\"maxrss_kb\":");
    if (usage_ok) jash_jsonw_safe_i64(&w, rss); else jash_jsonw_raw(&w, "null");
    const char* version[] = {JASH_VERSION, " ", JASH_CODENAME};
    jash_jsonw_raw(&w, ",\"jash\":"); jash_jsonw_string_parts(&w, version, 3);
    jash_jsonw_raw(&w, ",\"git\":"); jash_jsonw_string(&w, JASH_GITHASH);
    jash_jsonw_raw(&w, ",\"modabi\":"); jash_jsonw_u64(&w, JMOD_ABI_VERSION);
    static const struct { const char* key; int kind; } fields[] = {
        {"created", JASH_UNDO_FILE_CREATE}, {"modified", JASH_UNDO_FILE_OVERWRITE},
        {"deleted", JASH_UNDO_TRASH}, {"moved", JASH_UNDO_MOVE},
        {"directories_created", JASH_UNDO_DIR_CREATE}, {"directories_deleted", JASH_UNDO_DIR_REMOVE},
        {"links_created", JASH_UNDO_LINK_CREATE}, {"metadata_changed", JASH_UNDO_META}
    };
    if (kinds & (1u << JASH_UNDO_TIMES)) kinds |= 1u << JASH_UNDO_META;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i >= 4 && !(kinds & (1u << fields[i].kind))) continue;
        jash_jsonw_raw(&w, ","); jash_jsonw_string(&w, fields[i].key); jash_jsonw_raw(&w, ":");
        aj_mutations(&w, mutations, count, fields[i].kind, 1);
    }
    if (kinds & (1u << JASH_UNDO_MOVE)) {
        jash_jsonw_raw(&w, ",\"moves\":"); aj_mutations(&w, mutations, count, JASH_UNDO_MOVE, 0);
    }
    jash_jsonw_raw(&w, ",\"timeline_scope\":{\"retained_records\":"); jash_jsonw_i64(&w, count);
    jash_jsonw_raw(&w, ",\"lost_to_ring\":"); jash_jsonw_i64(&w, jash_timeline_lost());
    jash_jsonw_raw(&w, ",\"capacity\":"); jash_jsonw_u64(&w, JASH_TIMELINE_MAX);
    jash_jsonw_raw(&w, "},\"files_changed\":{\"scanned\":");
    int valid = fsd && fsd->valid;
    int reported = valid ? fsd->n : 0;
    if (reported < 0) reported = 0;
    if (reported > JASH_FSDIFF_MAX_REPORT) reported = JASH_FSDIFF_MAX_REPORT;
    jash_jsonw_raw(&w, valid ? "true" : "false");
    jash_jsonw_raw(&w, ",\"total\":"); jash_jsonw_i64(&w, valid ? fsd->total : 0);
    jash_jsonw_raw(&w, ",\"reported\":"); jash_jsonw_i64(&w, reported);
    jash_jsonw_raw(&w, ",\"truncated\":"); jash_jsonw_raw(&w, valid && fsd->total > reported ? "true" : "false");
    jash_jsonw_raw(&w, ",\"scope\":\"bounded-cwd-snapshot\",\"created\":"); aj_fs_paths(&w, fsd, '+');
    jash_jsonw_raw(&w, ",\"modified\":"); aj_fs_paths(&w, fsd, '~');
    jash_jsonw_raw(&w, ",\"deleted\":"); aj_fs_paths(&w, fsd, '-');
    jash_jsonw_raw(&w, ",\"stale_writes\":"); aj_fs_paths(&w, fsd, 0);
    jash_jsonw_raw(&w, "}");
    if (world && (world->n_created || world->n_modified || world->n_deleted)) {
        jash_jsonw_raw(&w, ","); jash_world_delta_write(world, &w);
    }
    int add = 0, del = 0, age = 0;
    if (jash_memo_delta_last(&add, &del, &age)) {
        jash_jsonw_raw(&w, ",\"memo\":{\"delta\":{\"add\":"); jash_jsonw_i64(&w, add);
        jash_jsonw_raw(&w, ",\"del\":"); jash_jsonw_i64(&w, del);
        jash_jsonw_raw(&w, ",\"prev_age_s\":"); jash_jsonw_i64(&w, age);
        jash_jsonw_raw(&w, "}}");
    }
    if (nconflicts > 0) {
        jash_jsonw_raw(&w, ",\"claim_conflicts\":[");
        for (int i = 0; i < nconflicts; i++) {
            if (i) jash_jsonw_raw(&w, ",");
            jash_jsonw_string_or_bytes(&w, conflicts[i]);
        }
        jash_jsonw_raw(&w, "]");
    }
    if (jash_claims_conflicts_truncated()) jash_jsonw_raw(&w, ",\"claim_conflicts_truncated\":true");
    if (ndone > 0) {
        jash_jsonw_raw(&w, ",\"jobs_done\":[");
        for (int i = 0; i < ndone; i++) {
            if (i) jash_jsonw_raw(&w, ",");
            jash_jsonw_i64(&w, done_ids[i]);
        }
        jash_jsonw_raw(&w, "]");
    }
    if (ndone != 0) {
        jash_jsonw_raw(&w, ",\"jobs_notification\":{\"scanned\":"); jash_jsonw_raw(&w, ndone < 0 ? "false" : "true");
        jash_jsonw_raw(&w, ",\"batch_limit\":32,\"delivery\":\"write-confirmed\","
                           "\"client_acknowledged\":false,\"duplicates_possible\":true}");
    }
    jash_jsonw_raw(&w, ",\"absorbed\":{\"count\":");
    jash_jsonw_i64(&w, absorbed.count);
    jash_jsonw_raw(&w, ",\"failed\":"); jash_jsonw_i64(&w, absorbed.failed);
    jash_jsonw_raw(&w, ",\"ms\":"); jash_jsonw_rounded(&w, absorbed.ms);
    jash_jsonw_raw(&w, ",\"tl\":"); jash_jsonw_safe_u64(&w, absorbed.tl);
    jash_jsonw_raw(&w, ",\"complete\":"); jash_jsonw_raw(&w, absorbed.complete ? "true" : "false");
    jash_jsonw_raw(&w, "},\"adoption\":{\"level\":"); jash_jsonw_i64(&w, jash_adopt_level());
    jash_jsonw_raw(&w, ",\"source\":"); jash_jsonw_string(&w, jash_adopt_source_name(jash_adopt_source()));
    jash_jsonw_raw(&w, "},\"path_encoding\":\"utf8-or-base64\","
                       "\"integer_encoding\":\"safe-number-or-decimal-string\"}\n");
    if (jash_jsonw_finish(&w) != 0) return -1;
    aj_absorbed_release(&absorbed);
    if (ndone > 0) (void)jash_jobs_bg_done_ack(done_ids, ndone);
    return 0;
}

int jash_agent_emit(jash_state_t* s, int rc, double ms, int tl,
                    const jash_fsdiff_t* fsd, const jash_world_delta_t* world) {
    int fd = STDERR_FILENO;
    return jash_agent_emit_to(s, rc, ms, tl, fsd, world, jash_jsonw_fd_sink, &fd);
}
