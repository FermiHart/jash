// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_MESH_SNAPSHOT_H
#define JASH_BUILTINS_MESH_SNAPSHOT_H
#include <core/state.h>
#include <stdint.h>

/* Counts are valid only when complete is true. An absent store is a complete
 * empty observation; malformed or unreadable sources are explicit failures.
 * This provider never creates a store or prunes expired claims. Liveness is
 * solely an explicit harness PID checked with kill(pid, 0), not a heartbeat
 * inference. The same collectors and limits back mesh's native query modes. */
typedef struct {
    uint64_t workers, workers_live, workers_dead, workers_unknown;
    uint64_t tasks, tasks_ready, tasks_claimed, tasks_done, tasks_failed;
    uint64_t claims, claims_live, claims_expired;
    int64_t observed_at;
    int store_present, complete, status, error_number;
    const char* reason;
} jash_mesh_snapshot_t;

int jash_mesh_snapshot_counts(jash_state_t* state, jash_mesh_snapshot_t* snapshot);
#endif
