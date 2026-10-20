// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/gap — capability-gap radar: report, mine, verify. F E R M I ∞ H A R T
//
// Not "the agent erred" (that's `mistake`) — "JASH-THE-TOOL is missing
// something". Three honest signal classes, never blended:
//
//   reported  — an agent/human hit the wall and said so, in the moment:
//     gap "<what I tried>" [--expected "what should happen"] [--prove "expect …"]
//   observed  — mined from the chain, zero discipline required: rc=2 misuse
//     clusters (unknown option/subcommand) are capability gaps shouting:
//     gap scan [--json]
//   verified  — a gap entry carries an expect proof; `gap verify` re-runs
//     every open proof and marks FIXED whatever now passes. The bugfix queue
//     is a self-verifying test suite: a fix is PROVEN, not declared.
//
// Dedup is by signature with a count: frequency IS the roadmap. Every report
// and every auto-close is attested into the chain (`gap` / `gap-fixed`), so
// `recall` can answer "when did jash gain capability X, and why".
//
// Storage: ~/.jash/gaps.tsv  sig \t status \t count \t first_ts \t last_ts \t
// tried \t expected \t prove   (flock + tmp/rename, like mesh).
// Fleet demand: when the mesh dir exists, every report also bumps
// ~/.jash/mesh/gaps.tsv  sig \t hits \t last_ts \t tried — one counter shared
// by every agent in the swarm, so the backlog ranks by FLEET pain, not mine.

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>
#include <core/adopt.h>        /* jash_repo_key — per-repo chain (ADR-0051 W6) */
#include <module/sigil.h>      /* jash_sigil_attest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

#define GAP_MAX 512

typedef struct {
    char sig[24];
    char status[8];        /* open | fixed */
    int  count;
    long first_ts, last_ts;
    char tried[512];
    char expected[256];
    char prove[256];
} gap_t;

static int gap_file(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    snprintf(b, cap, "%s/.jash/gaps.tsv", h);
    return 1;
}
static int gap_lock(void) {
    const char* h = getenv("HOME");
    if (!h || !*h) return -1;
    char d[600], lp[680];
    snprintf(d, sizeof d, "%s/.jash", h);        mkdir(d, 0700);
    snprintf(lp, sizeof lp, "%s/.jash/gaps.lock", h);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}
static void gap_unlock(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }

/* FNV-1a over "tried \x01 expected" — the dedup signature. */
static void gap_sig(const char* tried, const char* expected, char* out, size_t cap) {
    unsigned long h = 1469598103934665603ULL;
    for (const char* p = tried; p && *p; p++)    { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h ^= 0x01; h *= 1099511628211ULL;
    for (const char* p = expected; p && *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    snprintf(out, cap, "%016lx", h);
}

/* TSV fields can't carry tab/newline — flatten to spaces. */
static void gap_flat(char* s) {
    for (; s && *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

/* split on '\t' PRESERVING empty fields (strtok collapses them — an empty
 * `expected` would shift `prove` into its slot: measured). */
static int gap_split(char* line, char* fld[], int max) {
    int nf = 0;
    char* s = line;
    while (nf < max && s) {
        fld[nf++] = s;
        char* t = strchr(s, '\t');
        s = t ? (*t = 0, t + 1) : NULL;
    }
    return nf;
}

static int gap_load(gap_t* arr, int max) {
    char fp[600];
    if (!gap_file(fp, sizeof fp)) return 0;
    FILE* f = fopen(fp, "r");
    if (!f) return 0;
    int n = 0;
    char line[1600];
    while (n < max && fgets(line, sizeof line, f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        char* fld[8] = {0};
        int nf = gap_split(line, fld, 8);
        if (nf < 5) continue;
        gap_t* g = &arr[n];
        memset(g, 0, sizeof *g);
        snprintf(g->sig, sizeof g->sig, "%s", fld[0]);
        snprintf(g->status, sizeof g->status, "%s", fld[1]);
        g->count = atoi(fld[2]);
        g->first_ts = atol(fld[3]);
        g->last_ts  = atol(fld[4]);
        if (nf > 5 && fld[5]) snprintf(g->tried, sizeof g->tried, "%s", fld[5]);
        if (nf > 6 && fld[6]) snprintf(g->expected, sizeof g->expected, "%s", fld[6]);
        if (nf > 7 && fld[7]) snprintf(g->prove, sizeof g->prove, "%s", fld[7]);
        n++;
    }
    fclose(f);
    return n;
}

static void gap_store(const gap_t* arr, int n) {
    char fp[600];
    if (!gap_file(fp, sizeof fp)) return;
    if (n <= 0) { unlink(fp); return; }
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", fp, (int)getpid());
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\t%s\t%d\t%ld\t%ld\t%s\t%s\t%s\n",
                arr[i].sig, arr[i].status, arr[i].count,
                arr[i].first_ts, arr[i].last_ts,
                arr[i].tried, arr[i].expected, arr[i].prove);
    if (fclose(f) != 0 || rename(tmp, fp) != 0) unlink(tmp);
}

static const char* gap_opt(char* a[], int n, const char* flag) {
    for (int i = 0; i < n - 1; i++) if (!strcmp(a[i], flag)) return a[i + 1];
    return NULL;
}
static int gap_has(char* a[], int n, const char* flag) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i], flag)) return 1;
    return 0;
}

