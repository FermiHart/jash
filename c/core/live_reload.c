// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/live_reload — re-execute jash with state preservation.

#include "live_reload.h"
#include "state.h"
#include <stdio.h>
#include <sys/syscalls.h>
#include <string.h>

int jash_live_reload(jash_state_t* s) {
    if (!s) return 1;

    /* 1. Serialize state to /tmp/jash_state.bin */
    char state_path[256]; snprintf(state_path, sizeof(state_path), "%s/jash_state.bin", jash_tmpdir());
    int rc = jash_state_serialize(s, state_path);
    if (rc < 0) return rc;  /* Serialization failed, abort */

    /* 2. Build argv with --restore flag */
    const char* jash_path = "/bin/jash";
    const char* argv[] = {
        jash_path,
        "--restore=/tmp/jash_state.bin",
        NULL
    };

    /* 3. Execve — does not return on success */
    long ret = sys_execve(jash_path, (char *const *)argv, 0);
    return (int)ret;
}

