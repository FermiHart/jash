// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — editor/snippet — multi-line snippet editor (Ctrl+E).

#ifndef JASH_EDITOR_SNIPPET_H
#define JASH_EDITOR_SNIPPET_H

#include <theme/io.h>
#include <core/state.h>
#include <readline/loop.h>

typedef struct jash_editor_session jash_editor_session_t;

/* Create editor session */
jash_editor_session_t* jash_editor_session_create(jash_state_t* s);

/* Destroy editor session */
void jash_editor_session_destroy(jash_editor_session_t* es);

/* Open editor with initial content. Returns edited content (caller frees) or NULL on cancel. */
char* jash_editor_session_run(jash_editor_session_t* es,
                              jash_reader_t r, void* rctx,
                              jash_writer_t w, void* wctx,
                              const char* initial);

#endif /* JASH_EDITOR_SNIPPET_H */
