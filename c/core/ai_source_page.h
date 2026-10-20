// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_AI_SOURCE_PAGE_H
#define JASH_CORE_AI_SOURCE_PAGE_H

#include <stddef.h>

/* Bounded, read-only source page in the current working directory. Only a
 * regular, nofollow C/C++ source basename is admitted. On failure out is
 * empty. The source is live, not a sealed observation or durable ref. */
int jash_ai_source_page(const char* path, size_t offset, size_t limit,
                         char* out, size_t capacity);

#endif
