// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/chain_read — shared chain.log reader (ADR-0042 Wave A).
//
// Factors the chain-line parser out of builtin_recall.c so F2 (world-delta),
// F3 (loopguard), F7 (blame-file), and F10 (distill) can all read the
// attested execution chain (~/.jash/chain.log) without duplicating sscanf
// soup. Read-only; no crypto — just the text format.
//
// F E R M I ∞ H A R T
#ifndef JASH_CORE_CHAIN_READ_H
#define JASH_CORE_CHAIN_READ_H

#include <stdio.h>

#define CHREAD_CMD_MAX  1024
#define CHREAD_TAG_MAX   32
#define CHREAD_HASH_MAX  80

typedef struct {
    unsigned long long seq;
    unsigned long long ts_sec;
    unsigned long ts_ns;
    int  rc;
    char hash[CHREAD_HASH_MAX];
    char tag[CHREAD_TAG_MAX];
    char cmd[CHREAD_CMD_MAX];
} jash_chain_entry_t;

/* Open chain.log for reading. Returns NULL if HOME unset or file missing. */
FILE* jash_chain_open(void);

/* Parse one line from an open chain file into `e`. Returns 1 on success,
 * 0 on parse failure or EOF (caller loops until 0). */
int jash_chain_next(FILE* f, jash_chain_entry_t* e);

/* Read the last N entries from chain.log into a caller-provided array.
 * Returns the number of entries filled (up to `cap`). Newest-first. */
int jash_chain_tail(jash_chain_entry_t* buf, int cap, int n);

/* Tokenize a command string into argv-style tokens (simple whitespace split,
 * respecting single/double quotes). Returns token count.
 * NOTE: tokens are NOT NUL-terminated (for idcmp). */
int jash_chain_tokenize(const char* cmd, char** argv, int max_argv);

/* Tokenize with NUL-termination (BUG-152: needed for jaccard similarity).
 * Copies tokens into work_buf and NUL-terminates each. Returns token count,
 * or -1 when arguments, workspace or token capacity cannot hold the complete
 * input. A failure never authorizes callers to consume a partial token set. */
int jash_chain_tokenize_nul(const char* cmd, char* work_buf, size_t buf_cap, char** argv, int max_argv);

/* Jaccard similarity of two token sets (used by distill/builtin).
 * Requires NUL-terminated tokens (use jash_chain_tokenize_nul for this). */
double jash_chain_jaccard(char** a, int na, char** b, int nb);

#endif /* JASH_CORE_CHAIN_READ_H */
