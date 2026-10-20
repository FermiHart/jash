// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/frecency — frecency-based directory navigation

#include "frecency.h"
#include <string.h>
#include <sys/syscalls.h>

static double recency_weight(time_t last_visit) {
    time_t now = time(NULL);
    double days = difftime(now, last_visit) / 86400.0;
    if (days < 1.0) return 1.0;
    if (days < 7.0) return 0.5;
    if (days < 30.0) return 0.25;
    return 0.1;
}

static uint32_t frecency_score(frecency_entry_t* e) {
    return (uint32_t)(e->visits * recency_weight(e->last_visit));
}

/* Dirty tracking in the predict_save_if_dirty spirit, without changing the
 * public struct (frecency.h): the shell owns exactly one frecency_table_t,
 * so a file-static pointer to the table with unsaved changes is enough. */
static frecency_table_t* g_dirty_table = NULL;

void frecency_init(frecency_table_t* ft, const char* home) {
    ft->count = 0;
    ft->persist_path[0] = '\0';
    if (home && home[0]) {
        size_t len = strlen(home);
        if (len + 16 < sizeof(ft->persist_path)) {
            memcpy(ft->persist_path, home, len);
            ft->persist_path[len] = '/';
            ft->persist_path[len + 1] = '.';
            memcpy(ft->persist_path + len + 2, "jash_frecency", 13);
            ft->persist_path[len + 15] = '\0';
        }
    }
}

void frecency_record(frecency_table_t* ft, const char* path) {
    if (!ft || !path || !path[0]) return;
    g_dirty_table = ft;  /* every path below mutates the table */

    /* Find existing entry */
    for (int i = 0; i < ft->count; i++) {
        if (strcmp(ft->entries[i].path, path) == 0) {
            ft->entries[i].visits++;
            ft->entries[i].last_visit = time(NULL);
            ft->entries[i].rank = frecency_score(&ft->entries[i]);
            return;
        }
    }

    /* Add new entry if space available */
    if (ft->count < FRECENCY_MAX) {
        frecency_entry_t* e = &ft->entries[ft->count];
        size_t len = strlen(path);
        if (len >= FRECENCY_PATH_MAX) len = FRECENCY_PATH_MAX - 1;
        memcpy(e->path, path, len);
        e->path[len] = '\0';
        e->visits = 1;
        e->last_visit = time(NULL);
        e->rank = frecency_score(e);
        ft->count++;
    } else {
        /* Replace entry with lowest rank */
        int min_idx = 0;
        uint32_t min_rank = ft->entries[0].rank;
        for (int i = 1; i < ft->count; i++) {
            if (ft->entries[i].rank < min_rank) {
                min_rank = ft->entries[i].rank;
                min_idx = i;
            }
        }
        frecency_entry_t* e = &ft->entries[min_idx];
        size_t len = strlen(path);
        if (len >= FRECENCY_PATH_MAX) len = FRECENCY_PATH_MAX - 1;
        memcpy(e->path, path, len);
        e->path[len] = '\0';
        e->visits = 1;
        e->last_visit = time(NULL);
        e->rank = frecency_score(e);
    }
}

const char* frecency_best(frecency_table_t* ft) {
    if (!ft || ft->count == 0) return NULL;

    int best_idx = 0;
    uint32_t best_rank = 0;
    for (int i = 0; i < ft->count; i++) {
        uint32_t rank = frecency_score(&ft->entries[i]);
        if (rank > best_rank ||
            (rank == best_rank && ft->entries[i].last_visit > ft->entries[best_idx].last_visit)) {
            best_rank = rank;
            best_idx = i;
        }
    }
    return ft->entries[best_idx].path;
}

const char* frecency_match(frecency_table_t* ft, const char* prefix) {
    if (!ft || !prefix || !prefix[0]) return NULL;

    int best_idx = -1;
    uint32_t best_rank = 0;
    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < ft->count; i++) {
        const char* path = ft->entries[i].path;
        size_t path_len = strlen(path);
        int matched = 0;

        /* 1. Full path starts with prefix */
        if (path_len >= prefix_len &&
            strncmp(path, prefix, prefix_len) == 0) {
            matched = 1;
        }

        /* 2. Path ends with prefix */
        if (!matched && path_len >= prefix_len &&
            strcmp(path + path_len - prefix_len, prefix) == 0) {
            matched = 1;
        }

        /* 3. Any path component starts with prefix */
        if (!matched) {
            const char* p = path;
            while (*p) {
                /* Find next component start */
                const char* comp = p;
                while (*p && *p != '/') p++;
                size_t comp_len = (size_t)(p - comp);
                if (comp_len >= prefix_len &&
                    strncmp(comp, prefix, prefix_len) == 0) {
                    matched = 1;
                    break;
                }
                if (*p == '/') p++;
            }
        }

        if (matched) {
            uint32_t rank = frecency_score(&ft->entries[i]);
            if (best_idx == -1 || rank > best_rank) {
                best_rank = rank;
                best_idx = i;
            }
        }
    }

    return best_idx >= 0 ? ft->entries[best_idx].path : NULL;
}

