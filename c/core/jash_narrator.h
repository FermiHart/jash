// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_narrator.h — Zero-LLM Narration Engine
// Human-readable feedback for tool operations without LLM calls.
// Port of BearCode's bear_narrator.py with anti-repeat ring.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_NARRATOR_H
#define JASH_CORE_NARRATOR_H

#include <stdint.h>
#include <stddef.h>

// ── Narrator Modes ───────────────────────────────────────────────────────────

typedef enum {
    NARRATOR_SILENT = 0,     // no narration
    NARRATOR_MINIMAL,         // single line ("feito.")
    NARRATOR_NORMAL,          // one phrase per operation
    NARRATOR_VERBOSE,         // contextual narration
} jash_narrator_mode_t;

// ── Operation Kinds ──────────────────────────────────────────────────────────

typedef enum {
    NARR_OP_NONE = 0,
    NARR_OP_BUILD_OK,         // build succeeded
    NARR_OP_BUILD_FAIL,       // build failed
    NARR_OP_TEST_OK,          // tests passed
    NARR_OP_TEST_FAIL,        // tests failed
    NARR_OP_CREATE_FILE,      // file created
    NARR_OP_MODIFY_FILE,      // file modified
    NARR_OP_DELETE_FILE,      // file deleted
    NARR_OP_MOVE_FILE,        // file moved/renamed
    NARR_OP_GIT_COMMIT,       // git commit
    NARR_OP_GIT_PUSH,         // git push
    NARR_OP_GIT_PULL,         // git pull
    NARR_OP_INSTALL,          // package install
    NARR_OP_REMOVE,           // package remove
    NARR_OP_DEPLOY,           // deploy
    NARR_OP_SEARCH,           // search complete
    NARR_OP_LEARN,            // learned something new
    NARR_OP_RECALL,           // recalled from memory
    NARR_OP_APPROVE,          // user approved
    NARR_OP_REJECT,           // user rejected
    NARR_OP_TIMEOUT,          // operation timed out
    NARR_OP_ERROR,            // generic error
    NARR_OP_START,            // operation started
    NARR_OP_DONE,             // operation finished
    NARR_OP_WAIT,             // waiting for something
    NARR_OP_CONNECT,          // connected to service
    NARR_OP_DISCONNECT,       // disconnected
    NARR_OP_SYNC,             // synced
} jash_narrator_op_t;

// ── Public API ───────────────────────────────────────────────────────────────

// Initialize narrator.
void jash_narrator_init(void);

// Set narration mode.
void jash_narrator_set_mode(jash_narrator_mode_t mode);
jash_narrator_mode_t jash_narrator_get_mode(void);

// Narrate an operation. Writes phrase to buf (up to cap bytes).
// Returns number of bytes written (excluding NUL).
int  jash_narrate(jash_narrator_op_t op, const char* detail, char* buf, size_t cap);

// Quick helper: get a static phrase (no detail interpolation).
// Use when you just need a short string for the status bar.
const char* jash_narrator_phrase(jash_narrator_op_t op);

// Reset anti-repeat ring (e.g., on new AI query).
void jash_narrator_reset(void);

#endif // JASH_CORE_NARRATOR_H