static long gap_null_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx; (void)buf; return (long)n;
}

/* ── fleet demand: one hit-counter per sig, shared by the whole swarm ──────
 * ~/.jash/mesh/gaps.tsv  sig \t hits \t last_ts \t tried(trunc 200).
 * Silent no-op when ~/.jash/mesh doesn't exist (mesh inactive) — the local
 * queue always works alone. Same flock + tmp/rename discipline as claims. */

static int gap_fleet_file(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    char md[600];
    snprintf(md, sizeof md, "%s/.jash/mesh", h);
    struct stat st;
    if (stat(md, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    snprintf(b, cap, "%s/gaps.tsv", md);
    return 1;
}

static void gap_fleet_bump(const char* sig, const char* tried) {
    char fp[680];
    if (!gap_fleet_file(fp, sizeof fp)) return;
    char lp[760];
    snprintf(lp, sizeof lp, "%s.lock", fp);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return; }

    typedef struct { char sig[24]; int hits; long last_ts; char tried[200]; } fg_t;
    static fg_t arr[GAP_MAX];
    int n = 0;
    FILE* f = fopen(fp, "r");
    if (f) {
        char line[600];
        while (n < GAP_MAX && fgets(line, sizeof line, f)) {
            char* nl = strchr(line, '\n'); if (nl) *nl = 0;
            if (!line[0]) continue;
            char* fld[4] = {0};
            int nf = gap_split(line, fld, 4);
            if (nf < 3) continue;
            memset(&arr[n], 0, sizeof arr[n]);
            snprintf(arr[n].sig, sizeof arr[n].sig, "%s", fld[0]);
            arr[n].hits = atoi(fld[1]);
            arr[n].last_ts = atol(fld[2]);
            if (nf > 3 && fld[3]) snprintf(arr[n].tried, sizeof arr[n].tried, "%s", fld[3]);
            n++;
        }
        fclose(f);
    }
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(arr[i].sig, sig)) { idx = i; break; }
    if (idx < 0) {
        if (n >= GAP_MAX) { flock(fd, LOCK_UN); close(fd); return; }
        idx = n++;
        memset(&arr[idx], 0, sizeof arr[idx]);
        snprintf(arr[idx].sig, sizeof arr[idx].sig, "%s", sig);
    }
    arr[idx].hits++;
    arr[idx].last_ts = (long)time(NULL);
    snprintf(arr[idx].tried, sizeof arr[idx].tried, "%.199s", tried);

    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", fp, (int)getpid());
    f = fopen(tmp, "w");
    if (f) {
        for (int i = 0; i < n; i++)
            fprintf(f, "%s\t%d\t%ld\t%s\n",
                    arr[i].sig, arr[i].hits, arr[i].last_ts, arr[i].tried);
        if (fclose(f) != 0 || rename(tmp, fp) != 0) unlink(tmp);
    }
    flock(fd, LOCK_UN);
    close(fd);
}

