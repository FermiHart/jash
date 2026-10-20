// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_STRUCTURED_OUTPUT_H
#define JASH_CORE_STRUCTURED_OUTPUT_H

#include "json_writer.h"
#include <theme/io.h>
#include <signal.h>

/* A command owns one guard. Begin removes only the known prose governor and
 * enables checked redirect delivery; finish restores the previous SIGPIPE
 * disposition and maps encoding/delivery failure to the reserved status 125. */
typedef struct {
    jash_json_writer_t json;
    struct sigaction previous_sigpipe;
    int restore_sigpipe;
} jash_structured_output_t;
int jash_structured_begin(jash_structured_output_t*, jash_writer_t, void*);
int jash_structured_finish(jash_structured_output_t*, int command_status);
int jash_structured_error(jash_writer_t, void*, const char* command,
                          const char* code, int command_status);

/* Buffers a bounded document before publishing it. Once full it continues
 * counting required bytes, but never allocates past limit or publishes a
 * truncated prefix. A protocol error envelope is exempt from the data budget:
 * even a budget of one byte must be able to explain the failed delivery. */
typedef struct {
    char* data;
    size_t used, capacity, limit, required;
    int error;
} jash_structured_buffer_t;
void jash_structured_buffer_init(jash_structured_buffer_t*, size_t limit);
long jash_structured_buffer_sink(void*, const char*, size_t);
void jash_structured_buffer_free(jash_structured_buffer_t*);
int jash_structured_document(jash_writer_t, void*, const char* command,
                             const char* document, size_t available,
                             size_t required, long budget, int command_status);

#endif
