// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/prefetch — the anticipatory shell (ADR-0042 F5).
// Transition learning (flocked TSV) + speculative firing of memoizable
// successors as bounded children. See prefetch.h. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "prefetch.h"
#include <core/memo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define PF_MIN_COUNT   3
#define PF_TOP         2
#define PF_MAX_ROWS    10000
#define PF_CMD_MAX     240
#define PF_DEFAULT_MS  1000

typedef struct { char from[PF_CMD_MAX]; char to[PF_CMD_MAX]; int count; long last_ts; } pf_row_t;

// ── enablement ───────────────────────────────────────────────────────────────
int jash_prefetch_enabled(void) {
    const char* e = getenv("JASH_PREFETCH");
    if (!e || e[0] != '1') return 0;
    if (getenv("JASH_PREFETCH_CHILD")) return 0;   /* recursion guard */
    const char* s = getenv("JASH_SAFE");
    if (s && s[0] == '1') return 0;                /* transactional safe mode */
    return 1;
}

// ── storage ──────────────────────────────────────────────────────────────────
static int pf_path(char* b, size_t cap) {
    const char* h = getenv("HOME"); if (!h || !*h) return 0;
    char d[600]; snprintf(d, sizeof d, "%s/.jash", h); mkdir(d, 0700);
    snprintf(b, cap, "%s/.jash/prefetch.tsv", h);
    return 1;
}
static int pf_lock(void) {
    const char* h = getenv("HOME"); if (!h || !*h) return -1;
    char lp[680]; snprintf(lp, sizeof lp, "%s/.jash/.prefetch.lock", h);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { (void)close(fd); return -1; }
    return fd;
}
static void pf_unlock(int fd) { if (fd >= 0) { (void)flock(fd, LOCK_UN); (void)close(fd); } }

static int pf_load(pf_row_t* rows, int max) {
    char p[600]; if (!pf_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "r"); if (!f) return 0;
    int n = 0; char line[600];
    while (n < max && fgets(line, sizeof line, f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        char* fld[4] = {0}; int nf = 0; char* s = line;
        for (char* t = strtok_r(s, "\t", &s); t && nf < 4; t = strtok_r(NULL, "\t", &s)) fld[nf++] = t;
        if (nf < 4) continue;
        snprintf(rows[n].from, PF_CMD_MAX, "%s", fld[0]);
        snprintf(rows[n].to,   PF_CMD_MAX, "%s", fld[1]);
        rows[n].count   = atoi(fld[2]);
        rows[n].last_ts = atol(fld[3]);
        n++;
    }
    (void)fclose(f);
    return n;
}
static void pf_store(const pf_row_t* rows, int n) {
    char p[600]; if (!pf_path(p, sizeof p)) return;
    FILE* f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < n; i++)
        (void)fprintf(f, "%s\t%s\t%d\t%ld\n", rows[i].from, rows[i].to, rows[i].count, rows[i].last_ts);
    (void)fclose(f);
}

// ── learn ────────────────────────────────────────────────────────────────────
void jash_prefetch_learn(const char* prev_cmd, const char* cur_cmd) {
    if (!prev_cmd || !*prev_cmd || !cur_cmd || !*cur_cmd) return;
    if (strchr(prev_cmd, '\t') || strchr(cur_cmd, '\t')) return;
    if (strlen(prev_cmd) >= PF_CMD_MAX || strlen(cur_cmd) >= PF_CMD_MAX) return;
    int lk = pf_lock(); if (lk < 0) return;
    static pf_row_t rows[PF_MAX_ROWS];
    int n = pf_load(rows, PF_MAX_ROWS);
    long now = (long)time(NULL);
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(rows[i].from, prev_cmd) == 0 && strcmp(rows[i].to, cur_cmd) == 0) { idx = i; break; }
    if (idx < 0) {
        if (n >= PF_MAX_ROWS) {           /* evict the oldest row */
            int oldest = 0;
            for (int i = 1; i < n; i++) if (rows[i].last_ts < rows[oldest].last_ts) oldest = i;
            idx = oldest;
        } else idx = n++;
        snprintf(rows[idx].from, PF_CMD_MAX, "%s", prev_cmd);
        snprintf(rows[idx].to,   PF_CMD_MAX, "%s", cur_cmd);
        rows[idx].count = 0;
    }
    rows[idx].count++;
    rows[idx].last_ts = now;
    pf_store(rows, n);
    pf_unlock(lk);
}

