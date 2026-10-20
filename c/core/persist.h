// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/persist — persistent shell state (CWD, env, aliases)
//
// Saves and restores shell state to ~/.jash_state so that it persists
// across sessions. History is handled separately by core/history.

#ifndef JASH_CORE_PERSIST_H
#define JASH_CORE_PERSIST_H

#include <core/state.h>

#define JASH_PERSIST_MAX_BYTES (16u * 1024u * 1024u)
typedef struct {
    int applied;       /* publication or at least one live state update occurred */
    int durable;       /* successful save/clear also fsynced its parent directory */
    int cwd_applied;
    int error_number;
    const char* error; /* stable non-sensitive code; NULL on success */
    uint64_t env_entries;
    uint64_t alias_entries;
} jash_persist_result_t;

int jash_persist_path(char* buffer, size_t capacity);
int jash_persist_save_checked(jash_state_t*, jash_persist_result_t*);
int jash_persist_load_checked(jash_state_t*, jash_persist_result_t*);
int jash_persist_clear_checked(jash_state_t*, jash_persist_result_t*);

/* Save shell state to ~/.jash_state */
void jash_persist_save(jash_state_t* s);

/* Load shell state from ~/.jash_state */
void jash_persist_load(jash_state_t* s);

/* Clear persisted state (unlink ~/.jash_state) */
void jash_persist_clear(jash_state_t* s);

/* ── F1: session-scoped persistence (JASH_SESSION) ─────────────────────────
 * Keyed by the JASH_SESSION id, these restore cwd+env+aliases even in -c
 * (non-interactive) mode, so an agent's separate `jash -c` calls share one
 * logical shell. No-ops when JASH_SESSION is unset/empty. */
void jash_session_load(jash_state_t* s);
void jash_session_save(jash_state_t* s);

/* Build the session state path from JASH_SESSION. Returns 1 on success
 * (buf filled), 0 if JASH_SESSION is unset/empty/invalid. */
int jash_session_path(char* buf, unsigned long cap);

#endif /* JASH_CORE_PERSIST_H */
