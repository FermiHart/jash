// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ui/progress — progress bar for long-running operations.

#ifndef JASH_UI_PROGRESS_H
#define JASH_UI_PROGRESS_H

#include <theme/io.h>
#include <stdint.h>

typedef struct jash_progress jash_progress_t;

/* Create progress bar context */
jash_progress_t* jash_progress_create(void);

/* Destroy progress bar context */
void jash_progress_destroy(jash_progress_t* p);

/* Start tracking a command */
void jash_progress_start(jash_progress_t* p, const char* cmd);

/* Update progress (0.0 to 1.0) */
void jash_progress_update(jash_progress_t* p, double fraction);

/* Mark complete */
void jash_progress_complete(jash_progress_t* p);

/* Render progress bar */
void jash_progress_render(jash_progress_t* p, jash_writer_t w, void* ctx);

/* Reset for next command */
void jash_progress_reset(jash_progress_t* p);

#endif /* JASH_UI_PROGRESS_H */
