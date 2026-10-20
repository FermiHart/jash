/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   rust_ffi_stub.c — Pure C stubs for Rust FFI functions
 *
 *   Used when ENABLE_RUST=OFF (pure C build).
 *   Provides minimal implementations of the bearjash_* array/string/highlight
 *   functions that the Rust library normally provides.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/

#include "jash_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sys/syscalls.h"

/* ── Minimal ShellArray implementation ──────────────────────────────────── */

#define MAX_ARRAY_ENTRIES 4096  /* J5: increased from 256 (bash has no limit) */

struct ShellArray {
    char *keys[MAX_ARRAY_ENTRIES];
    char *values[MAX_ARRAY_ENTRIES];
    size_t len;
    int is_associative;
};

ShellArray* bearjash_array_new_indexed(void)
{
    ShellArray *arr = calloc(1, sizeof(ShellArray));
    if (arr) arr->is_associative = 0;
    return arr;
}

ShellArray* bearjash_array_new_associative(void)
{
    ShellArray *arr = calloc(1, sizeof(ShellArray));
    if (arr) arr->is_associative = 1;
    return arr;
}

void bearjash_array_free(ShellArray *arr)
{
    if (!arr) return;
    for (size_t i = 0; i < arr->len; i++) {
        free(arr->keys[i]);
        free(arr->values[i]);
    }
    free(arr);
}

char* bearjash_array_get(const ShellArray *arr, const char *index)
{
    if (!arr || !index) return NULL;

    if (arr->is_associative) {
        for (size_t i = 0; i < arr->len; i++) {
            if (arr->keys[i] && strcmp(arr->keys[i], index) == 0) {
                return arr->values[i] ? strdup(arr->values[i]) : NULL;
            }
        }
        return NULL;
    } else {
        char *end;
        long idx = strtol(index, &end, 10);
        if (*end != '\0' || idx < 0 || (size_t)idx >= arr->len) return NULL;
        return arr->values[idx] ? strdup(arr->values[idx]) : NULL;
    }
}

void bearjash_array_set(ShellArray *arr, const char *index, const char *value)
{
    if (!arr || !index) return;

    if (arr->is_associative) {
        /* Check if key exists */
        for (size_t i = 0; i < arr->len; i++) {
            if (arr->keys[i] && strcmp(arr->keys[i], index) == 0) {
                free(arr->values[i]);
                arr->values[i] = value ? strdup(value) : NULL;
                return;
            }
        }
        /* New key */
        if (arr->len >= MAX_ARRAY_ENTRIES) return;
        arr->keys[arr->len] = strdup(index);
        arr->values[arr->len] = value ? strdup(value) : NULL;
        arr->len++;
    } else {
        char *end;
        long idx = strtol(index, &end, 10);
        if (*end != '\0' || idx < 0) return;
        if ((size_t)idx >= MAX_ARRAY_ENTRIES) return;

        /* Expand if needed */
        if ((size_t)idx >= arr->len) {
            for (size_t i = arr->len; i <= (size_t)idx; i++) {
                arr->keys[i] = NULL;
                arr->values[i] = NULL;
            }
            arr->len = (size_t)idx + 1;
        }
        free(arr->values[idx]);
        arr->values[idx] = value ? strdup(value) : NULL;
    }
}

size_t bearjash_array_len(const ShellArray *arr)
{
    return arr ? arr->len : 0;
}

void bearjash_array_free_string(char *s)
{
    free(s);
}

/* ── BUG-J4 resolution (ADR-0016 §5.4) ─────────────────────────────────────
 * The former bearjash_highlight/complete/string_* passthrough stubs are
 * GONE. They had zero callers — readline wires the native C highlighter
 * (c/output/highlighter.c) and completion (c/completion/) directly — and
 * a passthrough that silently returns unformatted text is a lie waiting
 * for a caller. Dead API removed instead of "fixed". The ShellArray
 * implementation above is real and consumed by c/builtins/array.c. */
