/* SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Small, bounded C-family syntax coloring shared by cat and the AI renderer.
 * This changes presentation only; callers keep the original source bytes.
 */
#ifndef JASH_CORE_CODE_HIGHLIGHT_H
#define JASH_CORE_CODE_HIGHLIGHT_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

typedef long (*jash_code_writer_t)(void* context, const char* bytes, size_t length);

typedef struct {
    int in_block_comment;
} jash_c_highlight_state_t;

typedef struct {
    char triple_quote;
    int expects_definition_name;
} jash_python_highlight_state_t;

static inline int jash_code_write(jash_code_writer_t writer, void* context,
                                  const char* bytes, size_t length) {
    if (!length) return 0;
    long written = writer(context, bytes, length);
    return written == (long)length ? 0 : -1;
}

static inline int jash_code_emit_safe(jash_code_writer_t writer, void* context,
                                      const char* text, size_t length) {
    size_t run = 0;
    for (size_t i = 0; i <= length; i++) {
        unsigned char c = i < length ? (unsigned char)text[i] : 0;
        if (i < length && c >= 0x20 && c != 0x7f) continue;
        if (i > run && jash_code_write(writer, context, text + run, i - run) < 0)
            return -1;
        if (i < length && c == '\t' &&
            jash_code_write(writer, context, "    ", 4) < 0) return -1;
        run = i + 1u;
    }
    return 0;
}

static inline int jash_code_is_word(const char* word, size_t length,
                                    const char* const words[]) {
    for (size_t i = 0; words[i]; i++)
        if (strlen(words[i]) == length && !memcmp(word, words[i], length)) return 1;
    return 0;
}

static inline int jash_c_highlight_c_line(jash_code_writer_t writer, void* context,
                                          const char* line, size_t length,
                                          jash_c_highlight_state_t* state) {
    static const char* const keywords[] = {
        "auto", "break", "case", "continue", "default", "do", "else",
        "for", "goto", "if", "return", "switch", "while", "sizeof",
        "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn",
        "_Static_assert", "_Thread_local", NULL
    };
    static const char* const types[] = {
        "char", "const", "double", "enum", "extern", "float", "inline",
        "int", "long", "register", "restrict", "short", "signed", "static",
        "struct", "typedef", "union", "unsigned", "void", "volatile",
        "_Bool", "_Complex", "_Imaginary", NULL
    };
    static const char* const color_comment = "\033[38;5;114m";
    static const char* const color_string = "\033[38;5;114m";
    static const char* const color_keyword = "\033[38;5;81m";
    static const char* const color_type = "\033[38;5;141m";
    static const char* const color_number = "\033[38;5;214m";
    static const char* const color_function = "\033[38;5;75m";
    static const char* const color_directive = "\033[38;5;220m";
    static const char reset[] = "\033[0m";
    if (!writer || (!line && length) || !state) return -1;

    size_t i = 0;
    while (i < length) {
        size_t start = i;
        const char* color = NULL;
        if (state->in_block_comment) {
            while (i < length) {
                if (i + 1u < length && line[i] == '*' && line[i + 1u] == '/') {
                    i += 2u;
                    state->in_block_comment = 0;
                    break;
                }
                i++;
            }
            color = color_comment;
        } else if (line[i] == '/' && i + 1u < length && line[i + 1u] == '/') {
            i = length;
            color = color_comment;
        } else if (line[i] == '/' && i + 1u < length && line[i + 1u] == '*') {
            i += 2u;
            state->in_block_comment = 1;
            while (i < length) {
                if (i + 1u < length && line[i] == '*' && line[i + 1u] == '/') {
                    i += 2u;
                    state->in_block_comment = 0;
                    break;
                }
                i++;
            }
            color = color_comment;
        } else if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i++];
            while (i < length) {
                if (line[i] == '\\' && i + 1u < length) { i += 2u; continue; }
                if (line[i++] == quote) break;
            }
            color = color_string;
        } else if (line[i] == '#' &&
                   (i == 0 || line[i - 1u] == ' ' || line[i - 1u] == '\t')) {
            i = length;
            color = color_directive;
        } else if (isdigit((unsigned char)line[i])) {
            i++;
            while (i < length && (isalnum((unsigned char)line[i]) ||
                   line[i] == '.' || line[i] == '_')) i++;
            color = color_number;
        } else if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            i++;
            while (i < length && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            size_t word_length = i - start;
            if (jash_code_is_word(line + start, word_length, keywords))
                color = color_keyword;
            else if (jash_code_is_word(line + start, word_length, types))
                color = color_type;
            else {
                size_t next = i;
                while (next < length && (line[next] == ' ' || line[next] == '\t')) next++;
                if (next < length && line[next] == '(') color = color_function;
            }
        } else {
            i++;
        }

        if (color && jash_code_write(writer, context, color, strlen(color)) < 0) return -1;
        if (jash_code_emit_safe(writer, context, line + start, i - start) < 0) return -1;
        if (color && jash_code_write(writer, context, reset, sizeof(reset) - 1u) < 0)
            return -1;
    }
    return 0;
}

