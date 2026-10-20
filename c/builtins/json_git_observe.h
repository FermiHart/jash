// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_JSON_GIT_OBSERVE_H
#define JASH_BUILTINS_JSON_GIT_OBSERVE_H
#include "semantic_value.h"

typedef struct {
    int command_status, adapter_status;
    int status_command_status, log_command_status;
    int status_started, log_started, log_requested, truncated;
    uint64_t output_bytes;
} jash_git_observation_t;

/* Read-only fixed Git plumbing, with optional locks, fsmonitor, pagers and
 * signature commands disabled. No stdout emission or shell evaluation.
 * Each child captures at most 256 KiB and drains the remaining output; excess
 * is an explicit error. Status supports 512 paths, latest log at most one row.
 * The returned RECORD owns its data in values->arena and contains schema,
 * status and latest_commit observations. Each observation has available,
 * started, command_status, adapter_status, output_bytes, truncated, payload,
 * and diagnostic_bytes (raw combined bytes only on command failure).
 * Missing repository/HEAD is never converted to clean/empty successful data.
 * If status fails, the optional log is not attempted. Return is adapter status
 * first, otherwise child status; both are also independently available. */
int jash_git_observe(semantic_value_t* values, int include_latest_commit,
                    jdv_value_t* result, jash_git_observation_t* outcome);
/* Same bounded observation envelope, for the actual unstaged diff numstat.
 * Diff drivers/textconv are disabled; raw paths, binary counts and failures
 * use the exact same collector as json git diff --stat. */
int jash_git_diff_stat_observe(semantic_value_t* values, jdv_value_t* result,
                              jash_git_observation_t* outcome);
#endif
