// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/digest_engine — project briefing generator for agents.
//
// Produces cached, BLAKE3-tagged project digests so an LLM agent can start
// from a briefing instead of exploring the codebase file by file.
//
// See Docs/ADR-0028-jash-self-digest.md

#ifndef JASH_DIGEST_ENGINE_H
#define JASH_DIGEST_ENGINE_H

#include <theme/io.h>

typedef enum {
    DIGEST_BRIEF    = 0,
    DIGEST_STANDARD = 1,
    DIGEST_DEEP     = 2
} digest_level_t;

/* Project inventory contract (generator version changes invalidate all caches).
 * Recognized regular source/header/test, documentation, build and configuration
 * files are content-hashed; opaque/unrecognized files (e.g. executables, model
 * weights, archives) are excluded regardless of size. Never size-skip an included
 * file. Source suffixes: .c/.h/.cpp/.cc/.cxx/.hpp/.hh/.hxx, .py/.pyi,
 * .js/.jsx/.mjs/.cjs, .ts/.tsx/.mts/.cts, .sh/.bash/.zsh, .rs,
 * .html/.htm/.css/.scss/.sass/.less/.vue/.svelte/.astro/.svg and extensionless
 * shebang scripts. Docs: .md/.rst/.txt/.adoc
 * and README/LICENSE/COPYING/CHANGELOG. Config: .json/.toml/.yaml/.yml/.ini/.cfg/
 * .conf/.xml/.lock/.properties/.env, .env.*, and common dotfile configuration
 * names. Build-manifest names/suffixes are enumerated in digest_classify().
 * Sorted, length-framed relative paths and directory entries include empty dirs:
 * additions, deletions and renames invalidate; timestamps alone do not.
 * Canonical absolute root + version + inventory form the public fingerprint;
 * the cache key additionally includes level. Explicit root aliases canonicalize.
 * Nested symlinks and non-regular files are excluded and never followed/opened.
 * Ignored directory basenames (at any depth): .git, .jash, build, build-speed,
 * node_modules, .cache, target, .venv, venv, __pycache__, oci-export, .Trash,
 * dist, Quarentena-codes, .DS_Store, .idea, .vscode, .pytest_cache, .mypy_cache,
 * .ruff_cache. A directory with its own regular CMakeCache.txt is also omitted,
 * whatever its name; symlink markers and build-* names alone are insufficient.
 * These are explicit rules, NOT .gitignore interpretation; regular
 * files with those names are included. Excluded content does not invalidate.
 * Bounds are global; directory entries include excluded names, except . and .. .
 * Content hashing runs on an explicit worker budget (jobs: 0 = auto, clamped
 * to online CPUs with a ceiling of 8; 1..32 explicit); the walk thread owns
 * traversal, staging and identity checks, workers only stream already-open
 * fds, and inventory bytes, rendered output, fingerprints and error selection
 * are identical for every budget.
 * Diff mode (diff_base) compares the live inventory against a prior
 * --manifest file and renders a jash.project-diff.v1 observation with
 * added/modified/removed entries sorted by path; kind changes surface as a
 * removed+added pair, and a base generator mismatch or corrupt entry fails
 * visibly. Diff mode never reads or writes the briefing cache.
 * Served briefings (cache hit or fresh render on the normal path) append one
 * jash.digest-receipt.v1 line to ledger.jsonl: root, fingerprint, level,
 * served_from_cache, bytes and counts. Pure modes (fingerprint, manifest,
 * diff, no-cache) never write receipts; telemetry is best-effort and never
 * fails the briefing it describes. Cached-briefing renders also stage read
 * content into cas/aa/bb... keyed by BLAKE3 (shared across projects; capped
 * at 512 MiB per snapshot, skipped past quota); `fetch` serves it back
 * byte-identically.
 * Any bound, allocation, read, path or observed concurrent-change error fails
 * visibly before output/cache publication. This is a best-effort scan, not a
 * filesystem snapshot: changes after an entry was read can await the next call.
 */
