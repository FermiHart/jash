// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/argv — quote-aware argv tokenizer (implementation)

#include "argv.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

char* jash_argv_join(int argc, char* const* argv) {
    if (argc < 0 || (argc && !argv)) { errno = EINVAL; return NULL; }
    size_t size = 1;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) { errno = EINVAL; return NULL; }
        if (size > SIZE_MAX - 3) { errno = EOVERFLOW; return NULL; }
        size += 3;
        for (const char* p = argv[i]; *p; p++) {
            size_t extra = *p == '\'' ? 4u : 1u;
            if (size > SIZE_MAX - extra) { errno = EOVERFLOW; return NULL; }
            size += extra;
        }
    }
    char* result = malloc(size);
    if (!result) return NULL;
    char* p = result;
    for (int i = 0; i < argc; i++) {
        if (i) *p++ = ' ';
        *p++ = '\'';
        for (const char* a = argv[i]; *a; a++) {
            if (*a == '\'') { memcpy(p, "'\\''", 4); p += 4; }
            else *p++ = *a;
        }
        *p++ = '\'';
    }
    *p = '\0';
    return result;
}

void jash_argv_free(jash_argv_t* out) {
    jash_argv_free_into(out, NULL);
}

int jash_argv_build(const char* line, jash_argv_t* out) {
    return jash_argv_build_into(line, out, NULL);
}

void jash_argv_free_into(jash_argv_t* out, jash_argv_scratch_t* scratch) {
    if (!out) return;
    if (out->argv && (!scratch || out->argv != scratch->argv)) free(out->argv);
    if (out->backing && (!scratch || out->backing != scratch->backing)) free(out->backing);
    out->argv = NULL;
    out->backing = NULL;
    out->argc = 0;
}

int jash_argv_build_into(const char* line, jash_argv_t* out,
                         jash_argv_scratch_t* scratch) {
    if (!out) return -1;
    out->argc = 0;
    out->argv = NULL;
    out->backing = NULL;
    if (!line || !*line) return 0;

    size_t len = strlen(line);
    /* Unquoting only ever shrinks a token, so len+1 bytes is always enough
     * backing storage for every token's content plus its NUL. */
    char* backing = out->backing = scratch && len < sizeof scratch->backing
        ? scratch->backing : (char*)malloc(len + 1);
    if (!backing) return -1;
    size_t argv_cap = scratch ? sizeof scratch->argv / sizeof scratch->argv[0]
                              : len / 2 + 2;
    char** argv = out->argv = scratch ? scratch->argv
        : (char**)malloc(sizeof(char*) * argv_cap);
    if (!argv) { jash_argv_free_into(out, scratch); return -1; }

    size_t bo = 0;
    int argc = 0;
    const char* p = line;
    /* BUG-094: quote state PERSISTS across \x1E array-element boundaries —
     * all the elements of a quoted "${a[@]}" share one surrounding quote
     * pair, and each \x1E ends one argv word (even inside "...") without
     * closing the quote. Without this, builtins received the raw \x1E bytes
     * inside a single argument. */
    int in_single = 0, in_double = 0;

    while (*p) {
        if (!in_single && !in_double)
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\x1E') p++;
        if (!*p) break;

        /* Reserve the word plus the final NULL. Quoted RS separators can
         * produce empty words more densely than the heap sizing estimate. */
        if ((size_t)argc + 1 >= argv_cap) {
            size_t next_cap = argv_cap * 2;
            if (next_cap < len / 2 + 2) next_cap = len / 2 + 2;
            int borrowed = scratch && argv == scratch->argv;
            char** next = borrowed ? (char**)malloc(sizeof(char*) * next_cap)
                : (char**)realloc(argv, sizeof(char*) * next_cap);
            if (!next) { jash_argv_free_into(out, scratch); return -1; }
            if (borrowed) memcpy(next, argv, (size_t)argc * sizeof(char*));
            argv = out->argv = next;
            argv_cap = next_cap;
        }
        argv[argc] = backing + bo;

        while (*p) {
            char c = *p;

            if (c == '\x1E') break;   /* element boundary — new argv word */
            if (c == '\'' && !in_double) { in_single = !in_single; p++; continue; }
            if (c == '"'  && !in_single) { in_double = !in_double; p++; continue; }

            if (!in_single && !in_double && (c == ' ' || c == '\t' || c == '\n'))
                break;

            if (in_double && c == '\\' && p[1] &&
                (p[1] == '$' || p[1] == '"' || p[1] == '\\' || p[1] == '\n')) {
                p++;
                if (*p == '\n') { p++; continue; }  /* line continuation: drop both */
                backing[bo++] = *p++;
                continue;
            }
            if (!in_single && !in_double && c == '\\' && p[1]) {
                backing[bo++] = p[1];
                p += 2;
                continue;
            }

            backing[bo++] = c;
            p++;
        }

        if ((in_single || in_double) && !*p) {
            /* Unterminated quote at end of input — same contract as
             * jash_unquote(). (Open state at a \x1E boundary is fine.) */
            jash_argv_free_into(out, scratch);
            return -1;
        }

        backing[bo++] = '\0';
        argc++;
        if (*p == '\x1E') p++;
    }

    /* BUG-096: restore shielded literal quotes in every token */
    {
        extern void jash_quote_unshield(char* s);
        for (int i = 0; i < argc; i++) jash_quote_unshield(argv[i]);
    }
    argv[argc] = NULL;
    out->argc = argc;
    out->argv = argv;
    out->backing = backing;
    return argc;
}
