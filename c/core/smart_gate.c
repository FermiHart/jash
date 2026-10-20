// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Smart Gate System: bloqueia Enter até shell carregar completamente
// Se usuário digita rápido demais, queue de comandos é executada em ordem

#include "../builtins/builtin.h"
#include "smart_gate.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_QUEUE_SIZE 256
#define MAX_CMD_LEN 4096
#define GATE_TIMEOUT_US 100000  // 100ms max wait

typedef struct {
    char* commands[MAX_QUEUE_SIZE];
    int count;
    int head;
    int tail;
} command_queue_t;

typedef struct {
    bool ready;
    uint64_t start_time_us;
    command_queue_t queue;
    bool gate_enabled;
} smart_gate_t;

static smart_gate_t g_gate = {0};

// Get current time in microseconds
static uint64_t get_time_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Initialize smart gate
void smart_gate_init(void) {
    g_gate.ready = false;
    g_gate.start_time_us = get_time_us();
    g_gate.queue.count = 0;
    g_gate.queue.head = 0;
    g_gate.queue.tail = 0;
    g_gate.gate_enabled = true;
}

// Mark shell as ready
void smart_gate_mark_ready(void) {
    g_gate.ready = true;
    uint64_t elapsed = get_time_us() - g_gate.start_time_us;
    
    if (elapsed > 1000) {  // Only log if took > 1ms
        fprintf(stderr, "\033[33m[Smart Gate] Shell ready after %luμs\033[0m\n", 
                (unsigned long)elapsed);
    }
}

// Check if shell is ready
bool smart_gate_is_ready(void) {
    return g_gate.ready;
}

// Enqueue a command
int smart_gate_enqueue(const char* cmd) {
    if (g_gate.queue.count >= MAX_QUEUE_SIZE) {
        fprintf(stderr, "\033[31m[Smart Gate] Queue full, dropping command\033[0m\n");
        return -1;
    }
    
    g_gate.queue.commands[g_gate.queue.tail] = strdup(cmd);
    g_gate.queue.tail = (g_gate.queue.tail + 1) % MAX_QUEUE_SIZE;
    g_gate.queue.count++;
    
    return 0;
}

// Dequeue a command
char* smart_gate_dequeue(void) {
    if (g_gate.queue.count == 0) {
        return NULL;
    }
    
    char* cmd = g_gate.queue.commands[g_gate.queue.head];
    g_gate.queue.head = (g_gate.queue.head + 1) % MAX_QUEUE_SIZE;
    g_gate.queue.count--;
    
    return cmd;
}

// Wait for shell to be ready, processing queued commands
int smart_gate_wait_and_execute(int (*execute_fn)(const char*)) {
    if (!g_gate.gate_enabled || g_gate.ready) {
        return 0;
    }
    
    // Wait for ready with timeout
    uint64_t start = get_time_us();
    while (!g_gate.ready) {
        uint64_t now = get_time_us();
        if (!start || !now || now < start || now - start > GATE_TIMEOUT_US) {
            fprintf(stderr, "\033[31m[Smart Gate] Timeout waiting for shell\033[0m\n");
            g_gate.ready = true;  // Force ready
            break;
        }
        usleep(1000);  // 1ms
    }
    
    // Execute all queued commands
    char* cmd;
    while ((cmd = smart_gate_dequeue()) != NULL) {
        if (execute_fn) {
            execute_fn(cmd);
        }
        free(cmd);
    }
    
    return 0;
}

// Disable smart gate (for testing)
void smart_gate_disable(void) {
    g_gate.gate_enabled = false;
    g_gate.ready = true;
}

// Enable smart gate
void smart_gate_enable(void) {
    g_gate.gate_enabled = true;
}

// Get gate statistics
void smart_gate_stats(uint64_t* elapsed_us, int* queued_commands) {
    if (elapsed_us) {
        *elapsed_us = get_time_us() - g_gate.start_time_us;
    }
    if (queued_commands) {
        *queued_commands = g_gate.queue.count;
    }
}