// ── fire ─────────────────────────────────────────────────────────────────────
static int pf_self_path(char* out, size_t cap) {
    const char* b = getenv("JASH_BIN");
    if (b && *b) { snprintf(out, cap, "%s", b); return 1; }
#ifdef __APPLE__
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) return 1;
#else
    ssize_t nr = readlink("/proc/self/exe", out, cap - 1);
    if (nr > 0) { out[nr] = 0; return 1; }
#endif
    return 0;
}

/* is the command's first token a read-only memoizable name? */
static int pf_memoizable(const char* cmd) {
    char name[64]; size_t o = 0;
    const char* p = cmd; while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t' && o + 1 < sizeof name) name[o++] = *p++;
    name[o] = 0;
    return jash_memo_name_memoizable(name);
}

void jash_prefetch_fire(const char* cur_cmd) {
    if (!jash_prefetch_enabled() || !cur_cmd || !*cur_cmd) return;
    long budget_ms = PF_DEFAULT_MS;
    const char* mb = getenv("JASH_PREFETCH_MAX_MS"); if (mb && *mb) { long v = atol(mb); if (v > 0) budget_ms = v; }

    static pf_row_t rows[PF_MAX_ROWS];
    int lk = pf_lock(); if (lk < 0) return;
    int n = pf_load(rows, PF_MAX_ROWS);
    pf_unlock(lk);

    /* collect memoizable successors with count >= threshold */
    int cand[64]; int nc = 0;
    for (int i = 0; i < n && nc < 64; i++)
        if (rows[i].count >= PF_MIN_COUNT && strcmp(rows[i].from, cur_cmd) == 0 && pf_memoizable(rows[i].to))
            cand[nc++] = i;
    if (!nc) return;
    /* selection sort: top PF_TOP by count desc */
    char self[1024]; if (!pf_self_path(self, sizeof self)) return;
    pid_t kids[PF_TOP]; int nk = 0;
    for (int pick = 0; pick < PF_TOP && pick < nc; pick++) {
        int best = -1;
        for (int j = 0; j < nc; j++) {
            if (cand[j] < 0) continue;
            if (best < 0 || rows[cand[j]].count > rows[cand[best]].count) best = j;
        }
        if (best < 0) break;
        const char* to = rows[cand[best]].to;
        cand[best] = -1;   /* consume */
        pid_t kid = fork();
        if (kid == 0) {
            setenv("JASH_PREFETCH_CHILD", "1", 1);
            int nfd = open("/dev/null", O_RDWR);
            if (nfd >= 0) { dup2(nfd, 0); dup2(nfd, 1); dup2(nfd, 2); if (nfd > 2) close(nfd); }
            execl(self, "jash", "-c", to, (char*)NULL);
            _exit(127);
        } else if (kid > 0) {
            kids[nk++] = kid;
        }
    }

    /* wait up to the budget, then kill survivors (bounded, deterministic warm). */
    struct timespec t0;
    int clock_ready = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < nk; i++) {
            if (kids[i] <= 0) continue;
            int st; pid_t r = waitpid(kids[i], &st, WNOHANG);
            if (r == kids[i] || (r < 0)) kids[i] = 0; else alive = 1;
        }
        if (!alive) break;
        struct timespec now;
        int clock_ok = clock_ready && clock_gettime(CLOCK_MONOTONIC, &now) == 0;
        long el = clock_ok ?
            (now.tv_sec - t0.tv_sec) * 1000 +
            (now.tv_nsec - t0.tv_nsec) / 1000000 : budget_ms;
        if (el >= budget_ms) {
            for (int i = 0; i < nk; i++) if (kids[i] > 0) { kill(kids[i], SIGKILL); waitpid(kids[i], NULL, 0); kids[i] = 0; }
            break;
        }
        struct timespec nap = {0, 5 * 1000 * 1000};   /* 5 ms */
        nanosleep(&nap, NULL);
    }
}
