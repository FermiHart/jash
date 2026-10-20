// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_DEBUG_TOKENS_H
#define JASH_BUILTINS_DEBUG_TOKENS_H
#include <stddef.h>
typedef int (*debug_token_callback_t)(void*, const char*, size_t, size_t);
/* The original debug --parse inspection is a quote-aware whitespace scanner,
 * not the shell parser. Share exactly that scanner between presentations. */
static inline int debug_scan_tokens(const char* line, debug_token_callback_t callback,
        void* context, int* single_open, int* double_open) {
    int sq = 0, dq = 0; const char* start = NULL;
    for (const char* at = line;; at++) {
        if (!*at || (!sq && !dq && (*at == ' ' || *at == '\t'))) {
            if (start && callback(context, start, (size_t)(at - start), (size_t)(start - line))) return -1;
            start = NULL;
        } else if (!start) start = at;
        if (!*at) break;
        if (*at == '\'' && !dq) sq = !sq;
        else if (*at == '"' && !sq) dq = !dq;
    }
    if (single_open) *single_open = sq;
    if (double_open) *double_open = dq;
    return 0;
}
#endif
