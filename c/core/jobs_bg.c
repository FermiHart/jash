// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/jobs_bg — cross-`-c` detached async jobs (ADR-0042 F6).
// Store + done-scan + record reader. main.c does the fork/setsid; the mesh of
// verbs (jobs/wait) and the envelope are thin callers. See jobs_bg.h.
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "jobs_bg.h"
#include <core/agent_json.h>   /* jash_json_esc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

// ── paths + flock critical section ──────────────────────────────────────────

static int jb_root(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    int n = snprintf(b, cap, "%s/.jash/jobs", h);
    return n >= 0 && (size_t)n < cap;
}
static void jb_ensure(void) {
    const char* h = getenv("HOME");
    if (!h || !*h) return;
    char p[600];
    snprintf(p, sizeof p, "%s/.jash", h);      mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/.jash/jobs", h); mkdir(p, 0700);
}
static int jb_lock(void) {
    char r[600], lp[680];
    if (!jb_root(r, sizeof r)) return -1;
    snprintf(lp, sizeof lp, "%s/.lock", r);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { (void)close(fd); return -1; }
    return fd;
}
static void jb_unlock(int fd) { if (fd >= 0) { (void)flock(fd, LOCK_UN); (void)close(fd); } }

static int jb_json_path(int id, char* b, size_t cap) {
    char r[600]; if (!jb_root(r, sizeof r)) return 0;
    snprintf(b, cap, "%s/%d.json", r, id);
    return 1;
}

int jash_jobs_bg_out_path(int id, char* out, size_t cap) {
    char r[600]; if (!jb_root(r, sizeof r)) return 0;
    snprintf(out, cap, "%s/%d.out", r, id);
    return 1;
}

/* Atomic record write: a concurrent reader (jash_jobs_bg_load, which cannot take
 * the lock — it runs inside locked scans) must never observe a truncated file.
 * Write to <path>.tmp then rename() over the target. */
static int jb_atomic_write(const char* jp, const char* line) {
    char tmp[720];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", jp);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    FILE* f = fopen(tmp, "w");
    if (!f) return -1;
    int failed = fputs(line, f) == EOF;
    if (fclose(f) != 0) failed = 1;
    if (!failed && rename(tmp, jp) == 0) return 0;
    (void)unlink(tmp);
    return -1;
}

// ── tiny fixed-shape JSON reader (jash-authored records only) ────────────────

