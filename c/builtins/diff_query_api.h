// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_DIFF_QUERY_API_H
#define JASH_BUILTINS_DIFF_QUERY_API_H
#include <core/state.h>
#include <theme/io.h>
#include <stdint.h>

/* Snapshot readers resolve their source paths once, then reuse the normal
 * diff observation engine. The logical path/checkpoint label the observation;
 * neither serializer reopens a file or invokes an external command. */
int jash_diff_query_snapshot(jash_state_t*, jash_writer_t, void*,
    const char* actual_before, const char* actual_after, const char* logical_path,
    const char* checkpoint, int native, uint64_t offset, uint64_t limit);
int jash_diff_query_error(jash_state_t*, jash_writer_t, void*,
    const char* command, const char* code, int native, int status);
#endif
