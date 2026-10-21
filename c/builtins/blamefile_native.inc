// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include "memory_output.h"
#include "memory_chain.h"

static int blame_native_path(const char* path, char out[PATH_MAX]) {
    char absolute[PATH_MAX];
    int n;
    if (path[0] == '/') n = snprintf(absolute, sizeof(absolute), "%s", path);
    else {
        char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd))) return -1;
        n = snprintf(absolute, sizeof(absolute), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", path);
    }
    if (n < 0 || (size_t)n >= sizeof(absolute)) { errno = ENAMETOOLONG; return -1; }
    size_t size = strlen(absolute);
    while (size > 1 && absolute[size - 1] == '/') absolute[--size] = 0;
    char* slash = strrchr(absolute, '/');
    char basename[PATH_MAX]; if (jash_copy_cstr(basename, sizeof(basename), slash ? slash + 1 : absolute)) { errno = ENAMETOOLONG; return -1; }
    if (!*basename || !strcmp(basename, ".") || !strcmp(basename, "..")) {
        return realpath(absolute, out) ? 0 : -1;
    }
    if (slash == absolute) slash[1] = 0; else if (slash) *slash = 0;
    char parent[PATH_MAX];
    if (!realpath(absolute, parent)) return -1;
    n = snprintf(out, PATH_MAX, "%s%s%s", parent, strcmp(parent, "/") ? "/" : "", basename);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int blame_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* path = NULL; uint64_t offset = 0, limit = 128; int literal = 0, have_offset = 0, have_limit = 0;
    for (int i = 0; i < argc; i++) {
        if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argv[i], "--offset") && !have_offset && i + 1 < argc) {
            if (memory_unsigned(argv[++i], 128, &offset)) return semantic_error(call, "invalid_arguments", 2);
            have_offset = 1;
        } else if (!literal && !strcmp(argv[i], "--limit") && !have_limit && i + 1 < argc) {
            if (memory_unsigned(argv[++i], 128, &limit) || !limit) return semantic_error(call, "invalid_arguments", 2);
            have_limit = 1;
        } else if (path || !*argv[i] || (!literal && argv[i][0] == '-')) return semantic_error(call, "invalid_arguments", 2);
        else path = argv[i];
    }
    if (!path) return semantic_error(call, "invalid_arguments", 2);
    char normalized[PATH_MAX];
    if (blame_native_path(path, normalized)) return semantic_error(call, "path_resolution_failed", 1);
    memory_chain_t chain;
    int status = memory_chain_load(&chain, 0, 0) ? 1 : 0;
    semantic_value_t* sem = &call->values; jdv_value_t entries[128];
    size_t matched = 0, emitted = 0;
    for (unsigned i = 0; i < 128; i++) {
        jash_timeline_entry_view_t entry;
        if (!jash_timeline_entry_at(i, &entry)) break;
        if (entry.kind == JASH_UNDO_CHDIR || entry.kind == JASH_UNDO_PROC_RESTORE || entry.kind == JASH_UNDO_PROC_SNAPSHOT) continue;
        int match = (entry.a_is_path && entry.a && !strcmp(entry.a, normalized)) ||
                    (entry.b_is_path && entry.b && !strcmp(entry.b, normalized));
        if (!match) continue;
        matched++;
        if (matched <= offset || emitted >= limit) continue;
        memory_chain_entry_t* nearest = NULL; uint64_t best = UINT64_MAX, candidates = 0;
        if (!chain.error && entry.timestamp_seconds >= 0) for (size_t c = 0; c < chain.count; c++) {
            uint64_t stamp = (uint64_t)entry.timestamp_seconds;
            uint64_t delta = chain.entries[c].seconds >= stamp ? chain.entries[c].seconds - stamp : stamp - chain.entries[c].seconds;
            if (delta > BF_WINDOW_S) continue;
            candidates++;
            if (delta < best) { nearest = &chain.entries[c]; best = delta; }
        }
        jdv_record_input_t fields[] = {
            semantic_field("id", jdv_value_u64(entry.id)),
            semantic_field("inverse_kind", semantic_symbol(sem, entry.kind_name)),
            semantic_field("recorded_description", semantic_text(sem, entry.description)),
            semantic_field("timestamp", jdv_value_i64(entry.timestamp_seconds)),
            semantic_field("path_a", entry.a_is_path ? semantic_path(sem, entry.a) : jdv_value_nothing()),
            semantic_field("path_b", entry.b_is_path ? semantic_path(sem, entry.b) : jdv_value_nothing()),
            semantic_field("nearest_command", memory_chain_value(sem, nearest)),
            semantic_field("candidate_count", jdv_value_u64(candidates)),
            semantic_field("delta_seconds", nearest ? jdv_value_u64(best) : jdv_value_nothing()),
            semantic_field("attribution", semantic_symbol(sem, "temporal_proximity_not_proof")),
            semantic_field("restoration_assessed", jdv_value_bool(0))
        };
        entries[emitted++] = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    }
    if (!status && !matched) status = 2;
    int truncated = offset + emitted < matched;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.blame-file.v1")),
        semantic_field("path", semantic_path(sem, normalized)),
        semantic_field("entries", semantic_list(sem, entries, emitted)),
        semantic_field("matched", jdv_value_u64(matched)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", truncated ? jdv_value_u64(offset + emitted) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(truncated)),
        semantic_field("timeline_lost", jdv_value_u64((uint64_t)jash_timeline_lost())),
        semantic_field("history_scope", semantic_symbol(sem, "retained_timeline_ring_max_128")),
        semantic_field("chain_source", memory_chain_source(sem, &chain)),
        semantic_field("error", semantic_symbol(sem, chain.error ? "chain_read_failed" : !matched ? "not_recorded" : NULL))
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    memory_chain_free(&chain); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(blamefile, "blame-file", "jash.blame-file.v1", blame_native_provider)
