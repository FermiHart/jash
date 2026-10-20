// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/loopguard — anti-loop circuit breaker (ADR-0042 F3).
//
// Detects when an agent retries the same failing command with micro-variations.
// 3rd consecutive similar failure → warn (stderr). 5th → block (rc 2, no exec).
// "Similar" = same argv[0] + Jaccard(remaining tokens) >= 0.7.
//
// F E R M I ∞ H A R T
#ifndef JASH_CORE_LOOPGUARD_H
#define JASH_CORE_LOOPGUARD_H

#include <theme/io.h>

/* Verdict for a command about to execute.
 * Returns: 0 = ok, 1 = warn (stderr hint), 2 = block (rc 2, skip exec).
 * `cmd` is the full command line, `name` is argv[0]. */
int jash_loopguard_verdict(const char* name, const char* cmd);

/* Post-exec hook: record the command and its rc for future verdicts.
 * Called after every command completes. */
void jash_loopguard_record(const char* name, const char* cmd, int rc);

#endif /* JASH_CORE_LOOPGUARD_H */