static inline int jash_code_emit_color(jash_code_writer_t writer, void* context,
                                       const char* color, const char* text,
                                       size_t length) {
    static const char reset[] = "\033[0m";
    if (jash_code_write(writer, context, color, strlen(color)) < 0 ||
        jash_code_emit_safe(writer, context, text, length) < 0 ||
        jash_code_write(writer, context, reset, sizeof(reset) - 1u) < 0)
        return -1;
    return 0;
}

static inline int jash_code_highlight_python_line(
    jash_code_writer_t writer, void* context, const char* line, size_t length,
    jash_python_highlight_state_t* state) {
    static const char* const keywords[] = {
        "and", "as", "assert", "async", "await", "break", "class",
        "continue", "def", "del", "elif", "else", "except", "finally",
        "for", "from", "global", "if", "import", "in", "is", "lambda",
        "nonlocal", "not", "or", "pass", "raise", "return", "try",
        "while", "with", "yield", NULL
    };
    static const char* const literals[] = {
        "False", "None", "True", "bool", "bytes", "dict", "float", "int",
        "list", "object", "set", "str", "tuple", NULL
    };
    static const char keyword_color[] = "\033[38;5;81m";
    static const char string_color[] = "\033[38;5;114m";
    static const char literal_color[] = "\033[38;5;141m";
    static const char number_color[] = "\033[38;5;214m";
    static const char decorator_color[] = "\033[38;5;220m";
    static const char comment_color[] = "\033[38;5;114m";
    if (!writer || (!line && length) || !state) return -1;

    size_t i = 0;
    while (i < length) {
        size_t start = i;
        const char* color = NULL;
        if (state->triple_quote) {
            char quote = state->triple_quote;
            while (i + 2u < length &&
                   !(line[i] == quote && line[i + 1u] == quote &&
                     line[i + 2u] == quote)) i++;
            if (i + 2u < length) {
                i += 3u;
                state->triple_quote = 0;
            } else i = length;
            color = string_color;
        } else if (line[i] == '#') {
            i = length;
            color = comment_color;
        } else if (line[i] == '\'' || line[i] == '"') {
            char quote = line[i];
            int triple = i + 2u < length && line[i + 1u] == quote &&
                         line[i + 2u] == quote;
            i += triple ? 3u : 1u;
            int closed = 0;
            while (i < length) {
                if (line[i] == '\\' && i + 1u < length) { i += 2u; continue; }
                if (triple && i + 2u < length && line[i] == quote &&
                    line[i + 1u] == quote && line[i + 2u] == quote) {
                    i += 3u;
                    closed = 1;
                    break;
                }
                if (!triple && line[i++] == quote) { closed = 1; break; }
                else if (triple) i++;
            }
            if (triple && !closed) state->triple_quote = quote;
            color = string_color;
            state->expects_definition_name = 0;
        } else if (line[i] == '@' &&
                   (i == 0 || isspace((unsigned char)line[i - 1u]))) {
            i++;
            while (i < length && (isalnum((unsigned char)line[i]) ||
                                  line[i] == '_' || line[i] == '.')) i++;
            color = decorator_color;
        } else if (isdigit((unsigned char)line[i])) {
            i++;
            while (i < length && (isalnum((unsigned char)line[i]) ||
                                  line[i] == '.' || line[i] == '_')) i++;
            color = number_color;
            state->expects_definition_name = 0;
        } else if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            i++;
            while (i < length && (isalnum((unsigned char)line[i]) ||
                                  line[i] == '_')) i++;
            size_t word_length = i - start;
            if (state->expects_definition_name) {
                color = "\033[38;5;75m";
                state->expects_definition_name = 0;
            } else if (jash_code_is_word(line + start, word_length, keywords)) {
                color = keyword_color;
                state->expects_definition_name =
                    (word_length == 3u && !memcmp(line + start, "def", 3u)) ||
                    (word_length == 5u && !memcmp(line + start, "class", 5u));
            } else {
                color = jash_code_is_word(line + start, word_length, literals)
                    ? literal_color : NULL;
                state->expects_definition_name = 0;
            }
        } else {
            i++;
            if (!isspace((unsigned char)line[start]))
                state->expects_definition_name = 0;
        }
        if (color) {
            if (jash_code_emit_color(writer, context, color, line + start,
                                     i - start) < 0) return -1;
        } else if (jash_code_emit_safe(writer, context, line + start,
                                       i - start) < 0) return -1;
    }
    return 0;
}

