// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DIGEST_NATIVE_H
#define JASH_CORE_DIGEST_NATIVE_H
#include "digest_engine.h"
#include <stdint.h>
#include <stddef.h>

/* Caller-owned observations of the existing inventory engine. Preparation
 * performs reads only. The caller validates and admits its receipt before
 * apply can publish a cache entry, content objects or a ledger record. */
typedef struct digest_snapshot digest_snapshot_t;
typedef struct {
    const char *path, *kind, *language, *change;
    uint64_t bytes, lines;
    char blake3[65], old_blake3[65];
    int directory;
} digest_native_entry_t;
typedef struct {
    const char *root, *level, *readme_path, *readme;
    size_t readme_bytes, entries;
    uint64_t visited, files, directories, bytes, lines;
    uint64_t excluded_files, excluded_special, excluded_directories;
    uint64_t categories[7], languages[11];
    char fingerprint[65], base_fingerprint[65], cache_key[65];
    int cache_requested, cache_hit, effects_started, cache_published, cache_durable;
    int cas_attempted, cas_complete, ledger_written, ledger_durable;
    uint64_t cas_verified_objects;
    const char* error;
} digest_native_observation_t;
int digest_snapshot_prepare(const digest_cfg_t*, digest_snapshot_t**);
const digest_native_observation_t* digest_snapshot_observation(const digest_snapshot_t*);
int digest_snapshot_entry(const digest_snapshot_t*, size_t, digest_native_entry_t*);
int digest_snapshot_apply(digest_snapshot_t*);
void digest_snapshot_close(digest_snapshot_t*);
/* Stable private store reads. Caller frees returned bytes. No rendering. */
int digest_native_ledger(char**, size_t*, int* present);
int digest_native_fetch(const char* hash, char**, size_t*);
typedef struct { uint64_t entries, bytes, oldest, newest, visited; int present; } digest_native_cache_t;
int digest_native_cache_status(digest_native_cache_t*);
const char* digest_native_ignored_directory(size_t);
#endif