/* fleet hits for a sig (0 when mesh inactive or sig unknown fleet-wide) */
static int gap_fleet_hits(const char* sig) {
    char fp[680];
    if (!gap_fleet_file(fp, sizeof fp)) return 0;
    FILE* f = fopen(fp, "r");
    if (!f) return 0;
    int hits = 0;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, sig, 16) && line[16] == '\t') { hits = atoi(line + 17); break; }
    }
    fclose(f);
    return hits;
}

static int gap_dispatch(jash_state_t* s, jash_writer_t w, void* ctx,
                        char* tok[], int nt);

/* sort by count desc, then most recent — frequency IS the roadmap */
static void gap_sort(gap_t* arr, int n) {
    for (int i = 1; i < n; i++) {
        gap_t k = arr[i]; int j = i - 1;
        while (j >= 0 && (arr[j].count < k.count ||
               (arr[j].count == k.count && arr[j].last_ts < k.last_ts))) {
            arr[j + 1] = arr[j]; j--;
        }
        arr[j + 1] = k;
    }
}

/* ── report ─────────────────────────────────────────────────────────────── */

static int gap_cmd_report(char* a[], int n, jash_writer_t w, void* ctx) {
    const char* tried = NULL;
    for (int i = 0; i < n; i++) { if (a[i][0] == '-') { i++; continue; } tried = a[i]; break; }
    if (!tried || !*tried) {
        jash_writes(jash_stderr_writer, 0,
            "gap: need what you tried — gap \"<cmd/idea>\" [--expected \"…\"] [--prove \"expect …\"]\r\n");
        return 2;
    }
    const char* expected = gap_opt(a, n, "--expected");
    const char* prove    = gap_opt(a, n, "--prove");
    char sig[24];
    gap_sig(tried, expected ? expected : "", sig, sizeof sig);

    int lk = gap_lock();
    static gap_t arr[GAP_MAX];
    int cnt = gap_load(arr, GAP_MAX);
    long now = (long)time(NULL);
    int idx = -1;
    for (int i = 0; i < cnt; i++)
        if (!strcmp(arr[i].sig, sig)) { idx = i; break; }
    if (idx < 0) {
        if (cnt >= GAP_MAX) { if (lk >= 0) gap_unlock(lk);
            jash_writes(jash_stderr_writer, 0, "gap: queue full (512) — run `gap verify`\r\n"); return 1; }
        idx = cnt++;
        memset(&arr[idx], 0, sizeof arr[idx]);
        snprintf(arr[idx].sig, sizeof arr[idx].sig, "%s", sig);
        snprintf(arr[idx].status, sizeof arr[idx].status, "open");
        arr[idx].first_ts = now;
    }
    arr[idx].count++;
    arr[idx].last_ts = now;
    snprintf(arr[idx].tried, sizeof arr[idx].tried, "%s", tried);
    gap_flat(arr[idx].tried);
    if (expected) { snprintf(arr[idx].expected, sizeof arr[idx].expected, "%s", expected); gap_flat(arr[idx].expected); }
    if (prove)    { snprintf(arr[idx].prove, sizeof arr[idx].prove, "%s", prove);       gap_flat(arr[idx].prove); }
    /* a re-report of a "fixed" gap reopens it — the fix didn't hold */
    if (!strcmp(arr[idx].status, "fixed")) snprintf(arr[idx].status, sizeof arr[idx].status, "open");
    gap_store(arr, cnt);
    if (lk >= 0) gap_unlock(lk);

    /* fleet demand: the swarm shares one counter per sig (no-op if mesh off) */
    gap_fleet_bump(sig, arr[idx].tried);
    int fleet = gap_fleet_hits(sig);

    /* attested: the chain remembers the gap with its context */
    {
        const char* k = jash_repo_key();
        char det[1000];
        snprintf(det, sizeof det, "sig=%s count=%d repo=%s tried=%.400s%s%.200s%s%.200s",
                 sig, arr[idx].count, (k && k[0] != '-') ? k : "-",
                 arr[idx].tried,
                 expected ? " expected=" : "", expected ? arr[idx].expected : "",
                 prove ? " prove=" : "", prove ? arr[idx].prove : "");
        jash_sigil_attest("gap", det);
    }
    jash_writef(w, ctx, "gap: recorded [%s]%s — hit #%d%s%s\r\n", sig,
                arr[idx].count > 1 ? " (again)" : "", arr[idx].count,
                fleet > arr[idx].count ? " · fleet-wide demand" : "",
                prove ? " — proof attached, `gap verify` will close it when true" : "");
    return 0;
}

