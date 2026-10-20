// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/digest — observed project briefing and separate session digest.

#include "builtin.h"
#include <core/argv.h>
#include <core/digest_engine.h>
#include <core/jash_invoice.h>
#include <theme/io.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long digest_file_writer(void* ctx, const char* buf, size_t n) {
    return (long)fwrite(buf, 1, n, (FILE*)ctx);
}

static int digest_arg_error(const char* message) {
    jash_writef(jash_stderr_writer, NULL, "digest: %s\n", message);
    return 2;
}

static void digest_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx,
        "digest — deterministic briefing from observed project files\n"
        "Usage:\n"
        "  digest [--brief|--standard|--deep] [--refresh] [--no-cache]\n"
        "         [--fingerprint|--manifest] [--output FILE] [--] [DIR]\n"
        "  digest --session [--brief|--standard|--deep] [--last N] [--no-git] [--output FILE]\n"
        "\n"
        "  --brief        counts, identity/README excerpt, up to 12 build paths\n"
        "  --standard     default; adds up to 40 paths per category\n"
        "  --deep         complete recognized inventory, within output bound\n"
        "  --refresh      regenerate and atomically replace this level's cache\n"
        "  --no-cache     bypass ALL cache reads, mkdirs, temporary files and writes;\n"
        "                 render in memory (takes precedence over --refresh)\n"
        "  --fingerprint  only 64 lowercase BLAKE3 hex + LF, from the same inventory;\n"
        "                 implies --no-cache; independent of presentation level\n"
        "  --manifest     versioned JSON inventory with per-file BLAKE3 and coverage;\n"
        "                 implies --no-cache; cannot combine with --fingerprint\n"
        "  --output FILE  explicitly write FILE instead of stdout, even with --no-cache\n"
        "  --jobs N      content-hashing workers, integer 1..32 (default: min(online\n"
        "                CPUs, 8)); output and fingerprints are identical for any N\n"
        "  --diff-base FILE  compare the live inventory against a prior --manifest\n"
        "                file; emits jash.project-diff.v1 JSON (added/modified/\n"
        "                removed, kind changes as removed+added); fixed format, no\n"
        "                level; never touches the briefing cache\n"
        "  --ledger [N]  print the last N savings receipts as JSONL (default 20,\n"
        "                1..1000); absent ledger prints nothing; read-only query\n"
        "  --cache-status  print jash.digest-cache-status.v1 JSON for the briefing\n"
        "                cache (entries, bytes, oldest/newest); read-only query\n"
        "  --fetch HASH  serve exact bytes for a 64-hex content hash from the\n"
        "                content store (populated on cached renders, shared\n"
        "                across projects); unknown hash fails visibly\n"
        "  --session      separate existing session digest; no project DIR/options\n"
        "  --last N       session only, integer 1..10000 (default chosen by level)\n"
        "  --no-git       session only, omit Git Status section\n"
        "  --             end options; permits a DIR beginning with '-'\n"
        "\n"
        "DIR defaults to cwd. Options can precede or follow DIR. Conflicting levels,\n"
        "multiple roots and unknown or mode-incompatible options are errors.\n"
        "Inventory: C/C++, Python, JS/TS, Rust/Cargo, HTML/CSS/templates, shebang\n"
        "entrypoints, shell, docs, build and config\n"
        "files; opaque/unrecognized files, nested symlinks and special files excluded.\n"
        "Fixed ignored directories (any depth; not .gitignore): .git, .jash, build,\n"
        "build-speed, node_modules, .cache, target, .venv, venv, __pycache__,\n"
        "oci-export, .Trash, dist, Quarentena-codes, .DS_Store, .idea, .vscode,\n"
        ".pytest_cache, .mypy_cache, .ruff_cache. Directories containing their\n"
        "own regular CMakeCache.txt are excluded regardless of basename. No\n"
        "vendor-wide exclusion or blanket build-* exclusion.\n"
        "Included file content/path and directory additions/deletions/renames\n"
        "(including empty directories) invalidate; excluded content/mtime alone do not.\n"
        "Limits: depth 32; 32768 entries globally; 8 MiB/included file; 128 MiB total;\n"
        "8 MiB rendered output; 4095-byte paths; --jobs workers 1..32. Limit/read\n"
        "errors fail visibly.\n"
        "Cache: $HOME/.jash/digest/<key>.md; key binds canonical root, generator\n"
        "version, content inventory and level. Unsafe/corrupt cache is an error.\n"
        "Read-only project mode uses no subprocess/network; shell recording and\n"
        "telemetry are independent of --no-cache. Session mode retains its behavior.\n");
}

#include "digest_native.inc"