static inline int jash_code_highlight_make_line(jash_code_writer_t writer,
                                                void* context,
                                                const char* line,
                                                size_t length) {
    static const char keyword_color[] = "\033[38;5;81m";
    static const char variable_color[] = "\033[38;5;141m";
    static const char comment_color[] = "\033[38;5;114m";
    static const char target_color[] = "\033[38;5;75m";
    if (!writer || (!line && length)) return -1;
    size_t first = 0;
    while (first < length && (line[first] == ' ' || line[first] == '\t')) first++;
    size_t i = 0;
    while (i < length) {
        size_t start = i;
        const char* color = NULL;
        if (line[i] == '#' && (i == 0 || line[i - 1u] != '\\')) {
            i = length;
            color = comment_color;
        } else if (line[i] == '$' && i + 1u < length &&
                   (line[i + 1u] == '(' || line[i + 1u] == '{')) {
            char close = line[i + 1u] == '(' ? ')' : '}';
            i += 2u;
            while (i < length && line[i] != close) i++;
            if (i < length) i++;
            color = variable_color;
        } else if (line[i] == '$' && i + 1u < length) {
            i += 2u;
            color = variable_color;
        } else if (i == first &&
                   (line[i] == '.' || isalpha((unsigned char)line[i]))) {
            while (i < length && !isspace((unsigned char)line[i]) &&
                   line[i] != ':' && line[i] != '=' && line[i] != '?') i++;
            size_t word_length = i - start;
            static const char* const directives[] = {
                "define", "endef", "export", "ifdef", "ifndef", "ifeq",
                "ifneq", "include", "-include", "override", "private",
                "sinclude", "unexport", "vpath", ".PHONY", ".DEFAULT",
                ".DELETE_ON_ERROR", ".SUFFIXES", NULL
            };
            color = jash_code_is_word(line + start, word_length, directives)
                ? keyword_color : target_color;
        } else {
            i++;
        }
        if (color) {
            if (jash_code_emit_color(writer, context, color, line + start,
                                     i - start) < 0) return -1;
        } else if (jash_code_emit_safe(writer, context, line + start,
                                       i - start) < 0) return -1;
    }
    return 0;
}

#endif
