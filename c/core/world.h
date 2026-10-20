// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/world — world-delta scanner (ADR-0042 F2).
//
// "The world changed while you were thinking." Between an agent's -c calls,
// other agents/processes/humans mutate the workspace. World-delta snapshots
// the cwd tree, diffs against the previous snapshot, and reports what changed
// (created/modified/deleted) with best-effort attribution to chain entries.
//
// On-disk: ~/.jash/world/<session_id>.scan — versioned hex-path TSV; legacy plain
// path TSV remains readable when unambiguous. Failed/capped snapshots are refused.
// Knobs: JASH_WORLD=1 (default on with JASH_SESSION), JASH_WORLD_DEPTH=3.
//
// F E R M I ∞ H A R T
#ifndef JASH_CORE_WORLD_H
#define JASH_CORE_WORLD_H

#include <stdint.h>
#include <theme/io.h>
#include <core/json_writer.h>

#define JASH_WORLD_MAX_ENTRIES 5000
#define JASH_WORLD_PATH_MAX    512

typedef struct {
    char path[JASH_WORLD_PATH_MAX];
    long mtime;
    long size;
    int  mode;
} jash_world_entry_t;

typedef struct {
    jash_world_entry_t* entries;
    int  count;
    int  capacity;
} jash_world_scan_t;

typedef struct {
    char created[8192];
    char modified[8192];
    char deleted[8192];
    char attribution[4096];
    int  n_created;
    int  n_modified;
    int  n_deleted;
    int  total_created;
    int  total_modified;
    int  total_deleted;
    uint32_t path_comparisons; /* deterministic complexity gate, not emitted */
    long since_s;
} jash_world_delta_t;

// Scan cwd recursively (depth-limited). Returns a new scan or NULL on failure.
jash_world_scan_t* jash_world_scan(const char* root, int max_depth);

// Diff two path-sorted scans. Fills delta; ownership remains with the caller.
void jash_world_diff(jash_world_scan_t* old_scan,
                     const jash_world_scan_t* new_scan,
                     jash_world_delta_t* delta);

// Store scan to disk (~/.jash/world/<sid>.scan).
void jash_world_store(const jash_world_scan_t* scan, const char* session_id);

// Load previous scan from disk, sorted by path. Returns NULL if none exists.
jash_world_scan_t* jash_world_load(const char* session_id);

// Free a scan.
void jash_world_scan_free(jash_world_scan_t* scan);

// Emit world_delta JSON fragment into buf. Returns bytes written, -1 if too small
// (buf is then empty). The streaming API avoids this compatibility buffer limit.
int jash_world_delta_json(const jash_world_delta_t* delta, char* buf, int buflen);
/* Emit the pre-encoded fragment produced by jash_world_diff, without a size
 * ceiling in the serializer. Collector limits are reported separately. */
void jash_world_delta_write(const jash_world_delta_t*, jash_json_writer_t*);

// Main integration: call at -c startup, returns delta (or NULL if no changes).
// Consumes session_id, cwd. Allocates delta — caller frees with free().
jash_world_delta_t* jash_world_check(const char* session_id, const char* cwd);

/* Main integration using a current scan produced by a shared tree walk.
 * Consumes current_scan regardless of whether a delta is returned. */
jash_world_delta_t* jash_world_check_with_scan(const char* session_id,
                                               const char* cwd,
                                               jash_world_scan_t* current_scan);

#endif /* JASH_CORE_WORLD_H */
