// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/token_tracker — real token counting for sessions (ADR-0034)
// F E R M I ∞ H A R T

#ifndef JASH_TOKEN_TRACKER_H
#define JASH_TOKEN_TRACKER_H

#include <stddef.h>
#include <theme/io.h>

// Token estimation: ~1 token per 4 characters (conservative for code)
#define JASH_CHARS_PER_TOKEN 4

// Session token data
typedef struct {
    long input_tokens;   // tokens from -c command strings
    long output_tokens;  // tokens from command output (bytes shown)
    long total_commands; // number of -c commands executed
    long total_ms;       // total execution time in ms
} jash_session_tokens_t;

// Record token usage for a -c command execution
// cmd_tokens: estimated tokens from the command string
// output_bytes: actual output bytes shown to consumer
// exec_ms: execution time in milliseconds
void jash_token_record(long cmd_tokens, long output_bytes, double exec_ms);

// Writer wrapper that counts bytes passing through
typedef struct {
    jash_writer_t real_w;
    void*         real_ctx;
    long          bytes_counted;
} jash_token_wrap_t;

// Initialize a token-counting writer wrapper
void jash_token_wrap_init(jash_token_wrap_t* tw, jash_writer_t w, void* ctx);

// Get the writer function for the wrapper
jash_writer_t jash_token_wrap_writer(jash_token_wrap_t* tw);

// Get total bytes counted by this wrapper
long jash_token_wrap_bytes(jash_token_wrap_t* tw);

// Get cumulative session token totals
// Reads from ~/.jash/token_usage.session
int jash_session_tokens(jash_session_tokens_t* out);
/* Bounded exact counter read, without creating ~/.jash or changing process
 * initialization. Missing store is zero counters with *exists=0. */
int jash_session_tokens_readonly(jash_session_tokens_t* out, int* exists);

// Reset session token counters
void jash_session_tokens_reset(void);

// Initialize session token file path (call once at startup)
void jash_token_init(void);

#endif // JASH_TOKEN_TRACKER_H
