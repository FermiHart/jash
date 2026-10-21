// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CHECKED_STRING_H
#define JASH_CHECKED_STRING_H
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Exact C-string copy. Unlike strlcpy, failure never publishes a truncated
 * prefix: the destination remains unchanged. Scan at most capacity bytes,
 * and allow overlapping ranges (including copying a string onto itself).
 * Source must provide readable bytes up to its NUL or capacity, whichever
 * comes first. Returns 0 on success, -1 with EINVAL/E2BIG on failure. */
static inline int jash_copy_cstr(char* destination, size_t capacity,
                                const char* source) {
    if (!destination || !source) { errno = EINVAL; return -1; }
    size_t length = 0;
    while (length < capacity && source[length]) length++;
    if (length == capacity) { errno = E2BIG; return -1; }
    memmove(destination, source, length + 1);
    return 0;
}
#endif
