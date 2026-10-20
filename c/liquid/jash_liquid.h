// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_liquid.h — Closed-form Continuous-time (CfC) Liquid Reasoning Engine
// F E R M I ∞ H A R T
//
// A tiny, sovereign, pure-C recurrent network for the shell. It learns
// from every command (online SGD) and predicts next steps, intent classes,
// blast-radius risk, and anomaly confidence.
//
// Based on Hasani et al., "Closed-form Continuous-time Neural Models"
// (arXiv:2106.13898, Nature Machine Intelligence 2022).

#ifndef JASH_LIQUID_H
#define JASH_LIQUID_H

#include <stddef.h>
#include <stdint.h>
#include <theme/io.h>

// ── Model dimensions ─────────────────────────────────────────────────────────

#define JL_INPUT_DIM       24   // engineered features per command
#define JL_HIDDEN          32   // liquid neurons
#define JL_CMD_CLASSES     64   // next-command hash buckets
#define JL_INTENT_CLASSES   8   // intent category buckets
#define JL_CMD_EMBED        8   // learned embedding dim per command class
#define JL_MAX_PATH        512
#define JL_CMD_HISTORY     16   // recent commands kept for context
#define JL_CMD_LABEL_MAX   64   // max stored command label length

// ── Public state ─────────────────────────────────────────────────────────────

typedef struct {
    uint32_t version;
    uint32_t hidden_size;
    uint32_t cmd_classes;
    uint32_t intent_classes;
    uint64_t examples_seen;
    uint64_t last_saved_ns;
    float    avg_loss;
    float    accuracy;          // next-cmd top-1 accuracy (exponential moving)
} jash_liquid_stats_t;

// ── One learned/training example ─────────────────────────────────────────────

typedef struct {
    float    features[JL_INPUT_DIM];
    int      cmd_class;         // class of the current command (-1 if unavailable)
    int      next_cmd_class;    // -1 if unknown
    int      intent_class;      // -1 if unknown
    float    reward;            // +1 success, -1 failure, 0 neutral
    uint64_t timestamp_ns;
} jash_liquid_example_t;

// ── Prediction output ────────────────────────────────────────────────────────

typedef struct {
    int      top_cmd_class;
    float    cmd_confidence;    // softmax probability of top_cmd_class
    int      top_intent_class;
    float    intent_confidence;
    float    risk;              // 0..1 blast-radius / anomaly risk
    float    confidence;        // overall model confidence
    char     top_cmd_name[64];  // best-effort human label
    int      top_k_classes[3];  // top-3 next command classes
    float    top_k_scores[3];   // their probabilities
} jash_liquid_prediction_t;

// ── Command class histogram: maps a class to the most common real command ────

typedef struct {
    int  class_id;
    char label[JL_CMD_LABEL_MAX];
    uint32_t count;
} jash_liquid_cmd_hist_t;

// ── Intent class labels (indices match JL_INTENT_CLASSES) ────────────────────

extern const char* JASH_LIQUID_INTENT_NAMES[];

// ── Lifecycle ────────────────────────────────────────────────────────────────

int  jash_liquid_init(const char* data_dir);
void jash_liquid_cleanup(void);
int  jash_liquid_save(void);
int  jash_liquid_load(void);

// ── Feature extraction ───────────────────────────────────────────────────────

// Build a feature vector from a command execution record.
void jash_liquid_features(const char* cmd, int rc, uint64_t duration_ms,
                          uint32_t mutation_count, const char* cwd,
                          float out[JL_INPUT_DIM]);

// ── Learning / prediction ────────────────────────────────────────────────────

// Feed one example and perform one online SGD update. Safe to call after
// every executed command.
void jash_liquid_learn(const jash_liquid_example_t* ex);

// Predict from the current hidden state + features.
void jash_liquid_predict(const float features[JL_INPUT_DIM],
                         jash_liquid_prediction_t* out);

// Explicit reward/punish the last prediction (for reinforcement-style loops).
void jash_liquid_reward(float reward);

// ── Utility ──────────────────────────────────────────────────────────────────

// Hash a command string into a stable integer class id.
int jash_liquid_cmd_class(const char* cmd);

// Map free-form text to one of the intent classes.
int jash_liquid_intent_class(const char* text);

// Map a predicted class to the most common real command seen for that class.
const char* jash_liquid_class_label(int class_id);

// Register a real command label for a class (used for human-readable suggestions).
void jash_liquid_update_label(int class_id, const char* cmd);

// Feed a sequence of previous commands to prime the hidden state before
// predicting the next one. Each call advances the network one timestep.
void jash_liquid_prime(const char* cmd, int rc, uint64_t duration_ms,
                       uint32_t mutation_count, const char* cwd);

// Hook called after every top-level command. Learns prev->current transitions,
// updates the command histogram, and optionally emits guard warnings.
void jash_liquid_post_cmd(const char* cmd, int rc, uint64_t duration_ms,
                          uint32_t mutation_count, const char* cwd,
                          jash_writer_t w, void* ctx);

// Returns 1 if the last predicted command is anomalous relative to history.
int jash_liquid_guard_alert(const char* cmd, int rc, const char* cwd);

// Generate an autosuggestion for the current prompt prefix. Returns 1 if a
// high-confidence brain suggestion was written to out, 0 otherwise.
int jash_liquid_autosuggest(const char* prefix, const char* cwd,
                            char* out, size_t cap);

// Returns 1 if the automatic brain hook should bypass the fast path.
int jash_liquid_should_hook(void);

// Reset learned weights to small random values (sovereign forget).
void jash_liquid_reset(void);

// Diagnostics.
jash_liquid_stats_t jash_liquid_stats(void);
void jash_liquid_print_status(jash_writer_t w, void* ctx);

#endif // JASH_LIQUID_H
