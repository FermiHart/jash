/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_UTIL_H
#define JASH_UTIL_H

#include <ctype.h>
#include <string.h>

/* Skip whitespace (space, tab, newline) */
static inline const char* skip_ws(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Skip non-whitespace */
static inline const char* skip_nonws(const char* s) {
    while (*s && !isspace((unsigned char)*s)) s++;
    return s;
}

/* Check if string is empty or whitespace-only */
static inline int is_blank(const char* s) {
    while (*s) {
        if (!isspace((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

/* Safe string length with NULL check */
static inline size_t safe_strlen(const char* s) {
    return s ? strlen(s) : 0;
}

/* Trim leading whitespace (returns pointer into original string) */
static inline char* trim_left(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Trim trailing whitespace (modifies string in place) */
static inline void trim_right(char* s) {
    if (!s || !*s) return;
    char* end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}

/* Trim both sides (modifies in place, returns pointer) */
static inline char* trim(char* s) {
    s = trim_left(s);
    trim_right(s);
    return s;
}

#endif /* JASH_UTIL_H */
