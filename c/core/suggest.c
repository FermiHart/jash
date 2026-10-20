// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/suggest — inline error recovery suggestions.

#include "suggest.h"
#include <theme/colors.h>
#include <theme/io.h>
#include <string.h>
#include <dirent.h>

static int suggest_min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static int suggest_edit_distance(const char* a, const char* b) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la > 63 || lb > 63) return 999;
    int d[64][64];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = suggest_min3(d[i - 1][j] + 1, d[i][j - 1] + 1,
                                   d[i - 1][j - 1] + cost);
        }
    }
    return d[la][lb];
}

void jash_suggest_file(jash_writer_t w, void* ctx, const char* bad_path) {
    if (!bad_path || !*bad_path) return;

    /* Extract directory and base name from bad_path. */
    char dir[512];
    char base[256];
    dir[0] = '\0';
    base[0] = '\0';

    const char* slash = strrchr(bad_path, '/');
    if (slash) {
        int dlen = (int)(slash - bad_path);
        if (dlen > 0 && dlen < 511) {
            for (int i = 0; i < dlen; i++) dir[i] = bad_path[i];
            dir[dlen] = '\0';
        } else {
            dir[0] = '.';
            dir[1] = '\0';
        }
        const char* bstart = slash + 1;
        int blen = (int)strlen(bstart);
        if (blen > 255) blen = 255;
        for (int i = 0; i < blen; i++) base[i] = bstart[i];
        base[blen] = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
        int blen = (int)strlen(bad_path);
        if (blen > 255) blen = 255;
        for (int i = 0; i < blen; i++) base[i] = bad_path[i];
        base[blen] = '\0';
    }

    if (base[0] == '\0') return;

    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    char best[256];
    best[0] = '\0';
    int best_dist = 4;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int dist = suggest_edit_distance(base, ent->d_name);
        if (dist < best_dist && dist <= 3) {
            best_dist = dist;
            int nlen = (int)strlen(ent->d_name);
            if (nlen > 255) nlen = 255;
            for (int i = 0; i < nlen; i++) best[i] = ent->d_name[i];
            best[nlen] = '\0';
        }
    }
    closedir(d);

    if (best[0]) {
        jash_writes(w, ctx, J_ROLE_MUTED "Similar file: ");
        jash_writes_sanitized(w, ctx, dir);
        jash_writes(w, ctx, "/");
        jash_writes_sanitized(w, ctx, best);
        jash_writes(w, ctx, J_RESET "\n");
    }
}
