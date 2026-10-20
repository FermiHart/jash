// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/suggest — inline error recovery suggestions.
//
// Provides "did you mean ...?" suggestions for misspelled commands and
// similar file paths when a command or path fails.

#ifndef JASH_CORE_SUGGEST_H
#define JASH_CORE_SUGGEST_H

#include <theme/io.h>

/* Suggest a similar file path when a file is not found.
 * Scans the directory containing bad_path and prints the closest match
 * if edit distance <= 3. */
void jash_suggest_file(jash_writer_t w, void* ctx, const char* bad_path);

#endif /* JASH_CORE_SUGGEST_H */
