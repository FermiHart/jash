// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_qa_battery.h — NSA-Level Test Battery System for Jash
// 10 batteries, 500+ tests, zero tolerance.

#ifndef JASH_QA_BATTERY_H
#define JASH_QA_BATTERY_H

#include <core/jash_qa.h>

// Battery IDs
typedef enum {
    BAT_CORE      = 0,   // Core shell functionality
    BAT_SECURITY  = 1,   // Injection, escapes, sandbox
    BAT_PERF      = 2,   // Stress, memory, timing
    BAT_EDGE      = 3,   // Unicode, special chars, boundaries
    BAT_INTEGRATE = 4,   // Pipes, redirects, subshells
    BAT_AI        = 5,   // AI mode, Groq API
    BAT_JOBS      = 6,   // Job control, signals, fg/bg
    BAT_BUILTINS  = 7,   // All builtin commands
    BAT_PARSER    = 8,   // Complex parsing, grammar
    BAT_CONCUR    = 9,   // Race conditions, fd leaks
    BAT_COUNT     = 10
} battery_id_t;

// Run a specific battery (0-9). Returns pass count.
int jash_battery_run(battery_id_t bat, jash_writer_t w, void *ctx, const char *jash_bin);

// Run all 10 batteries. Returns total pass count.
int jash_battery_run_all(jash_writer_t w, void *ctx, const char *jash_bin);

// Get battery name
const char* jash_battery_name(battery_id_t bat);

#endif // JASH_QA_BATTERY_H
