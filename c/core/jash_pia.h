// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_pia.h — Protocol of Intent Atoms: context compression with cross-message interning
// Port of BearCode's PIA v3 — prefix-stable cross-message interning + delta-dedup.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_PIA_H
#define JASH_CORE_PIA_H

#include <stdint.h>
#include <stddef.h>

// ── Constants ────────────────────────────────────────────────────────────────

#define PIA_MAX_INTERN_TABLE  256      // max interned strings per session
#define PIA_INTERN_KEY_MAX     32      // "§N" or "§hex..."
#define PIA_INTERN_VAL_MAX   1024      // original string
#define PIA_CHUNK_MAX        4096      // max chunk size for compression

// ── Interning ────────────────────────────────────────────────────────────────

typedef struct {
    char key[PIA_INTERN_KEY_MAX];      // "§1", "§2", ...
    char val[PIA_INTERN_VAL_MAX];      // original string
    uint32_t ref_count;                // how many times referenced
    uint32_t saved_bytes;              // bytes saved by referencing
} pia_intern_entry_t;

// ── PIA Session ──────────────────────────────────────────────────────────────

typedef struct {
    pia_intern_entry_t table[PIA_MAX_INTERN_TABLE];
    uint32_t count;                    // entries in table
    uint32_t next_id;                  // next "§N" number
    uint64_t total_saved_bytes;        // cumulative savings
    uint64_t total_input_bytes;        // cumulative input
    uint32_t msg_counter;              // message sequence number
} pia_session_t;

// ── Public API ───────────────────────────────────────────────────────────────

// Initialize PIA session (call once per shell session).
void pia_init(pia_session_t* pia);

// Reset session (start fresh).
void pia_reset(pia_session_t* pia);

// ── Compression ──────────────────────────────────────────────────────────────

// Compress a message using cross-message interning.
// Input: raw text. Output: compressed text (written to out, up to cap bytes).
// Returns bytes written to out.
int  pia_compress(pia_session_t* pia, const char* input, char* out, size_t cap);

// Decompress a previously compressed string.
// Returns bytes written to out, or -1 on invalid ref.
int  pia_decompress(pia_session_t* pia, const char* compressed, char* out, size_t cap);

// ── Delta-Dedup ──────────────────────────────────────────────────────────────

// Register a tool output (e.g., hex digest). Returns the key ("§hex...") if
// this output has been seen before, or NULL if it's new.
// If compress is non-NULL, the output is replaced with the reference.
const char* pia_delta_dedup(pia_session_t* pia, const char* tool_output,
                            char* compressed, size_t compressed_cap);

// ── Stats ───────────────────────────────────────────────────────────────────

// Get compression ratio so far.
float pia_ratio(pia_session_t* pia);

// Get total bytes saved.
uint64_t pia_saved(pia_session_t* pia);

// ── Prefix Stability (for prompt caching) ────────────────────────────────────

// Compress incrementally: add a message to the rolling window.
// Guarantees: compress(msgs[:k]) == compress(msgs)[:k].
// Returns bytes written to out.
int  pia_compress_next(pia_session_t* pia, const char* next_msg, char* out, size_t cap);

#endif // JASH_CORE_PIA_H
