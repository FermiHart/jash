// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/linediff — order-preserving multiset line diff (ADR-0042 F1).
// See linediff.h for the contract. F E R M I ∞ H A R T

#include "linediff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LINEDIFF_MAX_LINES 1000
#define LINEDIFF_MAX_BYTES (64L * 1024)
#define LINEDIFF_TABLE     4096   /* power of two, > 2 × MAX_LINES */

static unsigned long ld_fnv(const char* p, size_t n) {
    unsigned long h = 1469598103934665603UL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211UL; }
    return h;
}

/* Read `path` (≤ cap bytes) and split it into lines in place. On success the
 * malloc'd buffer is returned via *buf_out; lines[]/hashes[] are filled and
 * *nlines set. Returns 0 ok, 1 over-cap (too many bytes or lines), -1 I/O. */
static int ld_read_lines(const char* path, char** buf_out,
                         const char** lines, unsigned long* hashes, int* nlines) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (st.st_size > LINEDIFF_MAX_BYTES) return 1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char* buf = (char*)malloc((size_t)st.st_size + 1);
    if (!buf) { (void)fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    (void)fclose(f);
    buf[got] = '\0';

    int    nl    = 0;
    size_t start = 0;
    for (size_t i = 0; i <= got; i++) {
        if (i == got && i == start) break;         /* clean end after final \n */
        if (i == got || buf[i] == '\n') {
            if (nl >= LINEDIFF_MAX_LINES) { free(buf); return 1; }
            buf[i] = '\0';
            lines[nl]  = buf + start;
            hashes[nl] = ld_fnv(buf + start, i - start);
            nl++;
            start = i + 1;
        }
    }
    *buf_out = buf;
    *nlines  = nl;
    return 0;
}

typedef struct { unsigned long key; int count; int used; } ld_slot;

static void ld_build(ld_slot* t, const unsigned long* h, int n) {
    memset(t, 0, sizeof(ld_slot) * LINEDIFF_TABLE);
    for (int j = 0; j < n; j++) {
        int i = (int)(h[j] & (LINEDIFF_TABLE - 1));
        for (int k = 0; k < LINEDIFF_TABLE; k++) {
            if (!t[i].used)     { t[i].used = 1; t[i].key = h[j]; t[i].count = 1; break; }
            if (t[i].key == h[j]) { t[i].count++; break; }
            i = (i + 1) & (LINEDIFF_TABLE - 1);
        }
    }
}

/* Consume one occurrence of `h`; 1 if one was available, 0 if none left. */
static int ld_consume(ld_slot* t, unsigned long h) {
    int i = (int)(h & (LINEDIFF_TABLE - 1));
    for (int k = 0; k < LINEDIFF_TABLE; k++) {
        if (!t[i].used) return 0;
        if (t[i].key == h) {
            if (t[i].count > 0) { t[i].count--; return 1; }
            return 0;
        }
        i = (i + 1) & (LINEDIFF_TABLE - 1);
    }
    return 0;
}

int jash_linediff(const char* prev_path, const char* new_path,
                  jash_linediff_t* out) {
    memset(out, 0, sizeof(*out));

    static const char* plines[LINEDIFF_MAX_LINES];
    static const char* nlines[LINEDIFF_MAX_LINES];
    static unsigned long ph[LINEDIFF_MAX_LINES];
    static unsigned long nh[LINEDIFF_MAX_LINES];
    /* NOTE: statics are fine — the memo path is strictly single-threaded and
     * one command is diffed at a time (see memo.c g_pending single slot). */
    int   pn = 0, nn = 0;
    char* pbuf = NULL;
    char* nbuf = NULL;

    int r = ld_read_lines(prev_path, &pbuf, plines, ph, &pn);
    if (r != 0) return r;
    r = ld_read_lines(new_path, &nbuf, nlines, nh, &nn);
    if (r != 0) { free(pbuf); return r; }

    ld_slot* t = (ld_slot*)malloc(sizeof(ld_slot) * LINEDIFF_TABLE);
    if (!t) { free(pbuf); free(nbuf); return -1; }

    const char** add = (const char**)malloc(sizeof(char*) * (size_t)(nn ? nn : 1));
    const char** del = (const char**)malloc(sizeof(char*) * (size_t)(pn ? pn : 1));
    if (!add || !del) {
        free(t); free((void*)add); free((void*)del); free(pbuf); free(nbuf);
        return -1;
    }

    /* additions: new lines with no unconsumed match in prev */
    ld_build(t, ph, pn);
    int nadd = 0;
    for (int j = 0; j < nn; j++)
        if (!ld_consume(t, nh[j])) add[nadd++] = nlines[j];

    /* removals: prev lines with no unconsumed match in new */
    ld_build(t, nh, nn);
    int ndel = 0;
    for (int j = 0; j < pn; j++)
        if (!ld_consume(t, ph[j])) del[ndel++] = plines[j];

    free(t);
    out->prev_buf = pbuf;
    out->new_buf  = nbuf;
    out->add = add; out->n_add = nadd;
    out->del = del; out->n_del = ndel;
    return 0;
}

void jash_linediff_free(jash_linediff_t* d) {
    if (!d) return;
    free(d->prev_buf);
    free(d->new_buf);
    free((void*)d->add);
    free((void*)d->del);
    memset(d, 0, sizeof(*d));
}
