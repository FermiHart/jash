// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: dirs — display directory stack

#include "builtin.h"
#include <core/env.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int split_stack(const char* stack, char* tokens[], int max) {
    char* copy = strdup(stack ? stack : "");
    if (!copy) return 0;
    int count = 0;
    char* tok = strtok(copy, " ");
    while (tok && count < max) {
        tokens[count++] = tok;
        tok = strtok(NULL, " ");
    }
    return count;
}

/* Bash parity: dirs prints paths with $HOME compressed to '~'. */
static void write_tilde_path(jash_writer_t w, void* ctx, const char* path) {
    const char* home = getenv("HOME");
    if (home && *home) {
        size_t hl = strlen(home);
        if (strncmp(path, home, hl) == 0 &&
            (path[hl] == '\0' || path[hl] == '/')) {
            jash_writes(w, ctx, "~");
            jash_writes(w, ctx, path + hl);
            return;
        }
    }
    jash_writes(w, ctx, path);
}

int jash_b_dirs(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;
    const char* p = skip_ws(args ? args : "");
    int clear = 0;
    int verbose = 0;

    while (*p == '-') {
        p++;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == 'c') clear = 1;
            else if (*p == 'v') verbose = 1;
            else if (*p == 'l') { /* long format — same as default */ }
            else if (*p == 'p') { /* physical — same as default */ }
            else { jash_writef(w, ctx, "dirs: invalid option -- '%c'\n", *p); return 2; }
            p++;
        }
        p = skip_ws(p);
    }

    if (clear) {
        jash_env_set(s, "_DIRSTACK", "");
        return 0;
    }

    const char* stack_str = jash_env_get(s, "_DIRSTACK");
    if (!stack_str || stack_str[0] == '\0') {
        char cwd[1024];
        long len = sys_getcwd(cwd, sizeof(cwd));
        if (len >= 0) {
            write_tilde_path(w, ctx, cwd);
            jash_writes(w, ctx, "\n");
        }
        return 0;
    }

    if (verbose) {
        char* tokens[128];
        int tc = split_stack(stack_str, tokens, 128);
        for (int i = 0; i < tc; i++) {
            jash_writef(w, ctx, "%d ", i);
            write_tilde_path(w, ctx, tokens[i]);
            jash_writes(w, ctx, "\n");
        }
    } else {
        char* tokens[128];
        int tc = split_stack(stack_str, tokens, 128);
        for (int i = 0; i < tc; i++) {
            if (i) jash_writes(w, ctx, " ");
            write_tilde_path(w, ctx, tokens[i]);
        }
        jash_writes(w, ctx, "\n");
    }

    return 0;
}
