// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_AI_PATCH_H
#define JASH_CORE_AI_PATCH_H

#include <stddef.h>
#include <core/recovery.h>

typedef struct {
    char before_blake3[65];
    char after_blake3[65];
    jash_recovery_ref_t recovery;
    int applied;
} jash_ai_patch_result_t;

/* Replace a small source file in cwd only when its entire contents match the
 * supplied BLAKE3. A successful mutation has a captured native undo inverse.
 * On failure, result.applied distinguishes a post-mutation journal error. */
int jash_ai_patch_replace(const char* path, const char* before_blake3,
                          const char* text, size_t text_length,
                          jash_ai_patch_result_t* result);
/* Complete BLAKE3 of an admitted <=256 KiB C/C++ source in cwd. */
int jash_ai_source_digest(const char* path, char out[65]);
/* Replace exactly one matching byte span in an admitted source, with full-file
 * precondition and the same verified backup/undo receipt as replace. */
int jash_ai_patch_hunk(const char* path, const char* before_blake3,
                       const char* old_text, size_t old_length,
                       const char* new_text, size_t new_length,
                       jash_ai_patch_result_t* result);
#endif
