// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_ai.h — AI Agent stub header (flags + routing)
// Full implementation: modules/ai/ai.modjash (load with: ? on)

#ifndef JASH_AI_H
#define JASH_AI_H

#include <core/state.h>
#include <theme/io.h>
#include <stddef.h>

void jash_ai_init(void);

/* State setters (called by module via exported symbol) */
void jash_ai_set_mode(int active);
void jash_ai_set_auto(int active);
void jash_ai_set_learn(int active);
void jash_ai_set_debug(int active);
void jash_ai_set_busy(int active);

/* State getters (called by statusbar, theme, clear) */
int jash_ai_active(void);
int jash_ai_auto(void);
int jash_ai_learn(void);
int jash_ai_busy(void);
int jash_ai_debug(void);

/* Automatic AI response boundary: one timeline marker before any generated
 * command, then a bounded command-outcome event. Depth is not a CoW snapshot;
 * only entries journaled by jash may be rewound. No approval is requested. */
int jash_ai_auto_begin(char* checkpoint, size_t capacity, int* depth,
                       int* trash_protected);
int jash_ai_auto_record(const char* checkpoint, const char* command,
                        int rc, int depth_before, int* depth_after,
                        char digest_hex[65]);

/* Unified "?" router — eval.c calls this for every line starting with '?'.
 * Returns 1 if handled (was a '?' line), else 0. */
int  jash_ai_route(jash_state_t *s, jash_writer_t w, void *ctx, const char *line);
/* Thin shim for callers that have already stripped the leading '?' */
void jash_ai_submit_query(jash_state_t *s, jash_writer_t w, void *ctx, const char *query);

void jash_ai_render_spinner(jash_writer_t w, void *ctx);
void jash_ai_help(jash_writer_t w, void *ctx);

#endif
