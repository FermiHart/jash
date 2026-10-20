// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Shared descriptors for discovery and pre-effect runtime validation (ADR-0056).
#ifndef JASH_CORE_AGENT_CONTRACT_H
#define JASH_CORE_AGENT_CONTRACT_H

#include "json_writer.h"
#include <stdint.h>

#define JASH_AGENT_CONTRACT_VERSION 1
#define JASH_AGENT_ID_VALUE_CAP 48u
/* Worst-case \u00XX for every decoded byte, quotes/NUL and escaper slack. */
#define JASH_AGENT_ID_JSON_CAP (6u * JASH_AGENT_ID_VALUE_CAP + 3u)

enum {
    SRV_JOB_SEEN_ID         = 1u << 0,
    SRV_JOB_SEEN_VERSION    = 1u << 1,
    SRV_JOB_SEEN_OP         = 1u << 2,
    SRV_JOB_SEEN_CAPS       = 1u << 3,
    SRV_JOB_SEEN_IR         = 1u << 4,
    SRV_JOB_SEEN_ARGV       = 1u << 5,
    SRV_JOB_SEEN_BACKGROUND = 1u << 6,
    SRV_JOB_SEEN_PTY        = 1u << 7,
    SRV_JOB_SEEN_ROWS       = 1u << 8,
    SRV_JOB_SEEN_COLS       = 1u << 9,
    SRV_JOB_SEEN_JOB_ID     = 1u << 10,
    SRV_JOB_SEEN_WAIT       = 1u << 11,
    SRV_JOB_SEEN_GRACE      = 1u << 12,
    SRV_JOB_SEEN_STREAM     = 1u << 13,
    SRV_JOB_SEEN_OFFSET     = 1u << 14,
    SRV_JOB_SEEN_LIMIT      = 1u << 15,
    SRV_JOB_SEEN_CWD        = 1u << 16,
    SRV_JOB_SEEN_ENV        = 1u << 17,
    SRV_JOB_SEEN_STDIN_B64  = 1u << 18,
    SRV_JOB_SEEN_DATA_B64   = 1u << 19,
    SRV_JOB_SEEN_STDIN_MODE = 1u << 20,
    SRV_JOB_SEEN_EXECUTION_SPEC = 1u << 21,
    SRV_JOB_SEEN_RESOURCES  = 1u << 22,
    SRV_JOB_SEEN_MAX_BYTES = 1u << 23,
    SRV_JOB_SEEN_AGE = 1u << 24,
    SRV_JOB_SEEN_DRY_RUN = 1u << 25,
    SRV_JOB_SEEN_ROOT_NS = 1u << 26,
    SRV_JOB_SEEN_ROOT_NAME = 1u << 27,
    SRV_JOB_SEEN_ROOT_HASH = 1u << 28,
    SRV_JOB_SEEN_TARGET_ID = 1u << 29,
    SRV_JOB_SEEN_CURSOR = 1u << 30,
};

#define SRV_JOB_SEEN_REVISION (UINT64_C(1) << 31)
#define SRV_JOB_SEEN_FD_ENABLED (UINT64_C(1) << 32)
#define SRV_JOB_SEEN_ARTIFACT_REF (UINT64_C(1) << 33)
#define SRV_JOB_SEEN_TRANSPORT (UINT64_C(1) << 34)
#define SRV_JOB_SEEN_STREAM_REF (UINT64_C(1) << 35)
#define SRV_JOB_SEEN_CREDIT_BYTES (UINT64_C(1) << 36)
#define SRV_JOB_SEEN_ACK_OFFSET (UINT64_C(1) << 37)

enum {
    SRV_SEEN_ID = 1u << 0, SRV_SEEN_ARGV = 1u << 1,
    SRV_SEEN_LINE = 1u << 2, SRV_SEEN_STDIN = 1u << 3,
    SRV_SEEN_TIMEOUT = 1u << 4, SRV_SEEN_CWD = 1u << 5,
    SRV_SEEN_CAPS = 1u << 6, SRV_SEEN_ENV = 1u << 7,
    SRV_SEEN_CANCELABLE = 1u << 8, SRV_SEEN_STDIN_B64 = 1u << 9,
    SRV_SEEN_VERSION = 1u << 10, SRV_SEEN_EXECUTION_SPEC = 1u << 11,
};

