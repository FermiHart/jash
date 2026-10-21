// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "source_location.h"
#include <stdint.h>
#include <stdlib.h>
static jash_source_span *active;
jash_source_span *jash_source_mark(void) { return active; }
void jash_source_restore(jash_source_span *mark) { active = mark; }
void jash_source_push(jash_source_span *s, const char *text, size_t length,
                      const unsigned *lines, unsigned first_line) {
    s->text = text; s->length = length; s->lines = lines;
    s->first_line = first_line; s->previous = active; active = s;
}
void jash_source_pop(jash_source_span *s) { active = s->previous; }
unsigned jash_source_line_at(const char *p) {
    uintptr_t address = (uintptr_t)p;
    for (jash_source_span *s = active; s; s = s->previous) {
        uintptr_t base = (uintptr_t)s->text;
        if (address < base || address - base > s->length) continue;
        size_t offset = (size_t)(address - base);
        if (s->lines) return s->lines[offset];
        unsigned line = s->first_line;
        for (size_t i = 0; i < offset; ++i) if (s->text[i] == '\n') ++line;
        return line;
    }
    return active ? active->first_line : 1;
}
unsigned *jash_source_map_slice(const char *text, size_t length) {
    if (length > SIZE_MAX / sizeof(unsigned) - 1) return NULL;
    unsigned *map = malloc((length + 1) * sizeof(*map));
    if (!map) return NULL;
    uintptr_t address = (uintptr_t)text;
    for (jash_source_span *s = active; s; s = s->previous) {
        uintptr_t base = (uintptr_t)s->text;
        if (address < base || address - base > s->length) continue;
        size_t offset = (size_t)(address - base);
        unsigned line = jash_source_line_at(text);
        for (size_t i = 0; i <= length; ++i) {
            if (offset + i <= s->length) {
                if (s->lines) line = s->lines[offset + i];
                else if (i && s->text[offset + i - 1] == '\n') ++line;
            }
            map[i] = line;
        }
        return map;
    }
    unsigned line = active ? active->first_line : 1;
    for (size_t i = 0; i <= length; ++i) {
        map[i] = line;
        if (i < length && text[i] == '\n') ++line;
    }
    return map;
}