void frecency_list(frecency_table_t* ft, char entries[][FRECENCY_PATH_MAX], int* count) {
    if (!ft || !count) return;

    int n = ft->count < *count ? ft->count : *count;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(ft->entries[i].path);
        if (len >= FRECENCY_PATH_MAX) len = FRECENCY_PATH_MAX - 1;
        memcpy(entries[i], ft->entries[i].path, len);
        entries[i][len] = '\0';
    }
    *count = n;
}

void frecency_save(frecency_table_t* ft) {
    if (!ft || !ft->persist_path[0]) return;
    if (g_dirty_table != ft) return;  /* nothing recorded since last save */

    /* Compose all entries in one buffer: a single write instead of one
     * sys_write per entry. Line format is unchanged. */
    static char out[FRECENCY_MAX * 1024];  /* worst case: 64 x full line */
    size_t total = 0;

    for (int i = 0; i < ft->count; i++) {
        char line[1024];
        int len = 0;

        /* Write path */
        const char* path = ft->entries[i].path;
        while (*path && len < (int)sizeof(line) - 32) {
            line[len++] = *path++;
        }

        /* Write space and visits */
        line[len++] = ' ';
        uint32_t visits = ft->entries[i].visits;
        char visits_str[16];
        int vlen = 0;
        if (visits == 0) {
            visits_str[vlen++] = '0';
        } else {
            while (visits > 0) {
                visits_str[vlen++] = '0' + (visits % 10);
                visits /= 10;
            }
        }
        for (int j = vlen - 1; j >= 0; j--) {
            line[len++] = visits_str[j];
        }

        /* Write space and last_visit */
        line[len++] = ' ';
        time_t last_visit = ft->entries[i].last_visit;
        char time_str[32];
        int tlen = 0;
        if (last_visit == 0) {
            time_str[tlen++] = '0';
        } else {
            while (last_visit > 0) {
                time_str[tlen++] = '0' + (last_visit % 10);
                last_visit /= 10;
            }
        }
        for (int j = tlen - 1; j >= 0; j--) {
            line[len++] = time_str[j];
        }

        line[len++] = '\n';
        if (total + (size_t)len > sizeof(out)) break;
        memcpy(out + total, line, (size_t)len);
        total += (size_t)len;
    }

    long fd = sys_open(ft->persist_path, 0x241, 0644);  /* O_WRONLY | O_CREAT | O_TRUNC */
    if (fd < 0) return;

    size_t off = 0;
    while (off < total) {
        long wn = sys_write(fd, out + off, total - off);
        if (wn <= 0) break;
        off += (size_t)wn;
    }

    sys_close(fd);
    g_dirty_table = NULL;
}

void frecency_load(frecency_table_t* ft) {
    if (!ft || !ft->persist_path[0]) return;

    long fd = sys_open(ft->persist_path, 0, 0);  /* O_RDONLY */
    if (fd < 0) return;

    char buf[4096];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);

    if (n <= 0) return;
    buf[n] = '\0';

    ft->count = 0;
    char* p = buf;

    while (*p && ft->count < FRECENCY_MAX) {
        /* Skip empty lines */
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        /* Read path */
        char* path_start = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        size_t path_len = (size_t)(p - path_start);
        if (path_len >= FRECENCY_PATH_MAX) path_len = FRECENCY_PATH_MAX - 1;

        frecency_entry_t* e = &ft->entries[ft->count];
        memcpy(e->path, path_start, path_len);
        e->path[path_len] = '\0';

        /* Skip space */
        while (*p == ' ') p++;

        /* Read visits */
        e->visits = 0;
        while (*p >= '0' && *p <= '9') {
            e->visits = e->visits * 10 + (*p - '0');
            p++;
        }

        /* Skip space */
        while (*p == ' ') p++;

        /* Read last_visit */
        e->last_visit = 0;
        while (*p >= '0' && *p <= '9') {
            e->last_visit = e->last_visit * 10 + (*p - '0');
            p++;
        }

        /* Skip newline */
        while (*p == '\n' || *p == '\r') p++;

        e->rank = frecency_score(e);
        ft->count++;
    }
}

