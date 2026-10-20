// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: array — shell arrays via Rust FFI

#include "builtin.h"
#include "shell_output.h"
#include <core/env.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdlib.h>
#include <jash_compat.h>

/* array NAME[index]=VALUE — set array element */
/* array NAME — show array */
int jash_b_array(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "array", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)s;
    if (!args || !*args) {
        jash_writef(w, ctx, "usage: array NAME[INDEX]=VALUE or array NAME\n");
        return 1;
    }

    /* Parse NAME[INDEX]=VALUE */
    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;

    /* Extract name */
    char name[64];
    unsigned i = 0;
    while (*p && *p != '[' && *p != ' ' && i < sizeof(name) - 1)
        name[i++] = *p++;
    name[i] = '\0';

    /* Check for [INDEX]=VALUE */
    if (*p == '[') {
        p++; /* skip [ */
        const char* idx_start = p;
        while (*p && *p != ']') p++;
        if (*p != ']') {
            jash_writef(w, ctx, "array: missing ]\n");
            return 1;
        }
        /* Parse index */
        char idx_str[32];
        unsigned idx_len = (unsigned)(p - idx_start);
        if (idx_len >= sizeof(idx_str)) idx_len = sizeof(idx_str) - 1;
        for (unsigned j = 0; j < idx_len; j++) idx_str[j] = idx_start[j];
        idx_str[idx_len] = '\0';
        size_t index = (size_t)atoi(idx_str);
        p++; /* skip ] */

        /* Expect = */
        if (*p != '=') {
            jash_writef(w, ctx, "array: expected = after ]\n");
            return 1;
        }
        p++; /* skip = */

        /* Value is rest of line */
        while (*p == ' ' || *p == '\t') p++;

        /* Create or get array and set value */
        ShellArray* arr = bearjash_array_new_indexed();
        bearjash_array_set(arr, idx_str, p);
        char key[128];
        unsigned ko = 0;
        for (unsigned j = 0; name[j] && ko + 1 < sizeof(key); j++) key[ko++] = name[j];
        if (ko + 1 < sizeof(key)) key[ko++] = '[';
        for (unsigned j = 0; idx_str[j] && ko + 1 < sizeof(key); j++) key[ko++] = idx_str[j];
        if (ko + 1 < sizeof(key)) key[ko++] = ']';
        key[ko] = '\0';
        jash_env_set(s, key, p);
        /* For now, just print what we set */
        const char* val = bearjash_array_get(arr, idx_str);
        jash_writef(w, ctx, "%s[%d]=%s\n", name, (int)index, val ? val : "");
        bearjash_array_free_string((char*)val);
        bearjash_array_free(arr);
        return 0;
    }

    /* Just NAME — show array info */
    jash_writef(w, ctx, "array: %s (use NAME[INDEX]=VALUE to set)\n", name);
    return 0;
}
