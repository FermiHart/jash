// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/err_digest — compact error digest for noisy build tools
//
// When JASH_ERR_DIGEST=1, stderr from cmake/make/gcc/clang/cargo/npm is
// captured, re-emitted verbatim, then followed by a compact [err-digest]
// block listing only the actual error lines. Saves LLMs from scanning
// 50+ lines to find the 2-3 that matter.

#ifndef JASH_CORE_ERR_DIGEST_H
#define JASH_CORE_ERR_DIGEST_H

#include <theme/io.h>

// Returns 1 if the command's basename is a known noisy build tool.
int jash_err_digest_is_noisy_tool(const char* fullpath);

// Returns 1 if the line matches a compiler/build error signature. Shared with
// the F4 salience scorer (ADR-0042) — one signature set, not two.
int jash_err_is_error_line(const char* line);

// Read tmppath, re-emit all lines to fd 2 (stderr), then print a compact
// error digest via writer w. tmppath is NOT unlinked here — caller must do it.
void jash_err_digest_run(const char* tmppath, jash_writer_t w, void* ctx);

/* ADR-0038 W4: fold-aware variant. In agent sessions (JASH_ERR_FOLD != 0)
 * the stderr cascade is FOLDED: first instance per root cause + sibling
 * counts + warning/noise totals + one machine-readable summary line; the
 * full stderr moves (rename, zero copy) to ~/.jash/overflow/ as a
 * recall-able file. Non-agent (or JASH_ERR_FOLD=0) falls back to the
 * passthrough+digest behavior above. `toolpath` names the overflow file. */
void jash_err_digest_run2(const char* tmppath, const char* toolpath,
                          jash_writer_t w, void* ctx);

#endif