/* ── list ───────────────────────────────────────────────────────────────── */

static int gap_cmd_list(char* a[], int n, jash_writer_t w, void* ctx) {
    int json = gap_has(a, n, "--json");
    int all  = gap_has(a, n, "--all");
    static gap_t arr[GAP_MAX];
    int cnt = gap_load(arr, GAP_MAX);
    gap_sort(arr, cnt);
    if (json) {
        jash_writes(w, ctx, "[");
        int c = 0;
        for (int i = 0; i < cnt; i++) {
            if (!all && strcmp(arr[i].status, "open")) continue;
            char et[1100], ee[600], ep[600];
            jash_json_esc(arr[i].tried, et, sizeof et);
            jash_json_esc(arr[i].expected, ee, sizeof ee);
            jash_json_esc(arr[i].prove, ep, sizeof ep);
            jash_writef(w, ctx,
                "%s{\"sig\":\"%s\",\"status\":\"%s\",\"count\":%d,\"fleet\":%d,\"last_ts\":%ld,"
                "\"tried\":\"%s\",\"expected\":\"%s\",\"prove\":\"%s\"}",
                c ? "," : "", arr[i].sig, arr[i].status, arr[i].count,
                gap_fleet_hits(arr[i].sig), arr[i].last_ts, et, ee, ep);
            c++;
        }
        jash_writes(w, ctx, "]\n");
        return 0;
    }
    jash_writes(w, ctx, "\033[1;36m═══ GAP QUEUE (demand-weighted) ═══\033[0m\r\n");
    int shown = 0;
    for (int i = 0; i < cnt; i++) {
        if (!all && strcmp(arr[i].status, "open")) continue;
        int fixed = !strcmp(arr[i].status, "fixed");
        int fleet = gap_fleet_hits(arr[i].sig);
        char fl[64] = "";
        if (fleet > arr[i].count)
            snprintf(fl, sizeof fl, "  \033[90m(fleet %dx)\033[0m", fleet);
        jash_writef(w, ctx, "  %s%3dx\033[0m [%s] %s%s%s%s%s%s\r\n",
                    fixed ? "\033[1;32m" : "\033[1;33m",
                    arr[i].count, fixed ? "fixed" : arr[i].sig,
                    arr[i].tried,
                    arr[i].expected[0] ? "  \033[90m→ " : "",
                    arr[i].expected[0] ? arr[i].expected : "",
                    arr[i].prove[0] ? "  \033[90m(proof attached)\033[0m" : "",
                    fl,
                    fixed ? " ✓" : "");
        shown++;
    }
    if (!shown) jash_writes(w, ctx, "  (empty — report with `gap \"<what you tried>\"`; mine with `gap scan`)\r\n");
    return 0;
}

/* ── scan: mine the chain for misuse clusters (observed, not reported) ───── */

