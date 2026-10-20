// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash -- core/output_budget -- stdout cap for LLM agent sessions (Feature 1, ADR-0022)

#ifndef JASH_CORE_OUTPUT_BUDGET_H
#define JASH_CORE_OUTPUT_BUDGET_H

#include <stddef.h>
#include <theme/io.h>

long jash_budget_limit(void);
int jash_budget_setup(int* fd_out, char* path_out, size_t path_len);
/* Returns 1 when EVERYTHING was shown (nothing capped/held) — the memo layer
 * (ADR-0038 W1) only records runs the agent saw in full. */
int jash_budget_flush(const char* tmppath, long limit, const char* cmdname,
                       int strip_ansi, jash_writer_t w, void* ctx);
int jash_tmp_path(const char* name, char* buf, size_t cap);

/* Wrapper that enforces JASH_LLM_BUDGET directly on a jash_writer_t.
 * Use this inside builtins that may produce large output. */
typedef struct {
    jash_writer_t real_w;
    void*         real_ctx;
    long          limit;
    long          shown;
    int           overflow_fd;
    char          overflow_path[256];
    const char*   cmdname;
    int           truncated;
} jash_budget_wrap_t;

void jash_budget_wrap_init(jash_budget_wrap_t* bw, jash_writer_t w, void* ctx, const char* cmdname);
jash_writer_t jash_budget_wrap_writer(jash_budget_wrap_t* bw);
void jash_budget_wrap_finish(jash_budget_wrap_t* bw, jash_writer_t notify_w, void* notify_ctx);
/* Typed protocol writers must not be chopped into text + a prose footer.
 * Remove only this known wrapper; retain the caller's actual destination. */
void jash_budget_unwrap(jash_writer_t* writer, void** context);

#endif
