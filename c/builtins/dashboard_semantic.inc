// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_chain.h"
#include "mesh_snapshot.h"

static jdv_value_t dashboard_observed_count(semantic_value_t* sem, const char* filename,
        uint64_t* count, int* status) {
    memory_store_t store = {-1, -1}; char* bytes = NULL; size_t size = 0; int exists = 0, error = 0;
    if (memory_store_open(&store, NULL, 0) || memory_store_read(&store, filename, MEMORY_STORE_MAX_BYTES, &bytes, &size, &exists)) error = errno;
    if (memory_store_close(&store) && !error) error = errno;
    *count = 0;
    if (!error && size) {
        if (memchr(bytes, 0, size) || bytes[size - 1] != '\n') error = EINVAL;
        else for (size_t i = 0; i < size; i++) if (bytes[i] == '\n') (*count)++;
    }
    free(bytes); *status = error;
    jdv_record_input_t fields[] = {
        semantic_field("file", semantic_text(sem, filename)),
        semantic_field("present", jdv_value_bool(exists)),
        semantic_field("complete", jdv_value_bool(!error)),
        semantic_field("basis", semantic_symbol(sem, "recorded-metadata-lines")),
        semantic_field("bytes", jdv_value_filesize(size)),
        semantic_field("lines", error ? jdv_value_nothing() : jdv_value_u64(*count)),
        semantic_field("error", semantic_symbol(sem, error ? memory_store_error(error) : NULL))
    };
    return semantic_record(sem, fields, 7);
}

