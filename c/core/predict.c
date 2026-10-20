// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Module: core/predict — smart command prediction (pattern learning)

#include "predict.h"
#include <string.h>
#include <stdio.h>
#include <sys/syscalls.h>

static uint32_t predict_score(predict_entry_t* e) {
    time_t now = time(NULL);
    double days = difftime(now, e->last_seen) / 86400.0;
    double recency;
    if (days < 1.0) recency = 1.0;
    else if (days < 7.0) recency = 0.5;
    else if (days < 30.0) recency = 0.25;
    else recency = 0.1;
    return (uint32_t)(e->count * recency);
}

void predict_make_context(char* ctx, size_t cap, const char* cwd) {
    if (cap == 0) return;
    ctx[0] = '\0';

    const char* base = strrchr(cwd, '/');
    base = base ? base + 1 : cwd;

    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    const char* period;
    if (tm->tm_hour < 12) period = "am";
    else if (tm->tm_hour < 17) period = "pm";
    else if (tm->tm_hour < 21) period = "eve";
    else period = "night";

    snprintf(ctx, cap, "%s:%s", base, period);
}

void predict_init(predict_table_t* pt, const char* home) {
    pt->count = 0;
    pt->dirty = 0;
    pt->persist_path[0] = '\0';
    if (home && home[0]) {
        size_t len = strlen(home);
        if (len + 18 < sizeof(pt->persist_path)) {
            memcpy(pt->persist_path, home, len);
            pt->persist_path[len] = '/';
            pt->persist_path[len + 1] = '.';
            memcpy(pt->persist_path + len + 2, "jash_predictions", 16);
            pt->persist_path[len + 18] = '\0';
        }
    }
}

void predict_record(predict_table_t* pt, const char* cmd, const char* context) {
    if (!pt || !cmd) return;
    pt->dirty = 1;  /* Mark as needing save */

    /* Find existing entry matching cmd+context */
    for (int i = 0; i < pt->count; i++) {
        if (strcmp(pt->entries[i].cmd, cmd) == 0 &&
            strcmp(pt->entries[i].context, context) == 0) {
            pt->entries[i].count++;
            pt->entries[i].last_seen = time(NULL);
            return;
        }
    }

    /* Add new entry if space available */
    if (pt->count < PREDICT_MAX) {
        predict_entry_t* e = &pt->entries[pt->count];
        size_t clen = strlen(cmd);
        if (clen >= PREDICT_CMD_LEN) clen = PREDICT_CMD_LEN - 1;
        memcpy(e->cmd, cmd, clen);
        e->cmd[clen] = '\0';
        size_t xlen = strlen(context);
        if (xlen >= PREDICT_CONTEXT_LEN) xlen = PREDICT_CONTEXT_LEN - 1;
        memcpy(e->context, context, xlen);
        e->context[xlen] = '\0';
        e->count = 1;
        e->last_seen = time(NULL);
        pt->count++;
    } else {
        /* Replace entry with lowest score */
        int min_idx = 0;
        uint32_t min_score = predict_score(&pt->entries[0]);
        for (int i = 1; i < pt->count; i++) {
            uint32_t sc = predict_score(&pt->entries[i]);
            if (sc < min_score) {
                min_score = sc;
                min_idx = i;
            }
        }
        predict_entry_t* e = &pt->entries[min_idx];
        size_t clen = strlen(cmd);
        if (clen >= PREDICT_CMD_LEN) clen = PREDICT_CMD_LEN - 1;
        memcpy(e->cmd, cmd, clen);
        e->cmd[clen] = '\0';
        size_t xlen = strlen(context);
        if (xlen >= PREDICT_CONTEXT_LEN) xlen = PREDICT_CONTEXT_LEN - 1;
        memcpy(e->context, context, xlen);
        e->context[xlen] = '\0';
        e->count = 1;
        e->last_seen = time(NULL);
    }
}

