/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */

static int sn_quarantine_artifacts(semantic_builtin_t* call, jdv_value_t* manifests, jdv_value_t* artifacts) {
    char* names[SN_QUERY_ROWS]; size_t count = 0; int error = 0;
    int descriptor = open(g_ledger.quarantine_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno != ENOENT) return -1;
        *manifests = semantic_list(&call->values, NULL, 0);
        *artifacts = semantic_list(&call->values, NULL, 0); return 0;
    }
    DIR* directory = fdopendir(descriptor);
    if (!directory) { int number = errno; close(descriptor); errno = number; return -1; }
    struct dirent* entry;
    for (;;) {
        errno = 0; entry = readdir(directory);
        if (!entry) { if (errno) error = errno; break; }
        if (entry->d_name[0] == '.' || strstr(entry->d_name, ".tmp.")) continue;
        if (count == SN_QUERY_ROWS) { error = EFBIG; break; }
        struct stat metadata;
        if (fstatat(dirfd(directory), entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW)) { error = errno; break; }
        if (!S_ISREG(metadata.st_mode)) { error = EINVAL; break; }
        names[count] = strdup(entry->d_name);
        if (!names[count]) { error = ENOMEM; break; }
        count++;
    }
    if (closedir(directory) && !error) error = errno;
    /* The directory order is not a semantic ordering. Sort full byte strings
     * before constructing either projection. */
    for (size_t i = 1; i < count; i++) {
        char* value = names[i]; size_t at = i;
        while (at && strcmp(names[at - 1], value) > 0) { names[at] = names[at - 1]; at--; }
        names[at] = value;
    }
    jdv_value_t manifest_rows[SN_QUERY_ROWS], artifact_rows[SN_QUERY_ROWS];
    size_t manifest_count = 0, artifact_count = 0;
    for (size_t i = 0; i < count; i++) {
        char path[PATH_MAX];
        int size = snprintf(path, sizeof path, "%s/%s", g_ledger.quarantine_dir, names[i]);
        if (size < 0 || (size_t)size >= sizeof path) error = ENAMETOOLONG;
        else if (!error) {
            jdv_value_t value = semantic_path(&call->values, path);
            if (strstr(names[i], ".manifest.json")) manifest_rows[manifest_count++] = value;
            else artifact_rows[artifact_count++] = value;
        }
        free(names[i]);
    }
    if (error) { errno = error; return -1; }
    *manifests = semantic_list(&call->values, manifest_rows, manifest_count);
    *artifacts = semantic_list(&call->values, artifact_rows, artifact_count);
    return 0;
}

