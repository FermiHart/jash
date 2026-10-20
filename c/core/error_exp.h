// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — error_exp — inline error explanation.
//
// When a command fails, show WHY it failed and WHAT to do.
// This is what Microsoft's "Intelligent Terminal" does with AI —
// Jash does it without AI, with pattern matching on stderr.

#ifndef JASH_CORE_ERROR_EXP_H
#define JASH_CORE_ERROR_EXP_H

#include <core/state.h>

/* Explain a command failure. Writes explanation to `out`.
 * Returns: 1 if explanation was written, 0 if no explanation available.
 * `cmd` = the command name, `exit_code` = the exit status,
 * `stderr_buf` = captured stderr output (may be NULL). */
int jash_error_explain(const char* cmd, int exit_code,
                       const char* stderr_buf,
                       char* out, unsigned out_cap);

/* Auto-suggest a fix for common errors.
 * Writes suggestion to `out`. Returns 1 if suggestion was made. */
int jash_error_suggest_fix(const char* cmd, int exit_code,
                           const char* stderr_buf,
                           char* out, unsigned out_cap);

#endif /* JASH_CORE_ERROR_EXP_H */
