// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_AGENT_CGROUP_H
#define JASH_CORE_AGENT_CGROUP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Aggregate cgroup-v2 limits, not per-process rlimits. CPU is bandwidth per
 * period, NOT a cumulative CPU-time budget. Cgroups alone cannot prevent
 * same-UID voluntary migration: a workload sandbox must hide writable cgroupfs
 * (including inherited descriptors) when that threat is in scope. */
typedef struct {
    uint64_t memory_max_bytes;
    uint64_t pids_max;
    uint64_t cpu_quota_us;
    uint64_t cpu_period_us;
    uint64_t wall_timeout_ms;
} jash_agent_limits_t;

int jash_agent_limits_validate(const jash_agent_limits_t* limits,
                               char* error, size_t cap);

typedef struct jash_agent_cgroup jash_agent_cgroup_t;
typedef struct {
    uint64_t memory_events;       /* memory.events max; never inferred from a signal. */
    uint64_t pids_events;         /* pids.events max, including handled EAGAIN. */
    uint64_t pids_current;
    int empty;                   /* cgroup.events populated == 0. */
} jash_agent_cgroup_snapshot_t;

/* Pins parent_fd and creates/configures a unique child. Never changes the
 * parent's subtree_control. All required files and exact readbacks must work;
 * unsupported platforms refuse with ENOTSUP. Caller retains parent_fd. */
int jash_agent_cgroup_create(int parent_fd, const jash_agent_limits_t* limits,
                             jash_agent_cgroup_t** out, char* error, size_t cap);
int jash_agent_cgroup_attach(jash_agent_cgroup_t* group, pid_t pid);
int jash_agent_cgroup_snapshot(jash_agent_cgroup_t* group,
                               jash_agent_cgroup_snapshot_t* snapshot);
int jash_agent_cgroup_kill(jash_agent_cgroup_t* group);
int jash_agent_cgroup_empty(jash_agent_cgroup_t* group); /* 1 empty, 0 populated, -1 error. */
/* Never kills or waits. Removes only its own empty group; releases FDs and
 * memory even on failure. Call only after killing/reaping any workers. */
int jash_agent_cgroup_destroy(jash_agent_cgroup_t* group);

#endif
