// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_DISCOVERY_OUTPUT_H
#define JASH_BUILTINS_DISCOVERY_OUTPUT_H

#include "builtin.h"
#include <core/data_value.h>

enum { JASH_DISCOVERY_TEXT, JASH_DISCOVERY_JSON, JASH_DISCOVERY_DATA };
#define JASH_DISCOVERY_ROWS_MAX 4096u
struct jash_discovery_effects;

/* Bounded native records shared by the JSON and JTV2 adapters. No rendered
 * human output is parsed or captured. Pointees remain owned through emit. */
typedef struct {
    jdv_arena_t arena;
    void* storage;
    jdv_value_t* items;
    size_t count;
    int error;
    struct jash_discovery_effects* effects;
} jash_discovery_rows_t;

/* Formats may appear before --; operands after -- never select a mode.
 * A negative result means duplicate/conflicting selectors. mode remains set
 * so callers can publish the failure in the selected transport. */
int jash_discovery_format(int argc, char* const* argv, int* mode);
int jash_discovery_format_flag(const char* argument);
int jash_discovery_rows_init(jash_discovery_rows_t*);
void jash_discovery_rows_free(jash_discovery_rows_t*);
jdv_value_t jash_discovery_string(jash_discovery_rows_t*, const char*);
jdv_value_t jash_discovery_path(jash_discovery_rows_t*, const char*);
int jash_discovery_add(jash_discovery_rows_t*, const jdv_record_input_t*, size_t);
int jash_discovery_emit(jash_writer_t, void*, const char* command, int mode,
                        jash_discovery_rows_t*, int command_status);
int jash_discovery_emit_schema(jash_writer_t, void*, const char* command, int mode,
                               jash_discovery_rows_t*, int command_status, const char* schema_id);
/* Native source outcomes may retain observed rows while declaring FAILED or
 * CANCELLED. Read-only query providers must not turn an incomplete read into
 * a healthy terminal merely because they collected an error row. */
int jash_discovery_emit_data_outcome(jash_writer_t, void*, const char* command,
                                     jash_discovery_rows_t*, int command_status,
                                     jdv_stream_state_t, const char* reason);
const jdv_value_t* jash_discovery_field(const jdv_value_t* record, const char* field);
int jash_discovery_error(jash_writer_t, void*, const char* command, int mode,
                         const char* code, int command_status);
int jash_discovery_text(jash_builtin_fn, jash_state_t*, jash_writer_t, void*, int, char**);
int jash_discovery_resolve(jash_state_t*, jash_discovery_rows_t*, const char*, int all, int path_only);
/* Native loader callbacks may print diagnostics. Structured callers direct
 * those bytes to stderr while retaining stdout for the semantic result. */
int jash_discovery_callback_begin(int mode, int* saved_stdout);
int jash_discovery_callback_end(int saved_stdout);

/* Stateful operations validate arguments, then admit their checked response
 * before the first effect. Flush each result after the action and stop on
 * failure before the next effect. JSON mutation receipts, like mutation_report,
 * are exempt from the prose budget; collection/frame limits still apply.
 * begin returns 0 or an already-reported shell status. flush returns 0/125.
 * end returns the final shell status; emit_schema calls it automatically when
 * a session is active. rows_free releases an abandoned session safely. */
int jash_discovery_effects_begin(jash_discovery_rows_t*, jash_writer_t, void*,
                                  const char* command, int mode, const char* schema_id);
int jash_discovery_effects_flush(jash_discovery_rows_t*);
int jash_discovery_effects_end(jash_discovery_rows_t*, int command_status);
/* Explicit source outcome for streaming native providers. JSON mutation
 * envelopes retain their collection-complete contract. */
int jash_discovery_effects_end_outcome(jash_discovery_rows_t*, int command_status,
                                      jdv_stream_state_t, int truncated, const char* reason);

#endif
