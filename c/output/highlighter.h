// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — output/highlighter — auto-detect and highlight command output.
//
// Detects output format (JSON, logs, SQL, XML, etc.) and applies
// syntax highlighting in real-time.

#ifndef JASH_OUTPUT_HIGHLIGHTER_H
#define JASH_OUTPUT_HIGHLIGHTER_H

#include <theme/io.h>
#include <stddef.h>

typedef struct jash_highlighter jash_highlighter_t;

/* Create highlighter context */
jash_highlighter_t* jash_highlighter_create(void);

/* Destroy highlighter context */
void jash_highlighter_free(jash_highlighter_t* h);

/* Check if global highlighting is enabled */
int jash_highlighting_is_global_enabled(void);

/* Enable/disable global highlighting */
void jash_highlighting_set_global_enabled(int enabled);

/* Process a chunk of output and write with highlighting */
long jash_highlight_chunk(jash_highlighter_t* h, const char* buf, size_t len,
                          jash_writer_t w, void* ctx);

/* Flush any buffered output */
long jash_highlight_flush(jash_highlighter_t* h, jash_writer_t w, void* ctx);

#endif /* JASH_OUTPUT_HIGHLIGHTER_H */
