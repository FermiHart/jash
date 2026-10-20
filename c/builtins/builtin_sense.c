// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_sense — alias for `explain --risk` (ADR-0099 W2.3)
// F E R M I ∞ H A R T
//
// sense's job — "how dangerous is this command line?" — is the question
// explain already answers. The risk engine lives in builtin_explain.c as the
// single source of truth; this is a thin compatibility forwarder with no
// logic of its own.

#include <builtins/builtin.h>
#include <core/state.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>

int jash_b_sense(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    const char* p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        jash_writes(w, ctx, "sense: nothing to analyze\r\n");
        return 0;
    }
    char forwarded[4096];
    snprintf(forwarded, sizeof forwarded, "--risk %s", args ? args : "");
    return jash_b_explain(s, w, ctx, forwarded);
}