static int gap_cmd_scan(char* a[], int n, jash_writer_t w, void* ctx) {
    int json = gap_has(a, n, "--json");
    const char* home = getenv("HOME");
    if (!home || !*home) return 1;
    char cp[600];
    const char* k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(cp, sizeof cp, "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(cp, sizeof cp, "%s/.jash/chain.log", home);
    FILE* f = fopen(cp, "r");
    if (!f) { jash_writes(w, ctx, json ? "[]\n" : "gap: no chain yet — nothing to mine\r\n"); return 0; }

    /* cluster rc=2 (misuse) commands by their first two words */
    typedef struct { char key[160]; int count; } gap_cl_t;
    gap_cl_t cl[64];
    int nc = 0, total2 = 0;
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        const char* r = strstr(line, " rc=");
        if (!r || atoi(r + 4) != 2) continue;
        const char* c = strstr(line, " cmd=");
        if (!c) continue;
        total2++;
        char w0[64] = "", w1[64] = "";
        sscanf(c + 5, "%63s %63s", w0, w1);
        char k2[160];
        snprintf(k2, sizeof k2, "%s%s%s", w0, w1[0] ? " " : "", w1);
        int idx = -1;
        for (int i = 0; i < nc; i++) if (!strcmp(cl[i].key, k2)) { idx = i; break; }
        if (idx < 0) {
            if (nc >= 64) continue;
            idx = nc++;
            memset(&cl[idx], 0, sizeof cl[idx]);
            snprintf(cl[idx].key, sizeof cl[idx].key, "%s", k2);
        }
        cl[idx].count++;
    }
    fclose(f);
    /* sort desc */
    for (int i = 1; i < nc; i++) {
        gap_cl_t t = cl[i]; int j = i - 1;
        while (j >= 0 && cl[j].count < t.count) { cl[j + 1] = cl[j]; j--; }
        cl[j + 1] = t;
    }
    if (json) {
        jash_writes(w, ctx, "[");
        for (int i = 0; i < nc; i++) {
            char ek[256]; jash_json_esc(cl[i].key, ek, sizeof ek);
            jash_writef(w, ctx, "%s{\"misuse\":\"%s\",\"count\":%d}",
                        i ? "," : "", ek, cl[i].count);
        }
        jash_writes(w, ctx, "]\n");
        return 0;
    }
    jash_writef(w, ctx, "\033[1;36m═══ OBSERVED MISUSE (rc=2 clusters, %d total) ═══\033[0m\r\n", total2);
    if (!nc) { jash_writes(w, ctx, "  (none — no rc=2 misuse in this repo's chain)\r\n"); return 0; }
    int shown = 0;
    for (int i = 0; i < nc && shown < 10; i++, shown++)
        jash_writef(w, ctx, "  %3dx  %s\r\n", cl[i].count, cl[i].key);
    jash_writes(w, ctx, "  \033[90mthese are OBSERVED failures, not confirmed gaps — promote with `gap \"<tried>\" --expected \"…\"`\033[0m\r\n");
    return 0;
}

/* ── verify: re-run attached proofs; a fix is PROVEN, not declared ───────── */

static int gap_cmd_verify(char* a[], int n, jash_writer_t w, void* ctx, jash_state_t* s) {
    int json = gap_has(a, n, "--json");
    int lk = gap_lock();
    static gap_t arr[GAP_MAX];
    int cnt = gap_load(arr, GAP_MAX);
    int with_proof = 0, fixed_now = 0, still_open = 0;
    long now = (long)time(NULL);
    for (int i = 0; i < cnt; i++) {
        if (strcmp(arr[i].status, "open")) continue;
        if (!arr[i].prove[0]) { still_open++; continue; }
        with_proof++;
        if (jash_b_expect(s, gap_null_writer, NULL, arr[i].prove) == 0) {
            snprintf(arr[i].status, sizeof arr[i].status, "fixed");
            arr[i].last_ts = now;
            fixed_now++;
            char det[900];
            snprintf(det, sizeof det, "sig=%s tried=%.400s", arr[i].sig, arr[i].tried);
            jash_sigil_attest("gap-fixed", det);
        } else {
            still_open++;
        }
    }
    gap_store(arr, cnt);
    if (lk >= 0) gap_unlock(lk);
    if (json) {
        jash_writef(w, ctx,
            "{\"with_proof\":%d,\"fixed_now\":%d,\"still_open\":%d}\n",
            with_proof, fixed_now, still_open);
    } else {
        jash_writef(w, ctx,
            "gap verify: %d proven fixed, %d still open, %d with proof attached\r\n",
            fixed_now, still_open, with_proof);
        if (with_proof == 0)
            jash_writes(w, ctx, "  \033[90mattach proofs to make fixes self-verifying: gap \"…\" --prove \"expect …\"\033[0m\r\n");
    }
    return still_open > 0 ? 1 : 0;   /* usable as a pipeline gate */
}

