// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — Progressive Boot System (implementation)
//
// Key insight: user takes ~500ms to type first command.
// We use that time to load history, job table, etc. in background.

#include "boot.h"
#include <core/env.h>
#include <core/history.h>
#include <job_control.h>
#include <parser/coproc.h>
#include <ui/statusbar.h>
#include <string.h>
#include <unistd.h>

#ifndef JASH_TARGET_BEYOND
#include <pthread.h>

/* Waiters sleep on this condvar instead of busy-spinning on sched_yield.
 * Phase updates broadcast; waiters re-check their own phase predicate. */
static pthread_mutex_t g_boot_phase_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_boot_phase_cv = PTHREAD_COND_INITIALIZER;

static void boot_phase_mark(boot_loader_t* bl, boot_phase_t p) {
    __sync_fetch_and_or(&bl->phase, p);
    pthread_mutex_lock(&g_boot_phase_mu);
    pthread_cond_broadcast(&g_boot_phase_cv);
    pthread_mutex_unlock(&g_boot_phase_mu);
}
#else
#define boot_phase_mark(bl, p) __sync_fetch_and_or(&(bl)->phase, (p))
#endif

/* Core boot work — shared by both threaded and synchronous paths. */
static void boot_run_phases(boot_loader_t* bl) {
    jash_state_t* s = bl->state;

    /* Phase: STATUSBAR — setup scroll region (after prompt is visible).
     * Skip entirely when the statusbar is off (the default) so no terminal
     * line is reserved and login stays pristine. Opt in: JASH_STATUSBAR=1/on. */
    if (!bl->abort && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        const char* sb_env = jash_env_get(s, "JASH_STATUSBAR");
        if (sb_env && (sb_env[0] == '1' || (sb_env[0] == 'o' && sb_env[1] == 'n'))) {
            jash_statusbar_setup_terminal();
        }
        boot_phase_mark(bl, BOOT_HISTORY);
    }

    /* Phase: HISTORY — load history from disk (heaviest I/O) */
    if (!bl->abort) {
        jash_history_load(s, bl->hist_path);
        boot_phase_mark(bl, BOOT_HISTORY);
    }

    /* Phase: JOBS — allocate job control table */
    if (!bl->abort && !s->jobs) {
        s->jobs = jash_job_table_create();
        boot_phase_mark(bl, BOOT_JOBS);
    }

    /* Phase: COPROC — allocate coprocess table (static, just init) */
    if (!bl->abort && !s->coprocs) {
        static coproc_table_t coproc_tbl;
        jash_coproc_init(&coproc_tbl);
        s->coprocs = &coproc_tbl;
        boot_phase_mark(bl, BOOT_COPROC);
    }

    /* Mark complete */
    boot_phase_mark(bl, BOOT_COMPLETE);
}

#ifdef JASH_TARGET_BEYOND
/* Beyond: no pthread — all boot phases run synchronously. */

void jash_boot_init(boot_loader_t* bl, jash_state_t* s) {
    memset(bl, 0, sizeof(*bl));
    bl->state = s;
    bl->phase = BOOT_NONE;
    bl->abort = 0;
}

void jash_boot_start_background(boot_loader_t* bl) {
    __sync_fetch_and_or(&bl->phase, BOOT_CRITICAL | BOOT_ARGS);
    /* Run all boot phases synchronously — no thread. */
    boot_run_phases(bl);
}

void jash_boot_wait(boot_loader_t* bl, boot_phase_t phase) {
    /* Already complete — synchronous boot. */
    (void)bl; (void)phase;
}

void jash_boot_join(boot_loader_t* bl) {
    (void)bl;
}

int jash_boot_ready(boot_loader_t* bl, boot_phase_t phase) {
    return (bl->phase & phase) == phase;
}

void jash_boot_shutdown(boot_loader_t* bl) {
    bl->abort = 1;
    /* No thread to join on Beyond. */
}

#else
/* POSIX targets: background boot via pthread. */

static void* bg_loader_thread(void* arg) {
    boot_loader_t* bl = (boot_loader_t*)arg;
    boot_run_phases(bl);
    return NULL;
}

void jash_boot_init(boot_loader_t* bl, jash_state_t* s) {
    memset(bl, 0, sizeof(*bl));
    bl->state = s;
    bl->phase = BOOT_NONE;
    bl->abort = 0;
    pthread_mutex_init(&bl->mutex, NULL);
}

void jash_boot_start_background(boot_loader_t* bl) {
    __sync_fetch_and_or(&bl->phase, BOOT_CRITICAL | BOOT_ARGS);
    if (pthread_create(&bl->thread, NULL, bg_loader_thread, bl) != 0) {
        /* No thread exists to join. Complete synchronously so every caller
         * still observes BOOT_COMPLETE and can safely fork afterwards. */
        boot_run_phases(bl);
        bl->joined = 1;
    }
}

void jash_boot_wait(boot_loader_t* bl, boot_phase_t phase) {
    pthread_mutex_lock(&g_boot_phase_mu);
    while ((bl->phase & phase) != phase)
        pthread_cond_wait(&g_boot_phase_cv, &g_boot_phase_mu);
    pthread_mutex_unlock(&g_boot_phase_mu);
}

void jash_boot_join(boot_loader_t* bl) {
    if (bl->joined) return;
    jash_boot_wait(bl, BOOT_COMPLETE);
    pthread_join(bl->thread, NULL);
    bl->joined = 1;
}

int jash_boot_ready(boot_loader_t* bl, boot_phase_t phase) {
    return (bl->phase & phase) == phase;
}

void jash_boot_shutdown(boot_loader_t* bl) {
    bl->abort = 1;
    if (!bl->joined) pthread_join(bl->thread, NULL);
    bl->joined = 1;
    pthread_mutex_destroy(&bl->mutex);
}

#endif /* JASH_TARGET_BEYOND */
