/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Recovery publication groups report concrete observations immediately after
 * the group is stable. A failed delivery stops before the next group; it does
 * not interrupt a rollback needed to preserve the only recovery copy. */
typedef struct {
    const char* stage;
    const char* path;
    const char* auxiliary;
    const char* manifest;
    const char* reason;
    const char* scope;
    int applied;                 /* -1: attempted, outcome not established */
    int durable;                 /* -1: not measured */
    int outcome;
    int error_number;
    int has_sequence;
    uint64_t sequence, entries, fragments;
    long long retained_offset;
} sigil_transition_event_t;
typedef int (*sigil_transition_callback_t)(void*, const sigil_transition_event_t*);

typedef struct {
    int applied, failed, active_reset, tail_salvaged;
    int delivery_failed;
    const char* error;
} sigil_quarantine_observation_t;

static int sigil_recovery_event(sigil_quarantine_observation_t* report,
        sigil_transition_callback_t callback, void* context,
        const char* stage, const char* path, const char* manifest,
        const char* reason, int outcome, uint64_t entries,
        uint64_t fragments, long long retained_offset) {
    sigil_transition_event_t event = {
        .stage = stage, .path = path,
        .manifest = manifest && *manifest ? manifest : NULL,
        .reason = reason, .scope = "active-ledger-recovery-group",
        .applied = outcome == -2 ? -1 : outcome >= 0, .durable = outcome == 0,
        .outcome = outcome, .error_number = outcome ? errno : 0,
        .entries = entries, .fragments = fragments, .retained_offset = retained_offset
    };
    if (outcome >= 0) report->applied++;
    if (outcome) report->failed++;
    if (callback && callback(context, &event)) { report->delivery_failed = 1; return -1; }
    return 0;
}

static int sigil_recovery_input(const char* path, int* present) {
    struct stat metadata; *present = 0;
    if (lstat(path, &metadata)) return errno == ENOENT ? 0 : -1;
    *present = 1;
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 || (uint64_t)metadata.st_size > 32u * 1024u * 1024u) {
        errno = S_ISREG(metadata.st_mode) ? EFBIG : EINVAL; return -1;
    }
    return 0;
}

