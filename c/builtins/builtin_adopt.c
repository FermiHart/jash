// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/adopt — adoption governor control panel (ADR-0040 W2).
//
// Usage:
//   adopt                 → status: level, source, cumulative metrics
//   adopt --json          → machine-readable status
//   adopt nudge | on      → persist level 1 (hints on stderr only)
//   adopt rewrite         → persist level 2 (W3+: upgrade bash forms)
//   adopt max             → persist level 3 (W3+: rewrite + destructive floor)
//   adopt off             → persist level 0
//   adopt clear           → remove the persisted toggle (back to env/inference)
//
// The resolved level is decided in core/adopt.c — ONE precedence chain shared
// by the scorecard, the JSON envelope and (W3) the pre-exec engine.

#include "semantic_policy.h"
#include "builtin.h"
#include <core/adopt.h>
#include <core/guard.h>
#include <core/jash_invoice.h>
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* level_name(int lv) {
    switch (lv) {
        case 0: return "off";
        case 1: return "nudge";
        case 2: return "rewrite";
        default: return "aggressive";
    }
}

int jash_b_adopt(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "adopt", "jash.adopt.v1", args, semantic_policy);
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

    /* toggles — persist via core/adopt.c so every consumer agrees. */
    if (!strcmp(a, "off"))                       return jash_adopt_set(0) ? 1 : 0;
    if (!strcmp(a, "on") || !strcmp(a, "nudge")) return jash_adopt_set(1) ? 1 : 0;
    if (!strcmp(a, "rewrite"))                   return jash_adopt_set(2) ? 1 : 0;
    if (!strcmp(a, "max"))                       return jash_adopt_set(3) ? 1 : 0;
    if (!strcmp(a, "clear"))                     return jash_adopt_set(-1) ? 1 : 0;

    int json = strcmp(a, "--json") == 0;
    if (*a && !json && strcmp(a, "status") != 0) {
        jash_writes(w, ctx,
            "usage: adopt [status|--json|off|nudge|rewrite|max|clear]\r\n");
        return 1;
    }

    int lv  = jash_adopt_level();
    int src = jash_adopt_source();
    unsigned t = 0, n = 0, b = 0, bl = 0, up = 0;
    jash_guard_metrics_snapshot(&t, &n, &b, &bl, &up);
    int pct = t ? (int)((n * 100 + t / 2) / t) : 0;

    if (json) {
        jash_writef(w, ctx,
            "{\"level\":%d,\"level_name\":\"%s\",\"source\":\"%s\","
            "\"total\":%u,\"native\":%u,\"bypass\":%u,\"blocked\":%u,"
            "\"upgraded\":%u,\"pct\":%d}\n",
            lv, level_name(lv), jash_adopt_source_name(src),
            t, n, b, bl, up, pct);
        return 0;
    }

    jash_writes(w, ctx, "\033[1;36m═══ adoption governor ═══\033[0m\r\n");
    jash_writef(w, ctx, "  level               %d (%s) — source: %s\r\n",
                lv, level_name(lv), jash_adopt_source_name(src));
    jash_writef(w, ctx, "  commands recorded   %u\r\n", t);
    jash_writef(w, ctx, "  jash-native         %u (%d%% adoption · %u shell-upgraded)\r\n",
                n, pct, up);
    jash_writef(w, ctx, "  bypass attempts     %u\r\n", b);
    jash_writef(w, ctx, "  blocked             %u\r\n", bl);
    jash_writes(w, ctx, "  levels: 0 off · 1 nudge · 2 rewrite · 3 aggressive"
                        " — persist: adopt off|nudge|rewrite|max|clear\r\n");
    return 0;
}

SEMANTIC_ARGV_WRAPPER(adopt, "adopt", "jash.adopt.v1", semantic_policy)
