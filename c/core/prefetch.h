// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/prefetch — the anticipatory shell (ADR-0042 F5).
//
// Agent loops are predictable: edit → build → test → git status. The shell is
// idle during agent "think time". Prefetch learns the command-transition graph
// from the chain and, at the end of a -c call, speculatively runs the most
// probable NEXT commands — only read-only memoizable ones — so the next call
// hits a warm memo (near-zero tokens).
//
// Transition table ~/.jash/prefetch.tsv: from_cmd \t to_cmd \t count \t last_ts
// (flocked, 10k-row cap). Fire rule: a successor of this call's last command
// with count >= 3 → run the top <= 2 (memoizable) as children with
// JASH_PREFETCH_CHILD=1 (recursion guard), within JASH_PREFETCH_MAX_MS. Never
// under JASH_SAFE. Default OFF (JASH_PREFETCH=1 to enable — dogfood, then flip).
// F E R M I ∞ H A R T
#ifndef JASH_CORE_PREFETCH_H
#define JASH_CORE_PREFETCH_H

// 1 when JASH_PREFETCH=1, this is not a prefetch child, and not JASH_SAFE.
int jash_prefetch_enabled(void);

// Learn the transition prev_cmd → cur_cmd (increment its count in prefetch.tsv).
void jash_prefetch_learn(const char* prev_cmd, const char* cur_cmd);

// After cur_cmd, speculatively run its most-probable memoizable successors
// (count >= 3, top <= 2) as children (JASH_PREFETCH_CHILD=1), blocking up to the
// wall budget then killing survivors. No-op unless jash_prefetch_enabled().
void jash_prefetch_fire(const char* cur_cmd);

#endif /* JASH_CORE_PREFETCH_H */
