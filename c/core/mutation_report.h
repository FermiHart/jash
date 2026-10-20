// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_MUTATION_REPORT_H
#define JASH_CORE_MUTATION_REPORT_H

#include "json_writer.h"
#include <theme/io.h>
#include <signal.h>
#include "recovery.h"

typedef enum { JASH_MUT_FAILED, JASH_MUT_COMPLETED, JASH_MUT_SKIPPED,
               JASH_MUT_PLANNED, JASH_MUT_PARTIAL } jash_mutation_status_t;
typedef enum { JASH_MUT_NONE, JASH_MUT_CHANGED, JASH_MUT_POSSIBLE } jash_mutation_effect_t;
typedef enum { JASH_MUTATION_TEXT = 0, JASH_MUTATION_JSON = 1, JASH_MUTATION_DATA = 2 } jash_mutation_format_t;
typedef struct {
    const char* source;
    const char* destination;
    const char* action;
    const char* code;
    jash_mutation_status_t status;
    jash_mutation_effect_t effect;
    uint64_t bytes_written;
    int bytes_known;
    int verification; /* 0 not run, 1 passed, -1 failed */
    jash_recovery_ref_t undo;
    char* recovery_path;
    size_t recovery_capacity;
} jash_mutation_item_t;

typedef struct jash_mutation_report {
    jash_json_writer_t writer;
    uint64_t completed, skipped, failed, planned, partial, items;
    int preview;
    const char* error_code;
    const char* transaction;
    struct sigaction old_pipe;
    int pipe_installed;
    struct sigaction old_int, old_term;
    int int_installed, term_installed;
    volatile sig_atomic_t cancelled;
    sig_atomic_t previous_cancelled;
    struct jash_mutation_report* previous;
    struct jash_mutation_data* native;
} jash_mutation_report_t;

/* Detect/remove --json only in the option prefix, respecting -- and values of
 * caller-supplied options. 0 = legacy (no allocation), 1 = JSON, -1 = allocation
 * failure after detecting JSON. Filtered array borrows strings; caller frees it. */
int jash_mutation_args(int argc, char** argv, const char* const* value_options,
                       int* out_argc, char*** out_argv);
/* Structured modes in the same option prefix: --json, --data or --data-v2.
 * Conflicting JSON/data selectors fail EINVAL before any mutation. The format
 * is retained on allocation failure so callers can report through that format.
 * Return 0 legacy, 1 structured, -1 invalid options/allocation failure. */
int jash_mutation_args_mode(int argc, char** argv, const char* const* value_options,
                            int* out_argc, char*** out_argv, jash_mutation_format_t*);
int jash_mutation_report_begin(jash_mutation_report_t*, const char* operation,
                               const char* granularity, jash_writer_t, void*);
int jash_mutation_report_begin_mode(jash_mutation_report_t*, const char* operation,
                                    const char* granularity, jash_mutation_format_t,
                                    jash_writer_t, void*);
int jash_mutation_report_item(jash_mutation_report_t*, const jash_mutation_item_t*);
/* Delivery-only boundary used during an atomic transaction. It must not claim
 * a completed action or contribute to the mutation summary's item counts. */
int jash_mutation_report_heartbeat(jash_mutation_report_t*);
int jash_mutation_report_end(jash_mutation_report_t*, int rc);
int jash_mutation_report_failed(const jash_mutation_report_t*);
/* Cooperative cancellation at native action/IO boundaries. No active report
 * means legacy signal behavior is unchanged. Return 0 or 128+signal. */
int jash_mutation_cancel_status(void);
int jash_mutation_report_stop(const jash_mutation_report_t*);
/* Synchronous runtime scope. Called only at cooperative report boundaries,
 * never from a signal handler; clear before releasing the request state. */
void jash_mutation_cancel_control(int (*poll)(void*), void* context);
int jash_mutation_cancelable(const char* operation, int argc, char* const* argv);

#endif
