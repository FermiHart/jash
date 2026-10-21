// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_SOURCE_LOCATION_H
#define JASH_SOURCE_LOCATION_H
#include <stddef.h>
/* Lexical provenance, never guessed by matching command text. A NULL map
 * describes original, unmodified source starting at first_line. Scopes borrow
 * their text/map; persistent function definitions own a copy of the map. */
typedef struct jash_source_span {
    const char *text;
    size_t length;
    const unsigned *lines;
    unsigned first_line;
    struct jash_source_span *previous;
} jash_source_span;
void jash_source_push(jash_source_span *, const char *, size_t,
                      const unsigned *, unsigned);
void jash_source_pop(jash_source_span *);
jash_source_span *jash_source_mark(void);
void jash_source_restore(jash_source_span *);
unsigned jash_source_line_at(const char *);
unsigned *jash_source_map_slice(const char *, size_t);
#endif
