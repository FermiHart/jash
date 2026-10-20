// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Builtin: unset — remove variables

#include "builtin.h"
#include "shell_output.h"
#include <core/timeline.h>
#include <stdio.h>
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <string.h>

int jash_b_unset(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "unset", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)ctx;
    if (!args || !*args) {
        const char* msg = "unset: usage: unset name [...]\n";
        w(0, msg, strlen(msg));
        return 1;
    }

    /* Parse space-separated variable names */
    const char* p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        unsigned len = p - start;
        if (len > 0 && len < 256) {
            char name[256];
            for (unsigned i = 0; i < len; i++) name[i] = start[i];
            name[len] = '\0';
            {
                const char* prev = jash_env_get(s, name);
                if (prev) {
                    char desc[120];
                    snprintf(desc, sizeof(desc), "unset %s", name);
                    jash_timeline_record(JASH_UNDO_ENV_UNSET, desc, name, prev);
                }
            }
            jash_env_unset(s, name);
            /* BUG-078: `unset a` on an ARRAY must also remove every element
             * key a[0], a[1], … a[k] — they are stored as separate env keys,
             * so the bare-name unset left the array populated (${#a[@]}
             * still 3). Sweep by prefix "name[" until none remain (the scan
             * restarts after each removal since the env is mutated). */
            {
                size_t nl = strlen(name);
                int removed;
                do {
                    removed = 0;
                    uint32_t cnt = jash_env_count(s);
                    for (uint32_t i = 0; i < cnt; i++) {
                        const char *k = 0, *v = 0;
                        if (!jash_env_at(s, i, &k, &v)) continue;
                        if (strncmp(k, name, nl) == 0 && k[nl] == '[') {
                            char kb[288];
                            snprintf(kb, sizeof(kb), "%s", k);
                            jash_env_unset(s, kb);
                            removed = 1;
                            break;
                        }
                    }
                } while (removed);
            }
        }
    }
    return 0;
}
