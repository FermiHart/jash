// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ui/progress — progress bar for long-running operations.

#include "progress.h"
#include <theme/theme.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define PROGRESS_WIDTH 40
#define PROGRESS_THRESHOLD_NS 1000000000ULL  /* 1 second */

struct jash_progress {
    char cmd[256];
    uint64_t start_ns;
    double fraction;
    int active;
    int complete;
};

jash_progress_t* jash_progress_create(void) {
    jash_progress_t* p = (jash_progress_t*)malloc(sizeof(jash_progress_t));
    if (!p) return NULL;
    p->cmd[0] = '\0';
    p->start_ns = 0;
    p->fraction = 0.0;
    p->active = 0;
    p->complete = 0;
    return p;
}

void jash_progress_destroy(jash_progress_t* p) {
    if (p) free(p);
}

void jash_progress_start(jash_progress_t* p, const char* cmd) {
    if (!p) return;
    
    struct timespec ts;
    p->start_ns = clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ?
        (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec : 0;
    
    size_t len = strlen(cmd);
    if (len >= sizeof(p->cmd)) len = sizeof(p->cmd) - 1;
    memcpy(p->cmd, cmd, len);
    p->cmd[len] = '\0';
    
    p->fraction = 0.0;
    p->active = 1;
    p->complete = 0;
}

void jash_progress_update(jash_progress_t* p, double fraction) {
    if (!p || !p->active) return;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    p->fraction = fraction;
}

void jash_progress_complete(jash_progress_t* p) {
    if (!p) return;
    p->fraction = 1.0;
    p->complete = 1;
}

void jash_progress_reset(jash_progress_t* p) {
    if (!p) return;
    p->cmd[0] = '\0';
    p->start_ns = 0;
    p->fraction = 0.0;
    p->active = 0;
    p->complete = 0;
}

void jash_progress_render(jash_progress_t* p, jash_writer_t w, void* ctx) {
    if (!p || !p->active || !w) return;
    
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return;
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    uint64_t elapsed_ns = p->start_ns && now_ns >= p->start_ns ?
        now_ns - p->start_ns : 0;
    
    /* Only show after threshold */
    if (elapsed_ns < PROGRESS_THRESHOLD_NS && !p->complete) return;
    
    const jash_theme_t* theme = jash_theme_current();
    
    /* Calculate elapsed time */
    int sec = (int)(elapsed_ns / 1000000000ULL);
    int min = sec / 60;
    sec = sec % 60;
    
    /* Move to start of line and clear */
    jash_writes(w, ctx, "\r\033[K");
    
    /* Command name (truncated) */
    jash_writes(w, ctx, theme->role_accent);
    size_t cmd_len = strlen(p->cmd);
    if (cmd_len > 20) {
        char truncated[24];
        memcpy(truncated, p->cmd, 17);
        memcpy(truncated + 17, "...", 3);
        truncated[20] = '\0';
        jash_writes(w, ctx, truncated);
    } else {
        jash_writes(w, ctx, p->cmd);
    }
    jash_writes(w, ctx, "\033[0m ");
    
    /* Progress bar */
    jash_writes(w, ctx, "[");
    int filled = (int)(p->fraction * PROGRESS_WIDTH);
    for (int i = 0; i < PROGRESS_WIDTH; i++) {
        if (i < filled) {
            jash_writes(w, ctx, theme->role_ok);
            jash_writes(w, ctx, "█");
        } else if (i == filled && !p->complete) {
            /* Animated spinner */
            int phase = (int)(elapsed_ns / 100000000ULL) % 4;
            const char* spinner[] = {"▏", "▎", "▍", "▌"};
            jash_writes(w, ctx, theme->role_accent);
            jash_writes(w, ctx, spinner[phase]);
        } else {
            jash_writes(w, ctx, "\033[90m");
            jash_writes(w, ctx, "░");
        }
    }
    jash_writes(w, ctx, "\033[0m] ");
    
    /* Percentage */
    int pct = (int)(p->fraction * 100);
    jash_writes(w, ctx, theme->role_muted);
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%3d%%", pct);
    jash_writes(w, ctx, pct_str);
    jash_writes(w, ctx, "\033[0m ");
    
    /* Elapsed time */
    jash_writes(w, ctx, theme->role_muted);
    char time_str[16];
    if (min > 0) {
        snprintf(time_str, sizeof(time_str), "%dm%02ds", min, sec);
    } else {
        snprintf(time_str, sizeof(time_str), "%ds", sec);
    }
    jash_writes(w, ctx, time_str);
    jash_writes(w, ctx, "\033[0m");
    
    /* Clear rest of line */
    jash_writes(w, ctx, "\033[K");
}
