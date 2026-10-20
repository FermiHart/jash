// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_mcache.h — Memory Cache: RNN-style hidden state with checkpointing
// Applies Memory Caching (arXiv:2602.24281) to shell session context:
//   hidden state grows with session length via checkpoints at decision boundaries.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_MCACHE_H
#define JASH_CORE_MCACHE_H

#include <stdint.h>
#include <stddef.h>

// ── Constants ────────────────────────────────────────────────────────────────

#define MCACHE_STATE_BYTES      256      // hidden state vector size
#define MCACHE_MAX_CHECKPOINTS  128      // ring buffer of checkpoints
#define MCACHE_LABEL_MAX        48       // checkpoint label length
#define MCACHE_PATH_MAX         512      // directory path length
#define MCACHE_HEAT_INIT        100      // initial heat (0-100)
#define MCACHE_HEAT_DECAY_MS    60000    // heat decays 1 point per 60s idle

// ── Checkpoint ───────────────────────────────────────────────────────────────

typedef struct {
    uint64_t seq;                        // sigil chain seq at capture
    uint64_t timestamp_ns;               // CLOCK_MONOTONIC at capture
    char     label[MCACHE_LABEL_MAX];    // origin label (cmd, decision, event)
    uint8_t  state_hash[32];             // BLAKE3 of hidden state at checkpoint
    int      rc;                         // exit code that triggered this
    uint32_t heat;                       // 0-100, decays over time
    uint32_t token_estimate;             // approx tokens to replay here
} jash_mcache_cp_t;

// ── Hidden State ─────────────────────────────────────────────────────────────

typedef struct {
    uint8_t  data[MCACHE_STATE_BYTES];   // derived feature vector
    uint32_t len;                        // actual used bytes (<= STATE_BYTES)
    uint64_t version;                    // monotonic state version
} jash_mcache_state_t;

// ── Memory Cache ─────────────────────────────────────────────────────────────

typedef struct {
    jash_mcache_state_t  hidden;         // current hidden state
    jash_mcache_cp_t     cps[MCACHE_MAX_CHECKPOINTS];  // checkpoint ring
    uint32_t             cp_head;        // next slot to write (ring)
    uint32_t             cp_count;       // valid entries (<= MAX_CHECKPOINTS)
    uint64_t             last_write_ns;  // last state update
    uint32_t             heat;           // 0-100, decays over idle
    int                  enabled;        // 0 = off, 1 = on

    // Stats
    uint64_t             total_captures; // lifetime checkpoints captured
    uint64_t             total_recalls;  // lifetime checkpoint recalls
    uint64_t             total_decays;   // lifetime decay events
} jash_mcache_t;

// ── Public API ───────────────────────────────────────────────────────────────

// Init with state dir (~/.jash/mcache/). Returns 0 on success.
int  jash_mcache_init(const char* state_dir);

// Cleanup and persist.
void jash_mcache_cleanup(void);

// Enable/disable.
void jash_mcache_enable(int enabled);
int  jash_mcache_is_enabled(void);

// ── State Management ─────────────────────────────────────────────────────────

// Derive hidden state from current shell context (CWD, last_cmd, rc, env hash).
// Called automatically after each command.
void jash_mcache_derive(jash_mcache_state_t* out, const char* cwd,
                        const char* last_cmd, int rc, uint32_t env_hash);

// Update hidden with derived state + decay heat.
void jash_mcache_update(const jash_mcache_state_t* state);

// Get current hidden state (read-only).
const jash_mcache_state_t* jash_mcache_current(void);

// ── Checkpoints ──────────────────────────────────────────────────────────────

// Capture a checkpoint. label is a short description (e.g. "build:ok", "test:fail").
// Returns checkpoint index or -1 on failure.
int  jash_mcache_capture(const char* label, int rc, uint64_t sigil_seq,
                         uint32_t token_estimate);

// Find nearest checkpoint by sequence number. Returns index or -1.
int  jash_mcache_find_by_seq(uint64_t seq);

// Find best checkpoint for a given state (by state_hash prefix match).
// Returns index of best checkpoint or -1.
int  jash_mcache_find_best(const uint8_t* state_hash);

// Recall a checkpoint: returns the checkpoint at index. Returns NULL on invalid.
const jash_mcache_cp_t* jash_mcache_recall(int index);

// ── Heat / Decay ─────────────────────────────────────────────────────────────

// Tick heat decay — call this periodically (every command is fine).
void jash_mcache_tick(uint64_t now_ns);

// Get current cache heat (0-100).
uint32_t jash_mcache_heat(void);

// ── Persistence ──────────────────────────────────────────────────────────────

// Save state to ~/.jash/mcache/mcache.state (binary).
int  jash_mcache_save(void);

// Load state from disk.
int  jash_mcache_load(void);

// ── Integration ──────────────────────────────────────────────────────────────

// Hook: call after every command. Integrates with sigil + timeline.
void jash_mcache_post_cmd(const char* cmd, int rc, uint64_t sigil_seq,
                          uint64_t now_ns);

// Get context for AI prompt: writes compact summary of current memory state.
// Returns bytes written.
int  jash_mcache_context(char* buf, size_t cap);

// ── Stats ────────────────────────────────────────────────────────────────────

const jash_mcache_t* jash_mcache_stats(void);

#endif // JASH_CORE_MCACHE_H
