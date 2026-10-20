// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: popd — pop directory from stack and change directory

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

static void rebuild_stack(char* buf, size_t cap, char* tokens[], int count) {
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) strncat(buf, " ", cap - strlen(buf) - 1);
        strncat(buf, tokens[i], cap - strlen(buf) - 1);
    }
}

int jash_b_popd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)w; (void)ctx;
    const char* stack_str = jash_env_get(s, "_DIRSTACK");
    if (!stack_str || stack_str[0] == '\0') {
        jash_writes(w, ctx, "popd: directory stack empty\n");
        return 1;
    }

    char stack[4096];
    strlcpy(stack, stack_str, sizeof(stack));
    stack[sizeof(stack) - 1] = '\0';

    char* tokens[128];
    int tc = split_stack(stack, tokens, 128);

    if (tc == 0) { jash_writes(w, ctx, "popd: directory stack empty\n"); return 1; }

    const char* p = skip_ws(args ? args : "");

    if (*p == '+' || *p == '-') {
        int from_right = (*p == '-');
        p++;
        char* end;
        long n = strtol(p, &end, 10);
        if (end == p) { jash_writes(w, ctx, "popd: missing number\n"); return 1; }
        long idx = from_right ? tc - n - 1 : n;
        if (idx < 0 || idx >= tc) { jash_writes(w, ctx, "popd: no such directory\n"); return 1; }
        char* dir = tokens[idx];
        for (int i = (int)idx; i < tc - 1; i++) tokens[i] = tokens[i + 1];
        tc--;
        for (int i = tc; i > 0; i--) tokens[i] = tokens[i - 1];
        tokens[0] = dir;
        tc++;
        if (sys_chdir(dir) < 0) {
            jash_writef(w, ctx, "popd: %s: error\n", dir);
            return 1;
        }
        long len = sys_getcwd(s->cwd, sizeof(s->cwd));
        if (len >= 0) jash_env_set(s, "PWD", s->cwd);
        char ns[4096];
        rebuild_stack(ns, sizeof(ns), tokens, tc);
        jash_env_set(s, "_DIRSTACK", ns);
        jash_writes(w, ctx, ns);
        jash_writes(w, ctx, "\n");
        return 0;
    }

    if (tc < 2) { jash_writes(w, ctx, "popd: directory stack empty\n"); return 1; }
    for (int i = 0; i < tc - 1; i++) tokens[i] = tokens[i + 1];
    tc--;
    if (sys_chdir(tokens[0]) < 0) {
        jash_writef(w, ctx, "popd: %s: error\n", tokens[0]);
        return 1;
    }
    long len = sys_getcwd(s->cwd, sizeof(s->cwd));
    if (len >= 0) jash_env_set(s, "PWD", s->cwd);
    char ns[4096];
    rebuild_stack(ns, sizeof(ns), tokens, tc);
    jash_env_set(s, "_DIRSTACK", ns);
    jash_writes(w, ctx, ns);
    jash_writes(w, ctx, "\n");
    return 0;
}