static int sigil_quarantine_apply_observed(sigil_quarantine_observation_t* report,
        sigil_transition_callback_t callback, void* context) {
    memset(report, 0, sizeof *report);
    int transaction = chain_transaction_lock();
    if (transaction < 0) { report->failed = 1; report->error = "transaction_lock_failed"; return 1; }
    int chain_present, ratchet_present;
    if (sigil_recovery_input(chain_path(), &chain_present) ||
        sigil_recovery_input(ratchet_path(), &ratchet_present)) {
        report->failed = 1; report->error = "recovery_input_unavailable"; goto finish;
    }
    (void)ratchet_present;
    sigil_chain_scan_t active = {0}; active.first_fragment_offset = -1;
    const char* reason = NULL;
    if (chain_present) {
        if (sigil_scan_chain(chain_path(), &active)) {
            report->failed = 1; report->error = "active_scan_failed"; goto finish;
        }
        reason = sigil_scan_recovery_reason(&active);
    } else reason = sigil_absent_chain_recovery_reason();
    sigil_verify_observation_t verification;
    (void)sigil_verify_observe(&verification, transaction);
    if (!reason && verification.failure &&
        (!strcmp(verification.failure, "hash_mismatch") ||
         !strcmp(verification.failure, "log_truncated") ||
         !strcmp(verification.failure, "entries_forged"))) reason = verification.failure;
    if (!reason && verification.failure &&
        (strstr(verification.failure, "snapshot_") || !strcmp(verification.failure, "chain_byte_limit"))) {
        report->failed = 1; report->error = verification.failure; goto finish;
    }
    char old_path[PATH_MAX]; int old_present = 0;
    sigil_chain_scan_t old = {0}; old.first_fragment_offset = -1;
    if (sigil_path_suffix(old_path, sizeof old_path, chain_path(), ".v1") ||
        sigil_recovery_input(old_path, &old_present) ||
        (old_present && sigil_scan_chain(old_path, &old))) {
        report->failed = 1; report->error = "migration_artifact_unavailable"; goto finish;
    }
    int invalid_old = old_present && old.tag_lines > 0;
    int reset_needed = 0;
    char manifest[PATH_MAX] = {0};
    if (reason) {
        if (g_chain_fp) {
            FILE* current = g_chain_fp; g_chain_fp = NULL;
            if (fclose(current)) { report->failed = 1; report->error = "active_close_failed"; goto finish; }
        }
        int outcome;
        const char* stage;
        const char* target = chain_path();
        if (!strcmp(reason, "orphaned_ratchet_without_chain")) {
            target = ratchet_path(); stage = "quarantine-orphan-witness";
            outcome = quarantine_move_orphan_ratchet(reason, manifest, sizeof manifest);
            reset_needed = outcome == 0 || outcome == 1;
        } else if (!strcmp(reason, "corrupted_newline_fragment")) {
            stage = "quarantine-tail";
            outcome = active.first_fragment_offset >= 0 && !active.entries_after_fragment ?
                quarantine_tail_suffix(chain_path(), reason, active.s7_entries, active.fragments,
                                       active.first_fragment_offset, manifest, sizeof manifest) : -1;
            report->tail_salvaged = outcome == 0 || outcome == 1;
        } else {
            stage = "quarantine-active-pair";
            outcome = quarantine_move_one(chain_path(), reason, active.s7_entries, active.fragments,
                                          1, manifest, sizeof manifest);
            reset_needed = outcome == 0 || outcome == 1;
        }
        if (outcome) report->error = outcome == 2 ? "recovery_pair_split" : outcome < 0 ? "recovery_group_failed" : "recovery_group_not_durable";
        if (sigil_recovery_event(report, callback, context, stage, target, manifest, reason,
                                 outcome, active.s7_entries, active.fragments,
                                 report->tail_salvaged ? active.first_fragment_offset : -1)) goto finish;
        /* Outcome 1 means the active bytes already changed. Complete the
         * corresponding reset/rescan below while retaining failed/error; a
         * failed receipt still stops before fresh-pair publication. */
        if (outcome != 0 && outcome != 1) goto finish;
    }
    if (reset_needed) {
        int outcome = sigil_create_fresh_pair();
        report->active_reset = !outcome;
        if (outcome) report->error = "fresh_pair_failed";
        if (!outcome) {
            memset(g_prev_hash, 0, sizeof g_prev_hash);
            memset(g_ratchet_key, 0, sizeof g_ratchet_key);
            g_seq = 0; g_ratchet_advances = 0;
            g_append_block_reason = NULL; g_append_block_warned = 0;
            g_active_entries = g_active_fragments = g_active_unattested = 0;
            g_active_malformed = g_active_legacy = 0;
            g_first_fragment_offset = -1; g_entries_after_fragment = 0;
            if (unlink(ckpt_path()) && errno != ENOENT) { report->failed++; report->error = "checkpoint_invalidation_failed"; }
            if (ratchet_load() || !chain_open()) {
                g_append_block_reason = "fresh_pair_reload_failed";
                report->failed++; report->error = g_append_block_reason;
            }
        }
        if (sigil_recovery_event(report, callback, context, "create-fresh-pair", chain_path(), NULL,
                                 reason, outcome, 0, 0, -1)) goto finish;
    } else if (report->tail_salvaged) {
        chain_resume();
        if (!chain_open()) { report->failed++; report->error = "salvaged_pair_reload_failed"; }
    }
    /* Migration cleanup is independent of repairing the active pair. It must
     * neither prevent that repair nor hide a partial recovery failure. */
    if (invalid_old && !report->failed) {
        int outcome = quarantine_move_one(old_path, "invalid_migration_v1",
            old.s7_entries + old.legacy_entries, old.fragments, 0, manifest, sizeof manifest);
        if (outcome >= 0) { g_invalid_v1_artifact = 0; g_v1_entries = g_v1_fragments = 0; }
        if (outcome) report->error = "migration_quarantine_failed";
        if (sigil_recovery_event(report, callback, context, "quarantine-migration", old_path, manifest,
            "invalid_migration_v1", outcome, old.s7_entries + old.legacy_entries, old.fragments, -1)) goto finish;
    }
finish:
    chain_transaction_unlock(transaction);
    return report->delivery_failed ? 125 : report->failed ? 1 : 0;
}

static int cmd_quarantine_apply(jmod_writer_t writer, void* context) {
    sigil_quarantine_observation_t report;
    int status = sigil_quarantine_apply_observed(&report, NULL, NULL);
    char chain[PATH_MAX * 2], ratchet[PATH_MAX * 2];
    jash_json_esc(chain_path(), chain, sizeof chain);
    jash_json_esc(ratchet_path(), ratchet, sizeof ratchet);
    /* Historical applied counts recovery groups, excluding fresh creation. */
    int recovered = report.applied - report.active_reset;
    jash_writef(writer, context,
        "{\"applied\":%d,\"failed\":%d,\"active_reset\":%s,\"tail_salvaged\":%s,"
        "\"chain\":\"%s\",\"ratchet\":\"%s\"}\n",
        recovered, report.failed, report.active_reset ? "true" : "false",
        report.tail_salvaged ? "true" : "false", chain, ratchet);
    return status;
}