static int sn_quarantine_status(semantic_builtin_t* call) {
    int transaction = chain_transaction_lock();
    if (transaction < 0) return semantic_error(call, "transaction_lock_failed", 1);
    int present = 0, witness_present = 0, old_present = 0;
    const char* error = NULL;
    sigil_chain_scan_t scan = {0}, old = {0};
    scan.first_fragment_offset = old.first_fragment_offset = -1;
    char old_path[PATH_MAX] = {0};
    if (sigil_recovery_input(chain_path(), &present) ||
        sigil_recovery_input(ratchet_path(), &witness_present) ||
        sigil_path_suffix(old_path, sizeof old_path, chain_path(), ".v1") ||
        sigil_recovery_input(old_path, &old_present)) error = "recovery_input_unavailable";
    if (!error && ((present && sigil_scan_chain(chain_path(), &scan)) ||
                  (old_present && sigil_scan_chain(old_path, &old)))) error = "recovery_scan_failed";
    const char* reason = error ? NULL : present ? sigil_scan_recovery_reason(&scan) : sigil_absent_chain_recovery_reason();
    uint64_t advances = 0, entries = 0; int available = 0, mismatch = 0;
    const char* scope = "unavailable";
    if (!error && present && witness_present) {
        uint8_t key[32] = {0}; char nonce[33] = {0};
        available = ratchet_read_at(ratchet_path(), key, &advances, nonce) == 0;
        memset(key, 0, sizeof key);
        if (available) {
            int segment = scan.boundaries && scan.bnd_nonce[0] && !strcmp(scan.bnd_nonce, nonce);
            scope = segment ? "current_segment" : "whole_chain";
            entries = segment ? scan.entries_after_boundary : scan.s7_entries;
            mismatch = entries != advances;
        } else if (!reason) error = "ratchet_read_failed";
    }
    if (!reason && mismatch) reason = "ratchet_position_mismatch";
    jdv_value_t manifests = jdv_value_nothing(), artifacts = jdv_value_nothing();
    if (!error && sn_quarantine_artifacts(call, &manifests, &artifacts))
        error = errno == EFBIG ? "recovery_artifact_limit" : "recovery_artifacts_unavailable";
    if (error) {
        chain_transaction_unlock(transaction);
        return semantic_error(call, error, 1);
    }
    semantic_value_t* values = &call->values;
    jdv_record_input_t scanned[] = {
        semantic_field("attested", jdv_value_u64(scan.s7_entries)),
        semantic_field("unattested", jdv_value_u64(scan.unattested)),
        semantic_field("fragments", jdv_value_u64(scan.fragments)),
        semantic_field("malformed", jdv_value_u64(scan.malformed)),
        semantic_field("legacy", jdv_value_u64(scan.legacy_entries))
    };
    jdv_record_input_t witness[] = {
        semantic_field("available", jdv_value_bool(available)),
        semantic_field("position", semantic_symbol(values, !available ? "unavailable" : mismatch ? "mismatch" : "match")),
        semantic_field("scope", semantic_symbol(values, scope)),
        semantic_field("entries", available ? jdv_value_u64(entries) : jdv_value_nothing()),
        semantic_field("ratchet_advances", available ? jdv_value_u64(advances) : jdv_value_nothing())
    };
    jdv_record_input_t invalid[] = {
        semantic_field("path", semantic_path(values, old_path)),
        semantic_field("reason", semantic_symbol(values, "invalid_migration_v1")),
        semantic_field("entries", jdv_value_u64(old.s7_entries + old.legacy_entries)),
        semantic_field("fragments", jdv_value_u64(old.fragments))
    };
    jdv_record_input_t fields[] = {
        semantic_field("chain", semantic_path(values, chain_path())),
        semantic_field("ratchet", semantic_path(values, ratchet_path())),
        semantic_field("chain_present", jdv_value_bool(present)),
        semantic_field("ratchet_present", jdv_value_bool(witness_present)),
        semantic_field("active_reason", semantic_symbol(values, reason)),
        semantic_field("scan", semantic_record(values, scanned, sizeof scanned / sizeof scanned[0])),
        semantic_field("witness", semantic_record(values, witness, sizeof witness / sizeof witness[0])),
        semantic_field("invalid_migration", old_present && old.tag_lines ? semantic_record(values, invalid, 4) : jdv_value_nothing()),
        semantic_field("quarantined", manifests),
        semantic_field("recovery_artifacts", artifacts),
        semantic_field("verification_scope", semantic_symbol(values, "structural scan and witness count; hashes and historical tags not verified"))
    };
    chain_transaction_unlock(transaction);
    return sn_query_finish(call, "quarantine.status", semantic_record(values, fields, sizeof fields / sizeof fields[0]), 1, NULL, 0);
}

typedef struct {
    semantic_builtin_t* call;
    const char* operation;
    size_t delivered;
    uint64_t failed;
} sn_transition_output_t;

static jdv_value_t sn_transition_value(sn_transition_output_t* output, const sigil_transition_event_t* event) {
    semantic_value_t* values = &output->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.transition.v1")),
        semantic_field("row_kind", semantic_symbol(values, "receipt")),
        semantic_field("operation", semantic_symbol(values, output->operation)),
        semantic_field("stage", semantic_symbol(values, event->stage)),
        semantic_field("path", semantic_path(values, event->path)),
        semantic_field("auxiliary", semantic_path(values, event->auxiliary)),
        semantic_field("manifest", semantic_path(values, event->manifest)),
        semantic_field("reason", semantic_symbol(values, event->reason)),
        semantic_field("scope", semantic_symbol(values, event->scope)),
        semantic_field("applied", event->applied < 0 ? jdv_value_nothing() : jdv_value_bool(event->applied)),
        semantic_field("durable", event->durable < 0 ? jdv_value_nothing() : jdv_value_bool(event->durable)),
        semantic_field("outcome", jdv_value_i64(event->outcome)),
        semantic_field("errno", event->error_number ? jdv_value_i64(event->error_number) : jdv_value_nothing()),
        semantic_field("sequence", event->has_sequence ? jdv_value_u64(event->sequence) : jdv_value_nothing()),
        semantic_field("entries", jdv_value_u64(event->entries)),
        semantic_field("fragments", jdv_value_u64(event->fragments)),
        semantic_field("retained_offset", event->retained_offset >= 0 ? jdv_value_i64(event->retained_offset) : jdv_value_nothing())
    };
    return semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}

