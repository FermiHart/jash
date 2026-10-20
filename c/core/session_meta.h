// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — session_meta — LLM-friendly command metadata log
//
// Writes one JSON line per command to ~/.jash/session_meta.jsonl.
// Format: {"ts":UNIX,"exit":N,"dur_ms":N,"cmd":"..."}
//
// The file is append-only and never rotated within a session.
// LLMs read it with: cat ~/.jash/session_meta.jsonl | tail -20

#ifndef JASH_CORE_SESSION_META_H
#define JASH_CORE_SESSION_META_H

#include <stdint.h>

// Record one command. Lazy-inits on first call (reads HOME from env).
void jash_meta_record(const char* cmd, int exit_code, uint64_t dur_ms);

// Return the path being written to (empty string if not yet initialized).
const char* jash_meta_path(void);

#endif /* JASH_CORE_SESSION_META_H */