#define DIGEST_GENERATOR_VERSION "project-v5"
#define DIGEST_MAX_DEPTH 32u          /* root = depth 0 */
#define DIGEST_MAX_ENTRIES 32768u
#define DIGEST_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define DIGEST_MAX_TOTAL_BYTES (128u * 1024u * 1024u)
#define DIGEST_MAX_OUTPUT_BYTES (8u * 1024u * 1024u)
#define DIGEST_MAX_PATH 4096u         /* including terminating NUL */
#define DIGEST_MAX_JOBS 32            /* explicit hashing worker budget ceiling */

typedef struct {
    digest_level_t level;
    int            refresh;      /* force regeneration */
    const char*    output_path;  /* NULL = stdout */
    const char*    root_dir;     /* project root */
    int            no_cache;     /* no cache reads, mkdirs, tempfiles or writes */
    int            fingerprint;  /* plain 64 lowercase hex + LF; implies no_cache */
    int            manifest;     /* versioned JSON observation; implies no_cache */
    int            jobs;         /* hashing workers: 0 = auto (<=8 online CPUs), else 1..DIGEST_MAX_JOBS */
    const char*    diff_base;    /* NULL = off; else path to a --manifest file to diff against */
    const char*    fetch_hash;   /* NULL = off; else 64-hex content hash to serve from the store */
} digest_cfg_t;

/* Session digest config — generates a briefing of the live session state. */
typedef enum {
    SESH_BRIEF    = 0,   /* last 10 commands + git status + errors (~500 tokens) */
    SESH_STANDARD = 1,   /* last 25 + diff + timeline + tags (~1500 tokens)     */
    SESH_DEEP     = 2    /* full session (~4000 tokens)                         */
} digest_session_level_t;

typedef struct {
    digest_session_level_t level;
    int                    last_n;      /* max commands to include (0 = auto) */
    int                    include_git; /* 1 = include git status */
    const char*            output_path; /* NULL = stdout */
} digest_session_cfg_t;

/* Print the last `count` (1..1000) savings receipts as JSONL, oldest first.
 * An absent ledger is empty output, not an error. `output_path` behaves like
 * the project mode's (NULL = writer). Returns 0 on success. */
int digest_ledger_print(long count, const char* output_path,
                        jash_writer_t w, void* ctx);

/* Print a jash.digest-cache-status.v1 JSON summary of the briefing cache
 * (entry count, bytes, oldest/newest mtime; zeros when absent). Read-only:
 * never creates directories. Returns 0 on success. */
int digest_cache_status_print(const char* output_path,
                              jash_writer_t w, void* ctx);

/* Serve exact bytes for a 64-hex lowercase content hash from the
 * content-addressed store (populated on cached-briefing renders; shared
 * across projects by content). Unknown hash, unsafe object and oversize
 * object fail visibly. Read-only query. Returns 0 on success. */
int digest_fetch_print(const char* hash, const char* output_path,
                       jash_writer_t w, void* ctx);

/* Generate or load-cached digest and write it via `w`.
 * Without output_path, no_cache/fingerprint perform only filesystem reads and
 * writer calls (no disk spooling); output_path explicitly opts into that write.
 * This does not disable the calling shell's independent recording/telemetry.
 * Normal cache: $HOME/.jash/digest/<version-root-level-inventory-key>.md, private
 * owner-checked dirfds, no symlinks in cache paths, exclusive temporary files,
 * fsync + atomic rename; corrupt/unsafe/unavailable cache is a visible error.
 * Categories are exclusive: build manifests, headers (.h/.hpp/.hh/.hxx/.d.ts),
 * code tests (test/tests/__tests__ dirs, test_* / *_test / *.test / *.spec stems),
 * other sources, docs, configuration. Shell scripts are not automatically tests.
 * Lines (LF plus final unterminated line) and bytes cover included files only.
 * README/path text preserves valid UTF-8; invalid/control bytes are visibly
 * escaped. Brief/standard lists and README excerpts explicitly state their caps.
 * Returns 0 on success, non-zero with stderr diagnostic on error. */
int digest_run(const digest_cfg_t* cfg, jash_writer_t w, void* ctx);

/* Generate a session digest — summaries of recent commands, state, and timeline.
 * Returns 0 on success, non-zero on error. */
int digest_session_run(const digest_session_cfg_t* cfg, jash_writer_t w, void* ctx);

#endif /* JASH_DIGEST_ENGINE_H */