static long jb_num(const char* s, const char* key) {
    const char* p = strstr(s, key);
    if (!p) return 0;
    return atol(p + strlen(key));
}
// Extract the escaped cmd (last field): between "cmd":" and the trailing "}.
static void jb_cmd(const char* s, char* out, size_t cap) {
    out[0] = 0;
    const char* p = strstr(s, "\"cmd\":\"");
    if (!p) return;
    p += 7;
    const char* end = strrchr(p, '"');   /* last quote before }\n */
    if (!end || end < p) return;
    size_t n = (size_t)(end - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

int jash_jobs_bg_load(int id, jash_bgjob_t* out) {
    if (!out) return 0;
    char jp[680]; if (!jb_json_path(id, jp, sizeof jp)) return 0;
    int fd = open(jp, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat observed;
    if (fstat(fd, &observed) || !S_ISREG(observed.st_mode) ||
        observed.st_size < 0 || (uint64_t)observed.st_size > JASH_BGJOB_CMD_MAX + 256u) {
        close(fd); return 0;
    }
    FILE* f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    char line[JASH_BGJOB_CMD_MAX + 256];
    int ok = 0;
    if (fgets(line, sizeof line, f)) {
        memset(out, 0, sizeof *out);
        out->id       = (int)id;
        out->running  = strstr(line, "\"state\":\"done\"") ? 0 : 1;
        out->pid      = jb_num(line, "\"pid\":");
        out->rc       = (int)jb_num(line, "\"rc\":");
        out->ms       = jb_num(line, "\"ms\":");
        out->start_ts = jb_num(line, "\"start_ts\":");
        out->end_ts   = jb_num(line, "\"end_ts\":");
        out->notified = strstr(line, "\"notified\":true") ? 1 : 0;
        jb_cmd(line, out->cmd, sizeof out->cmd);
        ok = 1;
    }
    (void)fclose(f);
    return ok;
}

// ── writers (caller may hold the lock; per-id files are otherwise unique) ────

static int jb_write_running(int id, long pid, long start_ts, const char* esc_cmd) {
    char jp[680]; if (!jb_json_path(id, jp, sizeof jp)) return -1;
    char line[JASH_BGJOB_CMD_MAX + 256];
    snprintf(line, sizeof line,
        "{\"id\":%d,\"state\":\"running\",\"pid\":%ld,\"start_ts\":%ld,\"cmd\":\"%s\"}\n",
        id, pid, start_ts, esc_cmd);
    jb_atomic_write(jp, line);
    return 0;
}

int jash_jobs_bg_alloc(void) {
    jb_ensure();
    int lk = jb_lock();
    if (lk < 0) return 0;
    char r[600], np[680];
    jb_root(r, sizeof r);
    snprintf(np, sizeof np, "%s/nextid", r);
    int id = 1;
    FILE* f = fopen(np, "r");
    if (f) { if (fscanf(f, "%d", &id) != 1) id = 1; (void)fclose(f); }
    if (id < 1) id = 1;
    FILE* w = fopen(np, "w");
    if (w) { (void)fprintf(w, "%d\n", id + 1); (void)fclose(w); }
    jb_unlock(lk);
    return id;
}

int jash_jobs_bg_start(int id, long pid, const char* cmd) {
    jb_ensure();
    char esc[JASH_BGJOB_CMD_MAX];
    jash_json_esc(cmd ? cmd : "", esc, sizeof esc);
    int lk = jb_lock();
    long now = (long)time(NULL);
    int r = jb_write_running(id, pid, now, esc);
    jb_unlock(lk);
    return r;
}

int jash_jobs_bg_finalize(int id, int rc, long ms) {
    int lk = jb_lock();
    jash_bgjob_t j;
    long pid = 0, start_ts = (long)time(NULL);
    char esc[JASH_BGJOB_CMD_MAX] = "";
    if (jash_jobs_bg_load(id, &j)) {
        pid = j.pid; start_ts = j.start_ts;
        snprintf(esc, sizeof esc, "%s", j.cmd);   /* already escaped on disk */
    }
    char jp[680];
    if (jb_json_path(id, jp, sizeof jp)) {
        char line[JASH_BGJOB_CMD_MAX + 256];
        snprintf(line, sizeof line,
            "{\"id\":%d,\"state\":\"done\",\"rc\":%d,\"ms\":%ld,\"pid\":%ld,"
            "\"start_ts\":%ld,\"end_ts\":%ld,\"notified\":false,\"cmd\":\"%s\"}\n",
            id, rc, ms, pid, start_ts, (long)time(NULL), esc);
        jb_atomic_write(jp, line);
    }
    jb_unlock(lk);
    return 0;
}

// ── done-scan (one-shot notify) + listing ────────────────────────────────────

static int jb_id_from_name(const char* name) {
    /* "<n>.json" → n, else -1 */
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name || strcmp(dot, ".json") != 0 || *name < '1' || *name > '9') return -1;
    int value = 0;
    for (const char* p = name; p < dot; p++) {
        if (*p < '0' || *p > '9' || value > (INT_MAX - (*p - '0')) / 10) return -1;
        value = value * 10 + (*p - '0');
    }
    return value;
}

static int jb_done_collect(int* ids, int max, int consume) {
    char r[600]; if (!jb_root(r, sizeof r)) return 0;
    if (max <= 0 || !ids) return 0;
    int lk = jb_lock();
    if (lk < 0) return errno == ENOENT ? 0 : -1;
    DIR* d = opendir(r);
    if (!d) { int error = errno; jb_unlock(lk); return error == ENOENT ? 0 : -1; }
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max) {
        int id = jb_id_from_name(e->d_name);
        if (id < 0) continue;
        jash_bgjob_t j;
        if (!jash_jobs_bg_load(id, &j)) continue;
        if (j.running || j.notified) continue;
        ids[n++] = id;
        if (!consume) continue;
        /* Compatibility scan: immediate consumption. The envelope uses peek. */
        char jp[680];
        if (jb_json_path(id, jp, sizeof jp)) {
            char line[JASH_BGJOB_CMD_MAX + 256];
            snprintf(line, sizeof line,
                "{\"id\":%d,\"state\":\"done\",\"rc\":%d,\"ms\":%ld,\"pid\":%ld,"
                "\"start_ts\":%ld,\"end_ts\":%ld,\"notified\":true,\"cmd\":\"%s\"}\n",
                j.id, j.rc, j.ms, j.pid, j.start_ts, j.end_ts, j.cmd);
            jb_atomic_write(jp, line);
        }
    }
    (void)closedir(d);
    jb_unlock(lk);
    return n;
}

int jash_jobs_bg_done_scan(int* ids, int max) {
    int n = jb_done_collect(ids, max, 1);
    return n < 0 ? 0 : n;
}

int jash_jobs_bg_done_peek(int* ids, int max) { return jb_done_collect(ids, max, 0); }

int jash_jobs_bg_done_ack(const int* ids, int count) {
    if (!ids || count < 0) return -1;
    if (!count) return 0;
    int lk = jb_lock();
    if (lk < 0) return -1;
    int failures = 0;
    for (int i = 0; i < count; i++) {
        jash_bgjob_t j;
        if (!jash_jobs_bg_load(ids[i], &j)) { failures++; continue; }
        if (j.running || j.notified) continue;
        char path[680], line[JASH_BGJOB_CMD_MAX + 256];
        if (!jb_json_path(ids[i], path, sizeof(path))) { failures++; continue; }
        int n = snprintf(line, sizeof(line),
            "{\"id\":%d,\"state\":\"done\",\"rc\":%d,\"ms\":%ld,\"pid\":%ld,"
            "\"start_ts\":%ld,\"end_ts\":%ld,\"notified\":true,\"cmd\":\"%s\"}\n",
            j.id, j.rc, j.ms, j.pid, j.start_ts, j.end_ts, j.cmd);
        if (n < 0 || (size_t)n >= sizeof(line) || jb_atomic_write(path, line) != 0) failures++;
    }
    jb_unlock(lk);
    return failures ? -1 : 0;
}

int jash_jobs_bg_list_json(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char r[600]; if (!jb_root(r, sizeof r)) { snprintf(out, cap, "[]"); return 2; }
    DIR* d = opendir(r);
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "[");
    if (d) {
        long now = (long)time(NULL);
        int first = 1;
        struct dirent* e;
        while ((e = readdir(d))) {
            int id = jb_id_from_name(e->d_name);
            if (id < 0) continue;
            jash_bgjob_t j;
            if (!jash_jobs_bg_load(id, &j)) continue;
            if (o + JASH_BGJOB_CMD_MAX + 256 >= cap) break;
            if (j.running)
                o += (size_t)snprintf(out + o, cap - o,
                    "%s{\"id\":%d,\"state\":\"running\",\"pid\":%ld,\"cmd\":\"%s\",\"elapsed_s\":%ld}",
                    first ? "" : ",", j.id, j.pid, j.cmd, now - j.start_ts);
            else
                o += (size_t)snprintf(out + o, cap - o,
                    "%s{\"id\":%d,\"state\":\"done\",\"rc\":%d,\"ms\":%ld,\"pid\":%ld,\"cmd\":\"%s\"}",
                    first ? "" : ",", j.id, j.rc, j.ms, j.pid, j.cmd);
            first = 0;
        }
        (void)closedir(d);
    }
    o += (size_t)snprintf(out + o, cap - o, "]");
    return (int)o;
}
