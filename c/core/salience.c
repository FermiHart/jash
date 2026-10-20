// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/salience — ranked output under budget (ADR-0042 F4).
// Streaming-ish scorer (one read + two in-memory passes), bounded pattern
// frequency table, novelty vs a per-command previous-output cache, and greedy
// top-K selection under the byte budget. See salience.h. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "salience.h"
#include <core/err_digest.h>   /* jash_err_is_error_line — shared signature set */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscalls.h>

#define SAL_MAX_BYTES  (32L * 1024 * 1024)
#define SAL_MAX_LINES  300000
#define SAL_PAT_TABLE  4096
#define SAL_PREV_TABLE 65536
#define SAL_EDGE_N     5

int jash_governor_is_salient(void) {
    const char* g = getenv("JASH_GOVERNOR");
    return g && strcmp(g, "salient") == 0;
}

// ── hashing ─────────────────────────────────────────────────────────────────

static uint64_t fnv64(const char* s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

// Normalize a line into a pattern: digits and 0x… → '#', quoted strings and
// absolute-path tokens → '@'. The exact placeholders are internal (only the
// grouping matters).
static void normalize(const char* s, char* out, size_t cap) {
    size_t o = 0;
    const char* p = s;
    while (*p && o + 1 < cap) {
        unsigned char c = (unsigned char)*p;
        if (c == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
            out[o++] = '#'; p += 2;
            while (isxdigit((unsigned char)*p)) p++;
            continue;
        }
        if (isdigit(c)) {
            out[o++] = '#';
            while (isdigit((unsigned char)*p)) p++;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = (char)c; out[o++] = '@'; p++;
            while (*p && *p != q) p++;
            if (*p) p++;
            continue;
        }
        if (c == '/' && (p == s || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '=' ||
                         p[-1] == '(' || p[-1] == '\'' || p[-1] == '"')) {
            out[o++] = '@';
            while (*p && *p != ' ' && *p != '\t') p++;
            continue;
        }
        out[o++] = (char)c;
        p++;
    }
    out[o] = '\0';
}

// Broad runtime error markers the compiler-tuned set (err_digest) doesn't cover.
static int broad_error(const char* s) {
    static const char* m[] = {
        "FATAL", "fatal:", "panic:", "panic!", "Traceback", "Segmentation fault",
        "core dumped", "Exception", "FAILED", "[ERROR]", "ERROR:", "abort", NULL
    };
    for (int i = 0; m[i]; i++) if (strstr(s, m[i])) return 1;
    return 0;
}
static int is_error(const char* s) { return jash_err_is_error_line(s) || broad_error(s); }

// ── open-addressing count table (pattern freq) ──────────────────────────────

typedef struct { uint64_t key; int count; } pat_slot_t;

static void pat_add(pat_slot_t* t, uint64_t key) {
    size_t i = (size_t)(key % SAL_PAT_TABLE);
    for (size_t probe = 0; probe < SAL_PAT_TABLE; probe++) {
        size_t j = (i + probe) % SAL_PAT_TABLE;
        if (t[j].count == 0) { t[j].key = key; t[j].count = 1; return; }
        if (t[j].key == key) { t[j].count++; return; }
    }
    /* table full → ignore (freq stays as-seen; rare-scoring still bounded) */
}
static int pat_get(const pat_slot_t* t, uint64_t key) {
    size_t i = (size_t)(key % SAL_PAT_TABLE);
    for (size_t probe = 0; probe < SAL_PAT_TABLE; probe++) {
        size_t j = (i + probe) % SAL_PAT_TABLE;
        if (t[j].count == 0) return 1;
        if (t[j].key == key) return t[j].count;
    }
    return 1;
}

// ── novelty prev set (line hashes from the previous run) ────────────────────

static int prev_path(const char* cmd, char* out, size_t cap) {
    const char* h = getenv("HOME"); if (!h || !*h) return 0;
    char dir[600];
    snprintf(dir, sizeof dir, "%s/.jash", h);          mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.jash/salience", h); mkdir(dir, 0700);
    char safe[128]; size_t o = 0;
    for (const char* p = cmd ? cmd : "cmd"; *p && o + 1 < sizeof safe; p++) {
        char c = *p;
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.') safe[o++]=c;
    }
    safe[o] = 0; if (!o) snprintf(safe, sizeof safe, "cmd");
    snprintf(out, cap, "%s/.jash/salience/%s.prev", h, safe);
    return 1;
}

// Load previous line hashes into a hash set. Returns count loaded.
static int prev_load(const char* cmd, uint64_t* set, int cap) {
    char pp[720]; if (!prev_path(cmd, pp, sizeof pp)) return 0;
    FILE* f = fopen(pp, "r"); if (!f) return 0;
    int n = 0; char line[32];
    while (fgets(line, sizeof line, f)) {
        uint64_t h = strtoull(line, NULL, 16);
        if (!h) continue;
        size_t i = (size_t)(h % (uint64_t)cap);
        for (int probe = 0; probe < cap; probe++) {
            size_t j = (i + (size_t)probe) % (size_t)cap;
            if (set[j] == 0) { set[j] = h; n++; break; }
            if (set[j] == h) break;
        }
    }
    (void)fclose(f);
    return n;
}
static int set_has(const uint64_t* set, int cap, uint64_t h) {
    size_t i = (size_t)(h % (uint64_t)cap);
    for (int probe = 0; probe < cap; probe++) {
        size_t j = (i + (size_t)probe) % (size_t)cap;
        if (set[j] == 0) return 0;
        if (set[j] == h) return 1;
    }
    return 0;
}

// ── whole-buffer ANSI strip (ESC-CSI / OSC) ─────────────────────────────────
static size_t ansi_strip_all(char* buf, size_t len) {
    size_t di = 0;
    for (size_t i = 0; i < len; ) {
        if (buf[i] == '\033' && i + 1 < len) {
            char nx = buf[i + 1];
            if (nx == '[') { i += 2; while (i < len && (buf[i] < 0x40 || buf[i] > 0x7e)) i++; if (i < len) i++; continue; }
            if (nx == ']') { i += 2; while (i < len && buf[i] != '\007') i++; if (i < len) i++; continue; }
            if (nx >= 0x40 && nx <= 0x7e) { i += 2; continue; }
        }
        buf[di++] = buf[i++];
    }
    return di;
}

// ── selection sort helper (indices by score desc, then original order) ──────
static const double* g_scores;
static int cmp_idx(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    double sa = g_scores[ia], sb = g_scores[ib];
    if (sa < sb) return 1;
    if (sa > sb) return -1;
    return ia - ib;   /* stable: earlier line first on ties */
}

// ── the flush ────────────────────────────────────────────────────────────────

int jash_salience_flush(const char* tmppath, long limit, const char* cmdname,
                        int strip_ansi, jash_writer_t w, void* ctx) {
    struct stat st;
    if (stat(tmppath, &st) != 0) return -1;
    if (st.st_size > SAL_MAX_BYTES) return -1;
    size_t sz = (size_t)st.st_size;

    int fd = open(tmppath, O_RDONLY);
    if (fd < 0) return -1;
    char* buf = malloc(sz + 1);
    if (!buf) { close(fd); return -1; }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    if (strip_ansi) got = ansi_strip_all(buf, got);
    buf[got] = '\0';

    /* Under budget → pass through verbatim (no salience tax). */
    if ((long)got <= limit) {
        if (got) { ssize_t nw = write(STDOUT_FILENO, buf, got); (void)nw; }
        free(buf);
        return 1;
    }

    /* Split into NUL-terminated lines (record start pointers + lengths). */
    char** line = malloc(sizeof(char*) * SAL_MAX_LINES);
    int*   llen = malloc(sizeof(int)   * SAL_MAX_LINES);
    if (!line || !llen) { free(line); free(llen); free(buf); return -1; }
    int nlines = 0;
    {
        char* s = buf; char* end = buf + got;
        while (s < end && nlines < SAL_MAX_LINES) {
            char* nl = memchr(s, '\n', (size_t)(end - s));
            size_t len = nl ? (size_t)(nl - s) : (size_t)(end - s);
            line[nlines] = s; llen[nlines] = (int)len;
            if (nl) *nl = '\0';
            nlines++;
            s = nl ? nl + 1 : end;
        }
    }
    if (nlines == 0) { free(line); free(llen); free(buf); return -1; }

    uint64_t* lhash = malloc(sizeof(uint64_t) * (size_t)nlines);
    double*   score = malloc(sizeof(double)   * (size_t)nlines);
    int*      idx   = malloc(sizeof(int)      * (size_t)nlines);
    char*     pick  = calloc((size_t)nlines, 1);
    pat_slot_t* pats = calloc(SAL_PAT_TABLE, sizeof(pat_slot_t));
    uint64_t* prev = calloc(SAL_PREV_TABLE, sizeof(uint64_t));
    if (!lhash || !score || !idx || !pick || !pats || !prev) {
        free(lhash); free(score); free(idx); free(pick); free(pats); free(prev);
        free(line); free(llen); free(buf); return -1;
    }

    /* Pass 1: pattern frequencies + line hashes. */
    char norm[2048];
    uint64_t* phash = malloc(sizeof(uint64_t) * (size_t)nlines);
    if (!phash) { free(lhash); free(score); free(idx); free(pick); free(pats); free(prev); free(line); free(llen); free(buf); return -1; }
    for (int i = 0; i < nlines; i++) {
        lhash[i] = fnv64(line[i], (size_t)llen[i]);
        normalize(line[i], norm, sizeof norm);
        phash[i] = fnv64(norm, strlen(norm));
        pat_add(pats, phash[i]);
    }
    int prev_n = prev_load(cmdname, prev, SAL_PREV_TABLE);

    /* Pass 2: score. */
    int max_freq = 1;
    for (int i = 0; i < nlines; i++) {
        int freq = pat_get(pats, phash[i]);
        if (freq > max_freq) max_freq = freq;
        double s = 0.0;
        if (is_error(line[i])) s += 3.0;
        if (prev_n > 0 && !set_has(prev, SAL_PREV_TABLE, lhash[i])) s += 2.0;
        s += 1.0 / (double)freq;                                   /* rare */
        if (i < SAL_EDGE_N || i >= nlines - SAL_EDGE_N) s += 0.5;  /* edge */
        score[i] = s;
        idx[i] = i;
    }

    /* Select greedily by score under the byte budget (and JASH_SALIENT_K). */
    long kcap = 0;
    const char* ke = getenv("JASH_SALIENT_K");
    if (ke && *ke) kcap = strtol(ke, NULL, 10);
    if (kcap <= 0) { kcap = limit / 80; }
    if (kcap <= 0) { kcap = 1; }
    g_scores = score;
    qsort(idx, (size_t)nlines, sizeof(int), cmp_idx);
    long used = 0; int shown = 0;
    for (int r = 0; r < nlines && shown < kcap; r++) {
        int i = idx[r];
        long cost = (long)llen[i] + 1;
        if (used + cost > limit) continue;   /* try smaller lines further down */
        pick[i] = 1; used += cost; shown++;
    }

    /* Emit selected lines in ORIGINAL order. */
    for (int i = 0; i < nlines; i++) {
        if (!pick[i]) continue;
        ssize_t nw = write(STDOUT_FILENO, line[i], (size_t)llen[i]);
        nw = write(STDOUT_FILENO, "\n", 1);
        (void)nw;
    }

    /* Persist the full output for recall + write the new prev set. */
    char handle[128] = "cmd.txt";
    {
        const char* home = getenv("HOME"); if (!home || !*home) home = jash_tmpdir();
        char dir[300]; snprintf(dir, sizeof dir, "%s/.jash/overflow", home); mkdir(dir, 0700);
        char op[420];
        snprintf(op, sizeof op, "%s/%d-%s.txt", dir, (int)getpid(), cmdname ? cmdname : "cmd");
        int of = open(op, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (of >= 0) {
            for (int i = 0; i < nlines; i++) {
                ssize_t nw = write(of, line[i], (size_t)llen[i]); nw = write(of, "\n", 1); (void)nw;
            }
            close(of);
        }
        for (const char* p = op; *p; p++) if (*p == '/') snprintf(handle, sizeof handle, "%s", p + 1);
    }
    {
        char pp[720];
        if (prev_path(cmdname, pp, sizeof pp)) {
            FILE* pf = fopen(pp, "w");
            if (pf) {
                int cap_w = nlines < SAL_MAX_LINES ? nlines : SAL_MAX_LINES;
                for (int i = 0; i < cap_w; i++) (void)fprintf(pf, "%llx\n", (unsigned long long)lhash[i]);
                (void)fclose(pf);
            }
        }
    }

    /* Count distinct patterns for the footer. */
    int distinct = 0;
    for (int i = 0; i < SAL_PAT_TABLE; i++) if (pats[i].count) distinct++;

    char footer[512];
    int fn = snprintf(footer, sizeof footer,
        "[governor:salient] %d lines \xe2\x86\x92 %d shown \xc2\xb7 %d patterns \xc2\xb7 "
        "top \xc3\x97%d\n  full: recall tail 40 | recall %s\n",
        nlines, shown, distinct, max_freq, handle);
    if (fn > 0) jash_writes(w, ctx, footer);

    free(phash); free(prev); free(pats); free(pick); free(idx);
    free(score); free(lhash); free(llen); free(line); free(buf);
    return 0;
}
