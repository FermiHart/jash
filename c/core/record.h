// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — record — session recording for replay.
//
// Records every command + timestamp to ~/.jash_sessions/.
// Can replay sessions to debug issues or share workflows.
// Fish has nothing like this.

#ifndef JASH_CORE_RECORD_H
#define JASH_CORE_RECORD_H

#include <core/state.h>

/* Initialize session recording (call at shell start) */
void jash_record_init(jash_state_t* s);

/* Record a command execution */
void jash_record_command(jash_state_t* s, const char* cmd,
                         int exit_code, uint64_t duration_ns);

/* Get session file path for current session */
const char* jash_record_session_path(void);

/* List recorded sessions (writes paths to out, returns count) */
int jash_record_list_sessions(char out[][256], int max);

/* Replay a session file (calls callback for each command) */
typedef void (*jash_record_replay_cb)(const char* cmd, int exit_code,
                                       uint64_t duration_ns, void* ctx);
int jash_record_replay(const char* path, jash_record_replay_cb cb, void* ctx);

/* Get session stats */
typedef struct {
    int total_commands;
    int failed_commands;
    uint64_t total_duration_ns;
    uint64_t longest_command_ns;
    char longest_command[256];
} jash_record_stats_t;

int jash_record_stats(const char* path, jash_record_stats_t* stats);

#endif /* JASH_CORE_RECORD_H */
