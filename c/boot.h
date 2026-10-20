// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — Progressive Boot System
//
// Architecture: The shell boots in two phases:
//   Phase 1 (CRITICAL PATH, <0.1ms):
//     - Zero state scalars only (not 350KB arrays)
//     - Seed env defaults
//     - Parse args
//     - Show prompt / execute -c command
//
//   Phase 2 (BACKGROUND, 0-500ms while user types):
//     - Load history from disk
//     - Initialize job control table
//     - Initialize coprocess table
//     - Initialize function table
//     - Pre-scan PATH for tab completion (first 3 dirs)
//     - Load aliases
//
// The key insight: the user takes ~500ms to type their first command.
// We use that time to load heavy modules in a background thread.

#ifndef BOOT_H
#define BOOT_H

#include <core/state.h>

#ifdef JASH_TARGET_BEYOND
/* INV-725: Beyond boots synchronously; do not invent pthread ABI types. */
#define JASH_BOOT_SYNC 1
#else
#include <pthread.h>
#endif

/* Boot phases — what's been initialized. */
typedef enum {
    BOOT_NONE       = 0,
    BOOT_CRITICAL   = 1,   /* state zeroed, env seeded, PID set */
    BOOT_ARGS       = 2,   /* command-line flags parsed */
    BOOT_HISTORY    = 4,   /* history loaded from disk */
    BOOT_JOBS       = 8,   /* job control table allocated */
    BOOT_COPROC     = 16,  /* coprocess table allocated */
    BOOT_FUNCTIONS  = 32,  /* function table allocated */
    BOOT_COMPLETE   = 64,  /* all modules loaded */
} boot_phase_t;

/* Background loader state. */
typedef struct {
    jash_state_t*   state;
    volatile int    phase;         /* atomic: current boot phase */
    volatile int    abort;         /* signal background thread to stop */
#ifndef JASH_TARGET_BEYOND
    int             joined;        /* thread is quiescent; fork is safe */
    pthread_t       thread;        /* background loader thread */
    pthread_mutex_t mutex;         /* protects state mutation */
#endif
    char            hist_path[512];
} boot_loader_t;

/* Initialize the boot loader. Call once at startup. */
void jash_boot_init(boot_loader_t* bl, jash_state_t* s);

/* Start background loading (non-blocking).
 * Returns immediately; loading happens in a background thread. */
void jash_boot_start_background(boot_loader_t* bl);

/* Wait for a specific boot phase to complete (blocking).
 * Returns when the requested phase (or later) is reached. */
void jash_boot_wait(boot_loader_t* bl, boot_phase_t phase);
void jash_boot_join(boot_loader_t* bl);

/* Check if a boot phase is complete (non-blocking). */
int jash_boot_ready(boot_loader_t* bl, boot_phase_t phase);

/* Shutdown: join background thread, cleanup. */
void jash_boot_shutdown(boot_loader_t* bl);

#endif /* BOOT_H */
