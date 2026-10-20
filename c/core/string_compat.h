// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_STRING_COMPAT_H
#define JASH_STRING_COMPAT_H

#include <ctype.h>
#include <stddef.h>

static inline const char* jash_strcasestr_portable(const char* haystack,
                                                    const char* needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

#endif