/* ── close / dispatch ─────────────────────────────────────────────────────── */

static int gap_cmd_close(char* a[], int n, jash_writer_t w, void* ctx) {
    const char* sig = NULL;
    for (int i = 0; i < n; i++) { if (a[i][0] == '-') { i++; continue; } sig = a[i]; break; }
    if (!sig) { jash_writes(jash_stderr_writer, 0, "gap close: need a sig\r\n"); return 2; }
    int lk = gap_lock();
    static gap_t arr[GAP_MAX];
    int cnt = gap_load(arr, GAP_MAX);
    int done = 0;
    for (int i = 0; i < cnt; i++)
        if (!strcmp(arr[i].sig, sig) && strcmp(arr[i].status, "fixed")) {
            snprintf(arr[i].status, sizeof arr[i].status, "fixed");
            arr[i].last_ts = (long)time(NULL);
            done = 1;
            break;
        }
    if (done) gap_store(arr, cnt);
    if (lk >= 0) gap_unlock(lk);
    if (!done) { jash_writef(jash_stderr_writer, 0, "gap: no open gap [%s]\r\n", sig); return 1; }
    jash_writef(w, ctx, "gap: [%s] closed manually (no proof — an honor close, not a verified one)\r\n", sig);
    return 0;
}

#include "gap_semantic.inc"

int jash_b_gap(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "gap", "jash.gap.v1", args, gap_semantic);
    if (structured >= 0) return structured;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", args ? args : "");
    char* tok[64]; int nt = 0;
    char* p = buf;
    while (*p && nt < 64) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char q = 0; char* start;
        if (*p == '"' || *p == '\'') { q = *p; p++; start = p; while (*p && *p != q) p++; }
        else { start = p; while (*p && *p != ' ' && *p != '\t') p++; }
        if (*p) { *p = 0; p++; }
        tok[nt++] = start;
    }
    return gap_dispatch(s, w, ctx, tok, nt);
}

/* ADR-0036 W2: argv entry — quoted phrases arrive as ONE argv[i], so
 * `gap "jash_writef %+d" --prove "file x"` keeps its boundaries. */
int jash_b_gap_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                    int argc, char** argv) {
    int structured = semantic_dispatch_argv(s, w, ctx, "gap", "jash.gap.v1", argc, argv, gap_semantic);
    if (structured >= 0) return structured;
    return gap_dispatch(s, w, ctx, argv, argc);
}

static int gap_dispatch(jash_state_t* s, jash_writer_t w, void* ctx,
                        char* tok[], int nt) {
    if (nt == 0) {
        jash_writes(w, ctx,
            "gap — capability-gap radar: report, mine, verify. F E R M I ∞ H A R T\r\n"
            "  gap \"<what you tried>\" [--expected \"…\"] [--prove \"expect …\"]   report a tool gap\r\n"
            "  gap list [--json] [--all]     demand-weighted queue (frequency = priority)\r\n"
            "  gap scan [--json]             mine the chain for rc=2 misuse clusters (observed)\r\n"
            "  gap verify [--json]           re-run attached proofs; passing gaps → FIXED\r\n"
            "  gap close <sig>               manual close (honor close — no proof)\r\n"
            "  every report/fix is attested into the chain → `recall` sees capability history\r\n");
        return 0;
    }
    const char* sub = tok[0];
    char** a = tok + 1; int n = nt - 1;
    if (!strcmp(sub, "list"))   return gap_cmd_list(a, n, w, ctx);
    if (!strcmp(sub, "scan"))   return gap_cmd_scan(a, n, w, ctx);
    if (!strcmp(sub, "verify")) return gap_cmd_verify(a, n, w, ctx, s);
    if (!strcmp(sub, "close"))  return gap_cmd_close(a, n, w, ctx);
    if (!strcmp(sub, "report")) return gap_cmd_report(a, n, w, ctx);
    if (sub[0] != '-')          return gap_cmd_report(tok, nt, w, ctx);   /* gap "..." directly */
    jash_writef(jash_stderr_writer, 0, "gap: unknown subcommand '%s'\r\n", sub);
    return 2;
}