static int sn_transition_emit(void* context, const sigil_transition_event_t* event) {
    sn_transition_output_t* output = context; semantic_builtin_t* call = output->call;
    jdv_value_t value = sn_transition_value(output, event);
    if (call->values.error) return 125;
    if (call->mode == 2) {
        if (jdv_wire_push_v2_item(call->push, &value, 0)) return 125;
    } else {
        if (output->delivered) jash_jsonw_raw(&call->output.json, ",");
        if (jdv_json_write(&call->output.json, &value, &call->values.limits) ||
            jash_jsonw_finish(&call->output.json)) return 125;
    }
    output->delivered++;
    output->failed += event->outcome != 0;
    return 0;
}

static int sn_transition_start(sn_transition_output_t* output) {
    semantic_builtin_t* call = output->call;
    char path[PATH_MAX], suffix[301];
    memset(suffix, 'x', sizeof suffix - 1); suffix[sizeof suffix - 1] = 0;
    int path_size = snprintf(path, sizeof path, "%s/%s", g_ledger.quarantine_dir, suffix);
    if (path_size < 0 || (size_t)path_size >= sizeof path) return semantic_error(call, "recovery_path_limit", 2);
    sigil_transition_event_t maximum = {
        .stage = "quarantine-active-pair-and-preserve-outgoing-witness", .path = path,
        .auxiliary = path, .manifest = path, .reason = "outgoing_witness_publication_failed",
        .scope = "active-ledger-recovery-group-and-pinned-repository-lineage", .applied = 1, .durable = 1,
        .outcome = INT_MIN, .error_number = INT_MAX, .has_sequence = 1, .sequence = UINT64_MAX,
        .entries = UINT64_MAX, .fragments = UINT64_MAX, .retained_offset = LLONG_MAX
    };
    jdv_value_t rows[8];
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) rows[i] = sn_transition_value(output, &maximum);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.sigil.transition-report.v1")),
        semantic_field("receipts", semantic_list(&call->values, rows, 8)),
        semantic_field("result", rows[0])
    };
    jdv_value_t planned = semantic_record(&call->values, fields, 3);
    int status = semantic_preflight(call, &planned); if (status) return status;
    if (call->mode == 1) {
        jash_jsonw_raw(&call->output.json, "{\"schema\":\"jash.sigil.transition-report.v1\",\"receipts\":[");
        if (jash_jsonw_finish(&call->output.json)) return 125;
    }
    return 0;
}

static int sn_transition_finish(sn_transition_output_t* output, jdv_value_t result, int status) {
    semantic_builtin_t* call = output->call;
    if (call->values.error) return 125;
    if (call->mode == 2) {
        if (jdv_wire_push_v2_item(call->push, &result, 0)) return 125;
        jdv_stream_end_t outcome, final; (void)jdv_stream_end_init(&outcome);
        outcome.state = status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
        outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
        outcome.producer_status = status;
        outcome.errors_seen = output->failed ? output->failed : status ? 1 : 0;
        if (jdv_wire_push_v2_end(call->push, &outcome, &final)) return 125;
    } else {
        jash_jsonw_raw(&call->output.json, "],\"result\":");
        if (jdv_json_write(&call->output.json, &result, &call->values.limits)) return 125;
        jash_jsonw_raw(&call->output.json, "}\n");
    }
    return jash_structured_finish(&call->output, status);
}