unsigned jash_sync_contract_allowed(void);
const char* jash_sync_contract_fields(unsigned seen, int argc, int version);

/* Builtin-name classification is explicit and independent of the compiled
 * profile. It does not certify every subcommand/option of that name. The
 * compiled registry remains the denominator for discovery and execution. */
typedef enum {
    JASH_CONTRACT_STRUCTURED,
    JASH_CONTRACT_EXECUTION_ENVELOPE,
    JASH_CONTRACT_INTERACTIVE
} jash_builtin_contract_role_t;

typedef enum {
    JASH_CONTRACT_OUTPUT_NONE,
    JASH_CONTRACT_OUTPUT_NATIVE,
    JASH_CONTRACT_OUTPUT_ALTERNATE,
    JASH_CONTRACT_OUTPUT_MODULE
} jash_builtin_contract_output_t;

typedef struct {
    const char* name;
    const char* alias_of; /* NULL when not a documented alias. */
    const char* source;
    jash_builtin_contract_role_t role;
    jash_builtin_contract_output_t json_output;
    jash_builtin_contract_output_t data_output;
    int operations_complete;
    const char* operation_exclusions_json; /* Explicit non-provider operations; never omitted from scope. */
    const char* legacy_output_compatibility_json; /* Historical selector/shape boundaries, not qualified mode aliases. */
} jash_builtin_contract_t;

/* A concrete mode is deliberately narrower than the complete builtin. Its
 * input schema describes structural argv selection, not semantic admission;
 * the handler must still validate combinations, operands and authority.
 * options_json contains immutable JSON descriptors authored with the handler,
 * never request-derived JSON. NULL result_schema means no checked JSON schema
 * is claimed (for example, JTV1/JTV2 carry their own in-band typed schemas). */
typedef struct {
    const char* name;
    const char* id;
    const char* mode; /* "json" or "data" */
    const char* selector; /* NULL for a default structured interface. */
    const char* selector_scope;
    const char* framing; /* json, jsonl, jtv1, jtv2 */
    const char* result_schema;
    const char* options_json;
    const char* error_policy;
    const char* notes;
    const char* error_schema; /* Separate failure document; NULL if unmodeled. */
    const char* input_framing; /* NULL unmodeled; none, bytes, jsonl, jtv2. */
    const char* resource_limits_json; /* Scoped, explicit adapter bounds. */
    const char* operations_json; /* Reviewed operation grammar and effects. */
    const char* semantic_tests_json; /* Executable qualification gates. */
    const char* qualification_scope_json; /* Platforms actually qualified. */
    int qualified;
    int options_complete;
} jash_builtin_mode_contract_t;

const jash_builtin_contract_t* jash_builtin_contract_find(const char* name);
const jash_builtin_contract_t* jash_builtin_contract_at(size_t index);
size_t jash_builtin_contract_count(void);
const jash_builtin_mode_contract_t* jash_builtin_mode_contract_at(const char* name, size_t index);
size_t jash_builtin_mode_contract_count(const char* name);

typedef struct {
    const char* name;
    uint64_t allowed;
    uint64_t required;
    const char* capability; /* NULL means dynamic capability validation. */
    const char* missing_error;
    const char* summary;
} jash_job_contract_t;

const jash_job_contract_t* jash_job_contract_find(const char*);
const jash_job_contract_t* jash_job_contract_at(size_t);
size_t jash_job_contract_count(void);
/* Structural validation is shared with discovery; semantic/capability and
 * resource checks remain mandatory in serve.c before performing effects. */
const char* jash_job_contract_fields(const jash_job_contract_t*, uint64_t seen);
/* --contract-json is opt-in discovery only; no shell state or plugins load. */
int jash_agent_contract_json(const char* selector, jash_json_sink_t, void*);

#endif
