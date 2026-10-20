// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include "../c/core/serve.c"
#include <assert.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !setenv("HOME", argv[1], 1));
    assert(!srv_store_bind() && !flock(srv_store_lease, LOCK_EX));
    uint64_t used, limit, available = 0; unsigned entries; int known, applied = 0;
    assert(!maintenance_journal_usage(&used, &entries) && !used && !entries);
    assert(!srv_store_policy_limit("journal_quota", &limit) && !limit);
    assert(!maintenance_publish(".staging", "retained", 8, 0));
    assert(!maintenance_journal_usage(&used, &entries) && used == 8 && entries == 1);
    assert(!strcmp(maintenance_journal_admit(used + STORE_JOURNAL_HEADROOM - 1, used, entries, 1, &available, &known), "journal_quota_exceeded"));
    assert(!known);
    assert(!maintenance_journal_admit(used + STORE_JOURNAL_HEADROOM, used, entries, 1, &available, &known));
    assert(known && available >= STORE_JOURNAL_HEADROOM);
    assert(!strcmp(maintenance_journal_admit(UINT64_MAX, UINT64_MAX - 1024, entries, 1, &available, &known), "journal_quota_exceeded"));
    assert(!strcmp(maintenance_journal_admit(UINT64_MAX, used, entries, 0, &available, &known), "journal_measurement_incomplete"));
    assert(!maintenance_journal_admit(0, UINT64_MAX, UINT_MAX, 0, &available, &known) && !known);
    assert(!strcmp(maintenance_journal_admit(UINT64_MAX, used, SRV_STORE_MAX_FILES - 5, 1, &available, &known), "journal_entry_limit"));
    assert(!srv_store_policy_publish("journal_quota", used + STORE_JOURNAL_HEADROOM, &applied) && applied);
    assert(!srv_store_policy_limit("journal_quota", &limit) && limit == used + STORE_JOURNAL_HEADROOM);
    int fd = srv_store_dir("agent_maintenance"); assert(fd >= 0);
    assert(!linkat(fd, ".staging", fd, "hardlink", 0));
    assert(maintenance_journal_usage(&used, &entries));
    assert(!unlinkat(fd, "hardlink", 0) && !symlinkat(".staging", fd, "link"));
    assert(maintenance_journal_usage(&used, &entries)); close(fd);
    fd = srv_store_dir("agent_store"); assert(fd >= 0);
    int policy = openat(fd, "journal_quota", O_WRONLY | O_TRUNC | O_CLOEXEC); assert(policy >= 0);
    assert(write(policy, "bad\n", 4) == 4); close(policy); close(fd);
    assert(srv_store_policy_limit("journal_quota", &limit));
    close(srv_store_lease); srv_store_lease = -1;
    return 0;
}