static int digest_execute_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv, semantic_builtin_t* call) {
    (void)s;
    digest_cfg_t cfg = {.level = DIGEST_STANDARD};
    int session = 0, no_git = 0, last_n = 0, jobs = 0, have_level = 0, options = 1;
    int ledger_mode = 0, cache_status_mode = 0;
    long ledger_n = 20;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (options && !strcmp(a, "--")) { options = 0; continue; }
        if (options && (!strcmp(a, "--help") || !strcmp(a, "-h"))) {
            if (call) {
                jdv_record_input_t fields[] = {
                    semantic_field("schema", semantic_symbol(&call->values, "jash.digest.help.v1")),
                    semantic_field("usage", semantic_text(&call->values, "digest FORMAT [--brief|--standard|--deep] [--refresh|--no-cache] [--manifest|--fingerprint|--diff-base FILE] [--jobs N] [--output FILE] [--] [DIR]; digest FORMAT --session [--last N] [--no-git]; digest FORMAT --ledger [N]|--cache-status|--fetch HASH")),
                    semantic_field("depth_limit", jdv_value_u64(DIGEST_MAX_DEPTH)),
                    semantic_field("inventory_entry_limit", jdv_value_u64(DIGEST_MAX_ENTRIES)),
                    semantic_field("file_byte_limit", jdv_value_filesize(DIGEST_MAX_FILE_BYTES)),
                    semantic_field("total_file_byte_limit", jdv_value_filesize(DIGEST_MAX_TOTAL_BYTES)),
                    semantic_field("native_value_byte_limit", jdv_value_filesize(SEMANTIC_VALUE_BYTES))
                };
                return semantic_finish(call, semantic_record(&call->values, fields, 7), 0);
            }
            digest_usage(w, ctx);
            return 0;
        }
        if (options && (!strcmp(a, "--brief") || !strcmp(a, "--standard") || !strcmp(a, "--deep"))) {
            digest_level_t level = !strcmp(a, "--brief") ? DIGEST_BRIEF :
                                   !strcmp(a, "--deep") ? DIGEST_DEEP : DIGEST_STANDARD;
            if (have_level && cfg.level != level) return digest_mode_error(call, "conflicting levels");
            have_level = 1;
            cfg.level = level;
        } else if (options && !strcmp(a, "--session")) session = 1;
        else if (options && !strcmp(a, "--no-git")) no_git = 1;
        else if (options && !strcmp(a, "--refresh")) cfg.refresh = 1;
        else if (options && !strcmp(a, "--no-cache")) cfg.no_cache = 1;
        else if (options && !strcmp(a, "--fingerprint")) cfg.fingerprint = 1;
        else if (options && !strcmp(a, "--manifest")) cfg.manifest = 1;
        else if (options && !strcmp(a, "--last")) {
            if (++i == argc || last_n) return digest_mode_error(call, "--last requires one integer 1..10000");
            const char* value = argv[i];
            for (const char* p = value; *p; p++)
                if (*p < '0' || *p > '9') return digest_mode_error(call, "--last requires an integer 1..10000");
            errno = 0;
            char* end;
            long n = strtol(value, &end, 10);
            if (errno || !*value || *end || n < 1 || n > 10000)
                return digest_mode_error(call, "--last requires an integer 1..10000");
            last_n = (int)n;
        } else if (options && !strcmp(a, "--jobs")) {
            if (++i == argc || jobs) return digest_mode_error(call, "--jobs requires one integer 1..32");
            const char* value = argv[i];
            for (const char* p = value; *p; p++)
                if (*p < '0' || *p > '9') return digest_mode_error(call, "--jobs requires an integer 1..32");
            errno = 0;
            char* end;
            long n = strtol(value, &end, 10);
            if (errno || !*value || *end || n < 1 || n > DIGEST_MAX_JOBS)
                return digest_mode_error(call, "--jobs requires an integer 1..32");
            jobs = (int)n;
        } else if (options && !strcmp(a, "--output")) {
            if (++i == argc || cfg.output_path || !*argv[i])
                return digest_mode_error(call, "--output requires one nonempty file argument");
            cfg.output_path = argv[i];
            if (strlen(cfg.output_path) >= DIGEST_MAX_PATH) return digest_mode_error(call, "output path exceeds 4095 bytes");
        } else if (options && !strcmp(a, "--diff-base")) {
            if (++i == argc || cfg.diff_base || !*argv[i])
                return digest_mode_error(call, "--diff-base requires one nonempty file argument");
            cfg.diff_base = argv[i];
            if (strlen(cfg.diff_base) >= DIGEST_MAX_PATH) return digest_mode_error(call, "diff base path exceeds 4095 bytes");
        } else if (options && !strcmp(a, "--ledger")) {
            if (ledger_mode) return digest_mode_error(call, "--ledger requires at most one count");
            ledger_mode = 1;
            if (i + 1 < argc && *argv[i+1]) {
                const char* value = argv[i+1];
                int digits = 1;
                for (const char* p = value; *p; p++)
                    if (*p < '0' || *p > '9') digits = 0;
                if (digits) {
                    errno = 0;
                    char* end;
                    long n = strtol(value, &end, 10);
                    if (errno || !*value || *end || n < 1 || n > 1000)
                        return digest_mode_error(call, "--ledger takes an optional integer 1..1000");
                    ledger_n = n;
                    i++;
                }
            }
        } else if (options && !strcmp(a, "--cache-status")) {
            if (cache_status_mode) return digest_mode_error(call, "--cache-status takes no argument");
            cache_status_mode = 1;
        } else if (options && !strcmp(a, "--fetch")) {
            if (++i == argc || cfg.fetch_hash || !*argv[i])
                return digest_mode_error(call, "--fetch requires one nonempty hash argument");
            if (strlen(argv[i]) != 64)
                return digest_mode_error(call, "--fetch requires a 64-character content hash");
            cfg.fetch_hash = argv[i];
        } else if (options && a[0] == '-') {
            return digest_mode_error(call, "unknown option (see digest --help)");
        } else {
            if (cfg.root_dir || !*a) return digest_mode_error(call, "expected at most one nonempty project directory");
            if (strlen(a) >= DIGEST_MAX_PATH) return digest_mode_error(call, "project path exceeds 4095 bytes");
            cfg.root_dir = a;
        }
    }
    if (session && (cfg.root_dir || cfg.refresh || cfg.no_cache || cfg.fingerprint || cfg.manifest || jobs || cfg.diff_base))
        return digest_mode_error(call, "--session does not accept project inventory options");
    if (cfg.manifest && cfg.fingerprint) return digest_mode_error(call, "--manifest conflicts with --fingerprint");
    if (cfg.diff_base && (cfg.manifest || cfg.fingerprint))
        return digest_mode_error(call, "--diff-base conflicts with --fingerprint/--manifest");
    if (cfg.diff_base && have_level)
        return digest_mode_error(call, "--diff-base has a fixed format (no level)");
    if (cfg.diff_base && cfg.refresh)
        return digest_mode_error(call, "--diff-base does not use the cache (no --refresh)");
    int queries = (ledger_mode ? 1 : 0) + (cache_status_mode ? 1 : 0) +
                  (cfg.fetch_hash ? 1 : 0);
    if (queries > 1)
        return digest_mode_error(call, "--ledger, --cache-status and --fetch are mutually exclusive");
    if (queries &&
        (session || have_level || cfg.refresh || cfg.no_cache || cfg.fingerprint ||
         cfg.manifest || cfg.diff_base || jobs || cfg.root_dir))
        return digest_mode_error(call, "--ledger, --cache-status and --fetch accept only --output");
    if (!session && (last_n || no_git)) return digest_mode_error(call, "--last and --no-git require --session");

    if (call) {
        cfg.jobs = jobs;
        return digest_native_run(call, &cfg, session, no_git, last_n, ledger_mode, ledger_n, cache_status_mode);
    }

    if (queries) {
        jash_invoice_account_command("digest", 1);
        if (ledger_mode)
            return digest_ledger_print(ledger_n, cfg.output_path, w, ctx);
        if (cache_status_mode)
            return digest_cache_status_print(cfg.output_path, w, ctx);
        return digest_fetch_print(cfg.fetch_hash, cfg.output_path, w, ctx);
    }

    /* Invoice helpers persist to disk. Read-only project calls bypass them as
     * well as the cache/budget spool; outer shell telemetry is independent. */
    if (session || (!cfg.no_cache && !cfg.fingerprint && !cfg.manifest)) {
        jash_invoice_account_command("digest", 1);
        jash_invoice_add_saved(JASH_INVOICE_DIGEST, 1);
    }
    if (session) {
        digest_session_cfg_t scfg = {
            .level = (digest_session_level_t)cfg.level,
            .last_n = last_n,
            .include_git = !no_git
        };
        if (cfg.output_path) {
            FILE* f = fopen(cfg.output_path, "w");
            if (!f) return digest_mode_error(call, "cannot open session output file");
            int rc = digest_session_run(&scfg, digest_file_writer, f);
            if (ferror(f)) rc = 1;
            if (fclose(f)) rc = 1;
            if (rc) jash_writes(jash_stderr_writer, NULL, "digest: session output failed\n");
            return rc;
        }
        return digest_session_run(&scfg, w, ctx);
    }
    if (!cfg.root_dir) cfg.root_dir = ".";
    cfg.jobs = jobs;
    /* Project output is bounded and rendered in memory by the engine. The old
     * budget wrapper may spool to disk: incompatible with --no-cache. */
    return digest_run(&cfg, w, ctx);
}

static int digest_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    return digest_execute_argv(state, call->writer, call->context, argc, argv, call);
}

int jash_b_digest_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int result = semantic_dispatch_argv(state, writer, context, "digest", "jash.digest.v1", argc, argv, digest_native_provider);
    return result >= 0 ? result : digest_execute_argv(state, writer, context, argc, argv, NULL);
}

int jash_b_digest(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0)
        return digest_arg_error("cannot tokenize arguments");
    int rc = jash_b_digest_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