static int sn_quarantine_apply(semantic_builtin_t* call) {
    sn_transition_output_t output = {.call = call, .operation = "quarantine.apply"};
    int status = sn_transition_start(&output); if (status) return status;
    sigil_quarantine_observation_t observation;
    status = sigil_quarantine_apply_observed(&observation, sn_transition_emit, &output);
    if (observation.delivery_failed) return 125;
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.transition.v1")),
        semantic_field("row_kind", semantic_symbol(values, "result")),
        semantic_field("operation", semantic_symbol(values, "quarantine.apply")),
        semantic_field("chain", semantic_path(values, chain_path())),
        semantic_field("ratchet", semantic_path(values, ratchet_path())),
        semantic_field("applied_groups", jdv_value_u64((uint64_t)observation.applied)),
        semantic_field("failed_groups", jdv_value_u64((uint64_t)observation.failed)),
        semantic_field("active_reset", jdv_value_bool(observation.active_reset)),
        semantic_field("tail_salvaged", jdv_value_bool(observation.tail_salvaged)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("error", semantic_symbol(values, observation.error)),
        semantic_field("exit_code", jdv_value_i64(status)),
        semantic_field("scope", semantic_symbol(values, "pinned-module-ledger")),
        semantic_field("module_initialization_may_have_written", jdv_value_bool(1))
    };
    return sn_transition_finish(&output, semantic_record(values, fields, sizeof fields / sizeof fields[0]), status);
}

static int sn_ratchet_transition(semantic_builtin_t* call, int adopt) {
    const char* operation = adopt ? "ratchet.adopt" : "ratchet.rekey";
    sn_transition_output_t output = {.call = call, .operation = operation};
    int status = sn_transition_start(&output); if (status) return status;
    sigil_ratchet_observation_t observation;
    status = sigil_ratchet_transition_observed(&observation, adopt, sn_transition_emit, &output);
    if (observation.delivery_failed) return 125;
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.transition.v1")),
        semantic_field("row_kind", semantic_symbol(values, "result")),
        semantic_field("operation", semantic_symbol(values, operation)),
        semantic_field("chain", semantic_path(values, chain_path())),
        semantic_field("ratchet", semantic_path(values, ratchet_path())),
        semantic_field("mode", semantic_symbol(values, observation.mode)),
        semantic_field("from_shared", jdv_value_bool(observation.from_shared)),
        semantic_field("boundary_sealed", jdv_value_bool(observation.boundary_sealed)),
        semantic_field("boundary_sequence", observation.boundary_sealed ? jdv_value_u64(observation.boundary_sequence) : jdv_value_nothing()),
        semantic_field("previous_entries", jdv_value_u64(observation.previous_entries)),
        semantic_field("previous_advances", jdv_value_u64(observation.previous_advances)),
        semantic_field("previous_advances_scope", semantic_symbol(values, observation.from_shared ? "shared-aggregate-across-repos" : "repo-local")),
        semantic_field("retired_witness", semantic_path(values, observation.retired_witness[0] ? observation.retired_witness : NULL)),
        semantic_field("sidecar", semantic_path(values, observation.sidecar[0] ? observation.sidecar : NULL)),
        semantic_field("outgoing_published", jdv_value_bool(observation.outgoing_published)),
        semantic_field("retired_published", jdv_value_bool(observation.retired_published)),
        semantic_field("replacement_published", jdv_value_bool(observation.replacement_published)),
        semantic_field("sidecar_written", jdv_value_bool(observation.sidecar_written)),
        semantic_field("exclusivity", semantic_symbol(values, adopt ? "derived-from-shared; weaker exclusivity" : "fresh-key; unrelated to retired key")),
        semantic_field("past_entries_promoted_to_verified", jdv_value_bool(0)),
        semantic_field("applied_groups", jdv_value_u64((uint64_t)observation.applied_groups)),
        semantic_field("failed_groups", jdv_value_u64((uint64_t)observation.failed_groups)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("error", semantic_symbol(values, observation.error)),
        semantic_field("exit_code", jdv_value_i64(status)),
        semantic_field("scope", semantic_symbol(values, "pinned-module-ledger")),
        semantic_field("module_initialization_may_have_written", jdv_value_bool(1))
    };
    return sn_transition_finish(&output, semantic_record(values, fields, sizeof fields / sizeof fields[0]), status);
}
