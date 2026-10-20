// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Smart Gate System header

#ifndef JASH_SMART_GATE_H
#define JASH_SMART_GATE_H

#include <stdbool.h>
#include <stdint.h>

// Initialize smart gate (call at shell startup)
void smart_gate_init(void);

// Mark shell as ready (call after all initialization complete)
void smart_gate_mark_ready(void);

// Check if shell is ready
bool smart_gate_is_ready(void);

// Enqueue a command typed before shell was ready
int smart_gate_enqueue(const char* cmd);

// Dequeue next command
char* smart_gate_dequeue(void);

// Wait for ready and execute all queued commands
// execute_fn: function that executes a command string
int smart_gate_wait_and_execute(int (*execute_fn)(const char*));

// Disable smart gate (for testing)
void smart_gate_disable(void);

// Enable smart gate
void smart_gate_enable(void);

// Get statistics
void smart_gate_stats(uint64_t* elapsed_us, int* queued_commands);

#endif // JASH_SMART_GATE_H
