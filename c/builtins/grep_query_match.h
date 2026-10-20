// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Independent programs preserve each -e pattern's BRE/ERE/fixed semantics.
 * Combining their source text into one ERE would change backreferences,
 * anchors and literal characters. Choose the leftmost, longest actual span. */
#ifndef JASH_BUILTINS_GREP_QUERY_MATCH_H
#define JASH_BUILTINS_GREP_QUERY_MATCH_H

typedef struct {
    char* text;
    regex_t program;
    int using_regex;
} grep_query_matcher_t;

static void grep_query_patterns_free(grep_query_t* query) {
    grep_query_matcher_t* matchers = query->matchers;
    for (size_t i = 0; i < query->matcher_count; i++) {
        if (matchers[i].using_regex) regfree(&matchers[i].program);
        free(matchers[i].text);
    }
    free(matchers); query->matchers = NULL; query->matcher_count = 0;
}

static int grep_query_patterns_compile(const char* fallback, int flags) {
    grep_query_t* query = g_grep_query;
    grep_query_matcher_t* matchers = calloc(64u, sizeof(*matchers));
    if (!matchers) return -1;
    query->matchers = matchers;
    size_t count = query->pattern_count ? query->pattern_count : 1u;
    for (size_t i = 0; i < count; i++) {
        const char* source = query->pattern_count ? query->pattern_sources[i] : fallback;
        do {
            const char* newline = strchr(source, '\n');
            size_t length = newline ? (size_t)(newline - source) : strlen(source);
            if (query->matcher_count == 64u) { grep_query_patterns_free(query); return -1; }
            grep_query_matcher_t* matcher = &matchers[query->matcher_count++];
            matcher->text = strndup(source, length);
            if (!matcher->text || grep_compile_program(matcher->text, flags & 0x06 ? flags | 0x400 : flags,
                                                       &matcher->program, &matcher->using_regex)) {
                grep_query_patterns_free(query); return -1;
            }
            source = newline ? newline + 1 : NULL;
        } while (source);
    }
    return 0;
}

static int grep_query_span_multi(const char* line, int flags, int from, int* start, int* end) {
    const grep_query_t* query = g_grep_query;
    const grep_query_matcher_t* matchers = query->matchers;
    size_t line_size = strlen(line);
    if (from < 0 || (size_t)from > line_size) return 0;
    int found = 0;
    for (size_t i = 0; i < query->matcher_count; i++) {
        const grep_query_matcher_t* matcher = &matchers[i];
        int position = from;
        while ((size_t)position <= line_size) {
            int left, right;
            if (matcher->using_regex) {
                regmatch_t match;
                if (regexec(&matcher->program, line + position, 1u, &match, position ? REG_NOTBOL : 0)) break;
                left = position + (int)match.rm_so; right = position + (int)match.rm_eo;
            } else if (flags & 0x40000) {
                if (position || ((flags & 0x01) ? strcasecmp(line, matcher->text) : strcmp(line, matcher->text))) break;
                left = 0; right = (int)line_size;
            } else {
                const char* match = (flags & 0x01) ? jash_strcasestr_portable(line + position, matcher->text)
                    : strstr(line + position, matcher->text);
                if (!match) break;
                left = (int)(match - line); right = left + (int)strlen(matcher->text);
            }
            if (!(flags & 0x4000) || ((!left || !grep_is_word(line[left - 1])) &&
                                      (!line[right] || !grep_is_word(line[right])))) {
                if (!found || left < *start || (left == *start && right > *end)) {
                    *start = left; *end = right; found = 1;
                }
                break;
            }
            position = left + 1;
        }
    }
    return found;
}

#endif
