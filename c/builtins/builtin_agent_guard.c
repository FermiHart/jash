// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/agent-guard — agent anti-bypass control panel.
//
// Usage:
//   agent-guard status        → adoption / bypass / block metrics
//   agent-guard on            → enable bypass monitoring (JASH_GUARD_BYPASS=1)
//   agent-guard off           → disable
//   agent-guard strict        → block bypasses (JASH_GUARD_BYPASS_STRICT=1)
//   agent-guard relaxed       → warn only
//
// Default config (~/.jashrc):
//   export JASH_GUARD=1
//   export JASH_GUARD_BYPASS=1
//   export JASH_GUARD_BYPASS_STRICT=1
// When monitoring is active, every `bash -c`, `sh -c`, `python -c`, etc.
// is counted as a bypass. In strict mode it is blocked and the agent is
// forced to use jash primitives.

#include "semantic_policy.h"
#include "builtin.h"
#include <core/guard.h>
#include <core/jash_invoice.h>
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int jash_b_agent_guard(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "agent-guard", "jash.agent-guard.v1", args, semantic_policy);
    if (structured >= 0) return structured;
    (void)s;
    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;
    {
        uint64_t n = strlen(a) / 4;
        if (n < 1) n = 1;
        jash_invoice_account_command(a, 1);
        jash_invoice_add_saved(JASH_INVOICE_GUARD, n);
    }

    if (!*a || strcmp(a, "status") == 0) {
        unsigned total, native, bypass, blocks;
        jash_guard_agent_status(w, ctx, &total, &native, &bypass, &blocks);
        return 0;
    }
    if (strcmp(a, "on") == 0) {
        jash_guard_bypass_set(1, 0);
        jash_writes(w, ctx, "agent-guard: bypass monitoring ON (watch)\n");
        return 0;
    }
    if (strcmp(a, "off") == 0) {
        jash_guard_bypass_set(0, 0);
        jash_writes(w, ctx, "agent-guard: bypass monitoring OFF\n");
        return 0;
    }
    if (strcmp(a, "strict") == 0) {
        jash_guard_bypass_set(1, 1);
        jash_writes(w, ctx, "agent-guard: STRICT mode — bypasses will be BLOCKED\n");
        return 0;
    }
    if (strcmp(a, "relaxed") == 0) {
        jash_guard_bypass_set(1, 0);
        jash_writes(w, ctx, "agent-guard: RELAXED mode — bypasses warned but allowed\n");
        return 0;
    }

    jash_writes(w, ctx,
        "agent-guard — agent anti-bypass control panel\n"
        "Usage:\n"
        "  agent-guard status     show adoption / bypass / block metrics\n"
        "  agent-guard on         enable bypass monitoring\n"
        "  agent-guard off        disable bypass monitoring\n"
        "  agent-guard strict     block bypasses (force jash primitives)\n"
        "  agent-guard relaxed    warn on bypasses but allow them\n");
    return 1;
}

SEMANTIC_ARGV_WRAPPER(agent_guard, "agent-guard", "jash.agent-guard.v1", semantic_policy)
