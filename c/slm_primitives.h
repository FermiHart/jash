// SPDX-License-Identifier: LicenseRef-SPL-1.0
// slm-primitives.h — SLM Neural Microkernel Primitives
// Part of Jash Cognitive OS (ADR-0022)
//
// These are the cognitive primitives that form the neural microkernel.
// Each primitive is a lightweight SLM (~500M-3B params) that runs locally.
// Zero impact on main binary — loaded only via `module load slm-primitives`.

#ifndef SLM_PRIMITIVES_H
#define SLM_PRIMITIVES_H

#include <stdint.h>
#include <stddef.h>

// ── Confidence Score ────────────────────────────────────────────────────────
typedef struct {
    float score;        // 0.0 - 1.0
    uint8_t calibrated; // 1 if calibrated via conformal prediction
    float lower;        // lower bound of confidence interval
    float upper;        // upper bound of confidence interval
} slm_confidence_t;

// ── Developer Intent States ────────────────────────────────────────────────
typedef enum {
    SLM_INTENT_EXPLORING,   // browsing code, reading files
    SLM_INTENT_FOCUSING,    // deep work, single file
    SLM_INTENT_DEBUGGING,   // error analysis, breakpoints
    SLM_INTENT_REFACTORING, // restructuring code
    SLM_INTENT_TESTING,     // running tests, writing tests
    SLM_INTENT_DEPLOYING,   // build, push, deploy
    SLM_INTENT_UNKNOWN      // cannot determine
} slm_intent_state_t;

// ── Causal Effect ──────────────────────────────────────────────────────────
typedef struct {
    char* affected_path;    // file/resource affected
    float impact_score;     // 0.0 - 1.0
    char* description;      // human-readable description
    uint8_t will_break;     // 1 if will cause failure
} slm_causal_effect_t;

// ── Causal Prediction ──────────────────────────────────────────────────────
typedef struct {
    slm_causal_effect_t* effects;
    size_t effect_count;
    float total_risk;       // 0.0 - 1.0
    slm_confidence_t confidence;
} slm_causal_prediction_t;

// ── Memory Entry ───────────────────────────────────────────────────────────
typedef struct {
    char* key;              // semantic key (e.g., "bug do cache")
    char* value;            // associated value (e.g., "race condition in cache.c:142")
    float relevance;        // 0.0 - 1.0
    uint64_t timestamp;     // when this was stored
    uint32_t access_count;  // how often accessed
} slm_memory_entry_t;

// ── Memory Query Result ────────────────────────────────────────────────────
typedef struct {
    slm_memory_entry_t* entries;
    size_t count;
    slm_confidence_t confidence;
} slm_memory_result_t;

// ── Autocorrect Suggestion ─────────────────────────────────────────────────
typedef struct {
    char* original;         // original command
    char* corrected;        // suggested correction
    char* reason;           // why this correction
    float confidence;       // 0.0 - 1.0
    uint8_t auto_apply;     // 1 if should auto-apply (high confidence)
} slm_autocorrect_t;

// ── Perceptron de Intenção ─────────────────────────────────────────────────
// Detects developer's mental state from command patterns
typedef struct {
    slm_intent_state_t state;
    slm_confidence_t confidence;
    char* description;      // human-readable state description
} slm_perceptron_result_t;

// ── Simulador Causal ───────────────────────────────────────────────────────
// Predicts effects of a command before execution
typedef struct {
    char* command;
    slm_causal_prediction_t prediction;
    char* suggested_alternative; // safer alternative (NULL if none needed)
} slm_simulation_result_t;

// ── Microagent ─────────────────────────────────────────────────────────────
typedef enum {
    SLM_AGENT_VALIDATOR,    // validates code quality
    SLM_AGENT_DOCUMENTER,   // generates documentation
    SLM_AGENT_TESTER,       // runs/creates tests
    SLM_AGENT_REVIEWER,     // code review
    SLM_AGENT_SECURITY      // security analysis
} slm_agent_type_t;

typedef struct {
    slm_agent_type_t type;
    char* task;
    char* result;
    slm_confidence_t confidence;
    uint8_t completed;
} slm_microagent_t;

// ── Core API ───────────────────────────────────────────────────────────────

// Perceptron de Intenção
slm_perceptron_result_t slm_perceive_intent(const char* recent_commands, size_t count);

// Simulador Causal
slm_simulation_result_t slm_simulate(const char* command, const char* project_root);

// Orquestrador de Microagentes
slm_microagent_t slm_spawn_agent(slm_agent_type_t type, const char* task);
void slm_despawn_agent(slm_microagent_t* agent);

// Memória Associativa
slm_memory_result_t slm_memory_query(const char* query, size_t max_results);
int slm_memory_store(const char* key, const char* value);

// Autocorreção
slm_autocorrect_t slm_autocorrect(const char* command);

// Confidence calibration
slm_confidence_t slm_calibrate_confidence(float raw_score, const char* task_type);

// Utility
const char* slm_intent_name(slm_intent_state_t state);
const char* slm_agent_name(slm_agent_type_t type);
void slm_free_prediction(slm_causal_prediction_t* pred);
void slm_free_memory_result(slm_memory_result_t* result);
void slm_free_autocorrect(slm_autocorrect_t* ac);

#endif // SLM_PRIMITIVES_H