static int dashboard_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)argv;
    if (argc) return semantic_error(call, "invalid_arguments", 2);
    semantic_value_t* sem = &call->values;
    memory_chain_t chain;
    int chain_error = memory_chain_load(&chain, getenv("JASH_RECALL_GLOBAL") != NULL, 0);
    uint64_t total = 0, failed = 0, diff_uses = 0, per[32] = {0};
    uint64_t first = 0, last = 0; int monotonic = 1;
    if (!chain_error) for (size_t i = 0; i < chain.count; i++) {
        const memory_chain_entry_t* entry = &chain.entries[i];
        total++; if (entry->status) failed++;
        if (!i) first = entry->seconds;
        else if (entry->seconds < last) monotonic = 0;
        last = entry->seconds;
        int feature = scan_differentiator(entry->command);
        if (feature >= 0 && feature < 32) { diff_uses++; per[feature]++; }
    }
    jdv_value_t chain_source = memory_chain_source(sem, &chain);
    memory_chain_free(&chain);
    jash_mesh_snapshot_t mesh;
    int mesh_status = jash_mesh_snapshot_counts(state, &mesh);
    if (mesh_status && mesh.error_number == ECANCELED)
        return semantic_cancelled(call, mesh_status);
    uint64_t checkpoints = 0; int checkpoint_error = 0;
    jdv_value_t checkpoint_source = dashboard_observed_count(sem, "checkpoints.tsv", &checkpoints, &checkpoint_error);
    unsigned guard_total = 0, native = 0, bypass = 0, blocks = 0, upgraded = 0;
    jash_guard_metrics_snapshot(&guard_total, &native, &bypass, &blocks, &upgraded);
    jdv_record_input_t feature_fields[32]; size_t features = 0;
    for (size_t i = 0; DIFFS[i] && i < 32; i++) if (per[i]) feature_fields[features++] = semantic_field(DIFFS[i], jdv_value_u64(per[i]));
    jdv_record_input_t adoption[] = {
        semantic_field("pct", !chain_error && total ? jdv_value_u64((diff_uses * 100u + total / 2u) / total) : jdv_value_nothing()),
        semantic_field("differentiator_uses", chain_error ? jdv_value_nothing() : jdv_value_u64(diff_uses)),
        semantic_field("features", chain_error ? jdv_value_nothing() : semantic_record(sem, feature_fields, features)),
        semantic_field("classifier", semantic_symbol(sem, "legacy-command-position-heuristic"))
    };
    jdv_record_input_t guard[] = {
        semantic_field("total", jdv_value_u64(guard_total)), semantic_field("native", jdv_value_u64(native)),
        semantic_field("bypass_count", jdv_value_u64(bypass)), semantic_field("block_count", jdv_value_u64(blocks)),
        semantic_field("upgraded", jdv_value_u64(upgraded)),
        semantic_field("pct", guard_total ? jdv_value_u64(((uint64_t)bypass * 100u + guard_total / 2u) / guard_total) : jdv_value_nothing()),
        semantic_field("source", semantic_symbol(sem, "guard-engine-cumulative-snapshot")),
        semantic_field("counter_width_bits", jdv_value_u64(sizeof(unsigned) * CHAR_BIT))
    };
    jdv_record_input_t recording[] = {
        semantic_field("level", jdv_value_i64(jash_sigil_get())),
        semantic_field("trash_protected", jdv_value_bool(jash_trash_protected()))
    };
    jdv_record_input_t governor[] = {
        semantic_field("level", jdv_value_i64(jash_adopt_level())),
        semantic_field("hints_used", jdv_value_i64(jash_hint_budget_used())),
        semantic_field("hint_budget", jdv_value_i64(jash_hint_budget_cap()))
    };
    jdv_record_input_t board[] = {
        semantic_field("total", mesh.complete ? jdv_value_u64(mesh.tasks) : jdv_value_nothing()),
        semantic_field("ready", mesh.complete ? jdv_value_u64(mesh.tasks_ready) : jdv_value_nothing()),
        semantic_field("claimed", mesh.complete ? jdv_value_u64(mesh.tasks_claimed) : jdv_value_nothing()),
        semantic_field("done", mesh.complete ? jdv_value_u64(mesh.tasks_done) : jdv_value_nothing()),
        semantic_field("failed", mesh.complete ? jdv_value_u64(mesh.tasks_failed) : jdv_value_nothing())
    };
    jdv_record_input_t fleet[] = {
        semantic_field("total", mesh.complete ? jdv_value_u64(mesh.workers) : jdv_value_nothing()),
        semantic_field("live", mesh.complete ? jdv_value_u64(mesh.workers_live) : jdv_value_nothing()),
        semantic_field("dead", mesh.complete ? jdv_value_u64(mesh.workers_dead) : jdv_value_nothing()),
        semantic_field("unknown", mesh.complete ? jdv_value_u64(mesh.workers_unknown) : jdv_value_nothing()),
        semantic_field("liveness_basis", semantic_symbol(sem, "explicit-harness-pid-kill-zero"))
    };
    jdv_record_input_t mesh_source[] = {
        semantic_field("present", jdv_value_bool(mesh.store_present)),
        semantic_field("complete", jdv_value_bool(mesh.complete)),
        semantic_field("observed_at_seconds", jdv_value_i64(mesh.observed_at)),
        semantic_field("status", jdv_value_i64(mesh.status)),
        semantic_field("errno", mesh.error_number ? jdv_value_i64(mesh.error_number) : jdv_value_nothing()),
        semantic_field("reason", semantic_text(sem, mesh.reason)),
        semantic_field("claims", mesh.complete ? jdv_value_u64(mesh.claims) : jdv_value_nothing()),
        semantic_field("claims_live", mesh.complete ? jdv_value_u64(mesh.claims_live) : jdv_value_nothing()),
        semantic_field("claims_expired", mesh.complete ? jdv_value_u64(mesh.claims_expired) : jdv_value_nothing())
    };
    jdv_record_input_t sources[] = {
        semantic_field("chain", chain_source), semantic_field("mesh", semantic_record(sem, mesh_source, 9)),
        semantic_field("checkpoints", checkpoint_source)
    };
    int status = mesh_status ? mesh_status : chain_error || checkpoint_error ? 1 : 0;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("consistency", semantic_symbol(sem, "independent-source-observations")),
        semantic_field("sources", semantic_record(sem, sources, 3)),
        semantic_field("recording", semantic_record(sem, recording, 2)),
        semantic_field("commands", chain_error ? jdv_value_nothing() : jdv_value_u64(total)),
        semantic_field("command_counter_basis", semantic_symbol(sem, "all-non-marker-chain-records")),
        semantic_field("failed", chain_error ? jdv_value_nothing() : jdv_value_u64(failed)),
        semantic_field("fail_pct", !chain_error && total ? jdv_value_u64((failed * 100u + total / 2u) / total) : jdv_value_nothing()),
        semantic_field("uptime_s", !chain_error && total && monotonic ? jdv_value_u64(last - first) : jdv_value_nothing()),
        semantic_field("uptime_basis", semantic_symbol(sem, "recorded-chain-timestamp-span")),
        semantic_field("adoption", semantic_record(sem, adoption, 4)), semantic_field("bypass", semantic_record(sem, guard, 8)),
        semantic_field("governor", semantic_record(sem, governor, 3)),
        semantic_field("fleet", mesh.complete ? jdv_value_u64(mesh.workers) : jdv_value_nothing()),
        semantic_field("fleet_observation", semantic_record(sem, fleet, 5)), semantic_field("board", semantic_record(sem, board, 5)),
        semantic_field("checkpoints", checkpoint_error ? jdv_value_nothing() : jdv_value_u64(checkpoints))
    };
    return semantic_finish(call, semantic_record(sem, fields, 18), status);
}
