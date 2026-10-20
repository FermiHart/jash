// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_llm_invoice — Token economics for LLM sessions
// F E R M I ∞ H A R T
//
// Usage:
//   llm invoice begin "fix cd tab"    start a metered session
//   llm invoice close                 print session invoice and add to lifetime
//   llm invoice status                show lifetime + active session
//   llm invoice reset                 clear lifetime counters (sovereign)

#include "semantic_policy.h"
#include <builtins/builtin.h>
#include <core/state.h>
#include <core/jash_invoice.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>

static const char* skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

int jash_b_llm_invoice(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "llm-invoice", "jash.llm-invoice.v1", args, semantic_invoice);
    if (structured >= 0) return structured;
    if (!s || !w) return 1;
    jash_invoice_init(NULL);

    const char* a = skip_ws(args);
    if (!a || !*a) {
        jash_invoice_print_status(w, ctx);
        if (jash_invoice_session_active()) {
            jash_writes(w, ctx, "\nactive session:\n");
            jash_invoice_print_session(w, ctx, jash_invoice_current());
        }
        return 0;
    }

    if (strncmp(a, "begin", 5) == 0) {
        const char* intent = skip_ws(a + 5);
        char buf[256];
        buf[0] = '\0';
        if (intent && *intent) {
            size_t n = strlen(intent);
            if (n >= 2 && ((intent[0] == '"' && intent[n-1] == '"') ||
                           (intent[0] == '\'' && intent[n-1] == '\''))) {
                n -= 2;
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, intent + 1, n);
                buf[n] = '\0';
                intent = buf;
            }
        }
        if (jash_invoice_begin(intent && *intent ? intent : NULL) < 0) {
            jash_writes(w, ctx, "[llm invoice] session already active; close it first\n");
            return 1;
        }
        jash_writef(w, ctx, "[llm invoice] session started: %s\n",
                    intent && *intent ? intent : "(no intent)");
        return 0;
    }

    if (strncmp(a, "close", 5) == 0) {
        jash_invoice_stats_t st;
        if (jash_invoice_close(&st) < 0) {
            jash_writes(w, ctx, "[llm invoice] no active session\n");
            return 1;
        }
        jash_invoice_print_session(w, ctx, jash_invoice_current());
        jash_invoice_save();
        return 0;
    }

    if (strncmp(a, "status", 6) == 0) {
        jash_invoice_print_status(w, ctx);
        if (jash_invoice_session_active()) {
            jash_writes(w, ctx, "\nactive session:\n");
            jash_invoice_print_session(w, ctx, jash_invoice_current());
        }
        return 0;
    }

    if (strncmp(a, "reset", 5) == 0) {
        memset((void*)jash_invoice_lifetime(), 0, sizeof(jash_invoice_stats_t));
        jash_invoice_save();
        jash_writes(w, ctx, "[llm invoice] lifetime counters reset\n");
        return 0;
    }

    jash_writef(w, ctx, "llm invoice: unknown subcommand '%s'\n", a);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(llm_invoice, "llm-invoice", "jash.llm-invoice.v1", semantic_invoice)
