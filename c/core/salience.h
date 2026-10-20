// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/salience — ranked output under budget (ADR-0042 F4).
//
// The context governor caps output by POSITION (head/tail), but the decisive
// line is usually in the middle. When JASH_GOVERNOR=salient, an overflowing
// command's output is RANKED instead of tail-capped: each line is scored
//   3·error_signature + 2·novel(vs this command's previous output)
//   + 1·rare(pattern frequency in this output) + 0.5·edge(first/last 5)
// and the top selection (under JASH_LLM_BUDGET) is emitted in original order,
// with a pattern-histogram footer and the recall handle to the full output.
//
// Novelty uses a salience-local previous-output cache (~/.jash/salience/<cmd>.prev)
// rather than the F1 memo store: the memo whitelist is read-only tools, but the
// governor mostly fires on builds/tests (make, ctest), which are not memoized.
// Knobs: JASH_GOVERNOR=tail|salient, JASH_SALIENT_K (default: budget ÷ ~80 B).
// F E R M I ∞ H A R T
#ifndef JASH_CORE_SALIENCE_H
#define JASH_CORE_SALIENCE_H

#include <theme/io.h>

// 1 when JASH_GOVERNOR=salient.
int jash_governor_is_salient(void);

// Salient replacement for jash_budget_flush's tail path. Reads tmppath, and:
//   - if the output fits under `limit`, emits it verbatim (no salience tax);
//   - else ranks lines, emits the top selection in original order + a histogram
//     footer, and writes the FULL output to ~/.jash/overflow/ for recall.
// Returns 1 = everything fit, 0 = ranked/capped, -1 = declined (too large / I/O)
// → the caller falls back to the classic tail path.
int jash_salience_flush(const char* tmppath, long limit, const char* cmdname,
                        int strip_ansi, jash_writer_t w, void* ctx);

#endif /* JASH_CORE_SALIENCE_H */
