/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* BearJash compat layer — pure C (Rust removed).
 *
 * BUG-J4 (ADR-0016 §5.4): the bearjash_highlight/complete/string_*
 * passthrough declarations were removed together with their stub
 * implementations. They had no callers: readline uses the native C
 * highlighter (c/output/highlighter.c) and completion engine
 * (c/completion/) directly. Only the ShellArray API survives — it is
 * a real implementation consumed by c/builtins/array.c.
 */
#ifndef JASH_COMPAT_H
#define JASH_COMPAT_H

#include <stddef.h>

/* Opaque type for ShellArray */
typedef struct ShellArray ShellArray;

/* Array operations */
ShellArray* bearjash_array_new_indexed(void);
ShellArray* bearjash_array_new_associative(void);
void bearjash_array_free(ShellArray* arr);
char* bearjash_array_get(const ShellArray* arr, const char* index);
void bearjash_array_set(ShellArray* arr, const char* index, const char* value);
size_t bearjash_array_len(const ShellArray* arr);
void bearjash_array_free_string(char* s);

#endif /* JASH_COMPAT_H */