const char* predict_best(predict_table_t* pt, const char* context) {
    if (!pt || !context || !context[0]) return NULL;

    int best_idx = -1;
    uint32_t best_score = 0;

    for (int i = 0; i < pt->count; i++) {
        if (strcmp(pt->entries[i].context, context) == 0) {
            uint32_t sc = predict_score(&pt->entries[i]);
            if (best_idx == -1 || sc > best_score) {
                best_score = sc;
                best_idx = i;
            }
        }
    }

    return best_idx >= 0 ? pt->entries[best_idx].cmd : NULL;
}

void predict_save(predict_table_t* pt) {
    if (!pt || !pt->persist_path[0]) return;

    long fd = sys_open(pt->persist_path, 0x241, 0644);
    if (fd < 0) return;

    for (int i = 0; i < pt->count; i++) {
        char line[512];
        int len = 0;

        /* cmd:context */
        const char* s = pt->entries[i].cmd;
        while (*s && len < (int)sizeof(line) - 64) line[len++] = *s++;
        line[len++] = ':';
        s = pt->entries[i].context;
        while (*s && len < (int)sizeof(line) - 64) line[len++] = *s++;

        /* space + count */
        line[len++] = ' ';
        uint32_t v = pt->entries[i].count;
        char vbuf[16];
        int vlen = 0;
        if (v == 0) { vbuf[vlen++] = '0'; }
        else { while (v > 0) { vbuf[vlen++] = '0' + (v % 10); v /= 10; } }
        for (int j = vlen - 1; j >= 0; j--) line[len++] = vbuf[j];

        /* space + last_seen */
        line[len++] = ' ';
        time_t ts = pt->entries[i].last_seen;
        char tbuf[32];
        int tlen = 0;
        if (ts == 0) { tbuf[tlen++] = '0'; }
        else { while (ts > 0) { tbuf[tlen++] = '0' + (ts % 10); ts /= 10; } }
        for (int j = tlen - 1; j >= 0; j--) line[len++] = tbuf[j];

        line[len++] = '\n';
        sys_write(fd, line, len);
    }

    sys_close(fd);
}

void predict_load(predict_table_t* pt) {
    if (!pt || !pt->persist_path[0]) return;

    long fd = sys_open(pt->persist_path, 0, 0);
    if (fd < 0) return;

    char buf[8192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);

    if (n <= 0) return;
    buf[n] = '\0';

    pt->count = 0;
    char* p = buf;

    while (*p && pt->count < PREDICT_MAX) {
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        predict_entry_t* e = &pt->entries[pt->count];

        /* Read cmd:context */
        char* start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - start);

        /* Parse "cmd:context count last_seen" */
        char* colon = memchr(start, ':', line_len);
        char* sp1 = memchr(start, ' ', line_len);
        if (!colon || !sp1 || colon >= sp1) { while (*p == '\n') p++; continue; }

        size_t cmd_len = (size_t)(colon - start);
        if (cmd_len >= PREDICT_CMD_LEN) cmd_len = PREDICT_CMD_LEN - 1;
        memcpy(e->cmd, start, cmd_len);
        e->cmd[cmd_len] = '\0';

        size_t ctx_len = (size_t)(sp1 - colon - 1);
        if (ctx_len >= PREDICT_CONTEXT_LEN) ctx_len = PREDICT_CONTEXT_LEN - 1;
        memcpy(e->context, colon + 1, ctx_len);
        e->context[ctx_len] = '\0';

        /* Parse count */
        p = sp1 + 1;
        e->count = 0;
        while (*p >= '0' && *p <= '9') {
            e->count = e->count * 10 + (uint32_t)(*p - '0');
            p++;
        }

        /* Skip space */
        while (*p == ' ') p++;

        /* Parse last_seen */
        e->last_seen = 0;
        while (*p >= '0' && *p <= '9') {
            e->last_seen = e->last_seen * 10 + (*p - '0');
            p++;
        }

        while (*p == '\n' || *p == '\r') p++;
        pt->count++;
    }
}

void predict_mark_dirty(predict_table_t* pt) {
    if (pt) pt->dirty = 1;
}

void predict_save_if_dirty(predict_table_t* pt) {
    if (!pt || !pt->dirty) return;
    predict_save(pt);
    pt->dirty = 0;
}
