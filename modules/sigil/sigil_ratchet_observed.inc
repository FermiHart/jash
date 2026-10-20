/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
typedef struct {
    int from_shared, adopt, changed, delivery_failed;
    int applied_groups, failed_groups;
    int boundary_sealed, outgoing_published, retired_published, replacement_published, sidecar_written;
    uint64_t boundary_sequence, previous_entries, previous_advances;
    const char* mode;
    const char* error;
    char retired_witness[PATH_MAX], sidecar[PATH_MAX], nonce[33];
} sigil_ratchet_observation_t;

static int sigil_random_exact(uint8_t* bytes, size_t length) {
    int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    size_t offset = 0; int error = 0;
    while (offset < length) {
        ssize_t count = read(descriptor, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (size_t)count;
    }
    if (close(descriptor) && !error) error = errno;
    if (error) { memset(bytes, 0, length); errno = error; return -1; }
    return 0;
}

static int sigil_ratchet_event(sigil_ratchet_observation_t* report,
        sigil_transition_callback_t callback, void* context,
        const char* stage, const char* path, const char* auxiliary,
        int applied, int durable, int outcome) {
    sigil_transition_event_t event = {
        .stage = stage, .path = path, .auxiliary = auxiliary, .reason = outcome ? report->error : NULL,
        .scope = "pinned-ledger-ratchet-publication-group", .applied = applied, .durable = durable,
        .outcome = outcome, .error_number = outcome ? errno : 0, .has_sequence = report->boundary_sealed,
        .sequence = report->boundary_sequence, .entries = report->previous_entries, .retained_offset = -1
    };
    if (applied > 0) report->applied_groups++;
    if (outcome) report->failed_groups++;
    if (callback && callback(context, &event)) { report->delivery_failed = 1; return -1; }
    return 0;
}

static long sigil_sidecar_sink(void* context, const char* bytes, size_t length) {
    FILE* file = context;
    size_t written = fwrite(bytes, 1, length, file);
    return written == length ? (long)written : -1;
}

static int sigil_retirement_sidecar(sigil_ratchet_observation_t* report, const char* source,
                                    int* attempted) {
    *attempted = 0;
    const char* fault = getenv("JASH_SIGIL_TEST_RATCHET_FAIL");
    if (fault && !strcmp(fault, "sidecar-open")) { errno = EIO; return -1; }
    int descriptor = open(report->sidecar, O_WRONLY | O_APPEND | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) return -1;
    /* O_CREAT may already have changed the namespace before a later metadata,
     * lock or stdio failure. Such a failure cannot claim no effect. */
    *attempted = 1;
    struct stat metadata;
    if (fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        metadata.st_nlink != 1 || (metadata.st_mode & 077)) {
        int error = errno ? errno : EINVAL; close(descriptor); errno = error; return -1;
    }
    if (flock(descriptor, LOCK_EX)) {
        int error = errno; close(descriptor); errno = error; return -1;
    }
    FILE* file = fdopen(descriptor, "a");
    if (!file) { int error = errno; close(descriptor); errno = error; return -1; }
    jash_json_writer_t json; jash_jsonw_init(&json, sigil_sidecar_sink, file);
#define SR_STRING(key, value) do { jash_jsonw_raw(&json, "\"" key "\":"); jash_jsonw_string_or_bytes(&json, value); } while (0)
#define SR_UINT(key, value) do { jash_jsonw_raw(&json, "\"" key "\":"); jash_jsonw_u64(&json, value); } while (0)
    jash_jsonw_raw(&json, "{"); SR_UINT("retired_at_ns", now_ns());
    jash_jsonw_raw(&json, ","); SR_STRING("mode", report->mode);
    jash_jsonw_raw(&json, ","); SR_UINT("boundary_seq", report->boundary_sequence);
    jash_jsonw_raw(&json, ","); SR_UINT("segment_entries", report->previous_entries);
    jash_jsonw_raw(&json, ","); SR_UINT("prev_advances", report->previous_advances);
    jash_jsonw_raw(&json, ","); SR_STRING("prev_advances_scope", report->from_shared ? "shared-aggregate-across-repos" : "repo-local");
    jash_jsonw_raw(&json, ","); SR_STRING("source_key_path", source);
    jash_jsonw_raw(&json, ","); SR_STRING("source_key_scope", report->from_shared ? "shared-global" : "repo-scoped");
    jash_jsonw_raw(&json, ","); SR_STRING("retired_witness_path", report->retired_witness);
    jash_jsonw_raw(&json, ","); SR_STRING("segment_claims", "hash-chained");
    jash_jsonw_raw(&json, ","); SR_STRING("segment_does_not_claim", "position-witness");
    jash_jsonw_raw(&json, ","); SR_STRING("exclusivity", report->adopt ?
        "weaker: new key is derivable from the retired key" : "exclusive: new key is unrelated to the retired key");
    jash_jsonw_raw(&json, "}\n");
#undef SR_STRING
#undef SR_UINT
    int error = jash_jsonw_finish(&json) || fflush(file) || ferror(file) || fsync(descriptor);
    if (fault && !strcmp(fault, "sidecar-fsync")) error = 1;
    if (fclose(file)) error = 1;
    if (!error && sigil_fsync_parent(report->sidecar)) error = 1;
    if (error) { errno = EIO; return -1; }
    return 0;
}

static int sigil_ratchet_transition_observed(sigil_ratchet_observation_t* report, int adopt,
        sigil_transition_callback_t callback, void* context) {
    memset(report, 0, sizeof *report); report->adopt = adopt;
    const char* scoped = ratchet_path(); const char* legacy = legacy_ratchet_path();
    const char* source = scoped;
    uint8_t nonce[16] = {0}, old_key[32] = {0}, new_key[32] = {0};
    char old_nonce[33] = {0}; uint64_t advances = 0;
    int transaction = -1; FILE* active = NULL;
    if (sigil_random_exact(nonce, sizeof nonce)) { report->error = "boundary_entropy_unavailable"; goto finish; }
    hexenc(nonce, sizeof nonce, report->nonce);
    transaction = chain_transaction_lock();
    if (transaction < 0) { report->error = "transaction_lock_failed"; goto finish; }
    int scoped_present, chain_present, source_present;
    if (sigil_recovery_input(scoped, &scoped_present) || sigil_recovery_input(chain_path(), &chain_present)) {
        report->error = "ratchet_input_unavailable"; goto finish;
    }
    if (!chain_present) { report->error = "no_chain_file"; goto finish; }
    report->from_shared = strcmp(scoped, legacy) && !scoped_present;
    report->mode = adopt ? "adopt" : report->from_shared ? "rekey" : "rotate";
    source = report->from_shared ? legacy : scoped;
    if (adopt && !report->from_shared) { report->error = "already_repo_scoped"; goto finish; }
    if (sigil_recovery_input(source, &source_present) || !source_present ||
        ratchet_read_at(source, old_key, &advances, old_nonce)) {
        report->error = "source_ratchet_unavailable"; goto finish;
    }
    if (!report->from_shared) {
        sigil_verify_observation_t verification;
        if (sigil_verify_observe(&verification, transaction)) {
            report->error = verification.failure ? verification.failure : "chain_verification_failed"; goto finish;
        }
    }
    sigil_chain_scan_t scan;
    if (sigil_scan_chain(chain_path(), &scan) || scan.malformed || scan.fragments || scan.legacy_entries) {
        report->error = "chain_scan_failed"; goto finish;
    }
    report->previous_entries = scan.s7_entries;
    report->previous_advances = advances;
    if (advances == UINT64_MAX || (scan.s7_entries && scan.last_seq == UINT64_MAX)) {
        report->error = "ratchet_sequence_limit"; goto finish;
    }
    if (snprintf(report->retired_witness, sizeof report->retired_witness,
                 "%s/retired-ratchet-%s-%s.key", g_ledger.jash_dir, g_ledger.repo_key, report->nonce) >= (int)sizeof report->retired_witness ||
        snprintf(report->sidecar, sizeof report->sidecar, "%s/retired-%s.jsonl", g_ledger.jash_dir, g_ledger.repo_key) >= (int)sizeof report->sidecar) {
        report->error = "retirement_path_limit"; goto finish;
    }
    if (!adopt && sigil_random_exact(new_key, sizeof new_key)) { report->error = "replacement_entropy_unavailable"; goto finish; }
    char marker[512];
    int marker_size = snprintf(marker, sizeof marker, SIGIL_BOUNDARY_MARK
        "mode=%s lineage=%s prev_advances=%llu prev_entries=%llu nonce=%s", report->mode,
        adopt ? "derived-from-shared" : "fresh", (unsigned long long)advances,
        (unsigned long long)report->previous_entries, report->nonce);
    if (marker_size < 0 || (size_t)marker_size >= sizeof marker) { report->error = "boundary_record_limit"; goto finish; }
    active = chain_lock_active_held();
    if (!active) { report->error = "active_chain_lock_failed"; goto finish; }
    if (fflush(active) || ferror(active)) { report->error = "active_chain_flush_failed"; goto finish; }
    /* The locked full scan supplies the exact anchor. Do not fall back to a
     * process-local tip after an unobserved tail-read error. */
    if (scan.s7_entries) {
        unsigned long long sequence = 0; char timestamp[32], hash[65];
        if (sscanf(scan.last_s7, "%llu %31s %64s", &sequence, timestamp, hash) != 3 ||
            !sigil_hex64(hash) || sequence == ULLONG_MAX || hexdec(hash, g_prev_hash, 32)) {
            report->error = "chain_anchor_unavailable"; goto finish;
        }
        g_seq = (uint64_t)sequence + 1;
    } else { g_seq = 0; memset(g_prev_hash, 0, sizeof g_prev_hash); }
    report->boundary_sequence = g_seq;
    chain_write_entry(active, marker, 0, old_key, &advances);
    if (fflush(active) || ferror(active) || fsync(fileno(active))) {
        report->error = "boundary_sync_failed"; g_append_block_reason = report->error;
        (void)sigil_ratchet_event(report, callback, context, "seal-boundary", chain_path(), source, -1, 0, 1);
        goto finish;
    }
    report->boundary_sealed = 1; report->changed = 1; g_seq++;
    if (adopt) jash_blake3_keyed(old_key, (const uint8_t*)g_ledger.repo_key, strlen(g_ledger.repo_key), new_key);
    const char* fault = getenv("JASH_SIGIL_TEST_RATCHET_FAIL");
    int injected = fault && !strcmp(fault, "outgoing");
    if (injected || ratchet_write_at(source, old_key, advances, old_nonce)) {
        report->error = "outgoing_witness_publish_failed"; g_append_block_reason = report->error;
        if (injected) errno = EIO;
        (void)sigil_ratchet_event(report, callback, context, "seal-boundary-and-advance-witness", chain_path(), source, 1, 0, 1);
        goto finish;
    }
    report->outgoing_published = 1;
    /* The first receipt covers the consistency group: a durable boundary and
     * its advanced outgoing witness. There is no new-key publication yet. */
    if (sigil_ratchet_event(report, callback, context, "seal-boundary-and-advance-witness", chain_path(), source, 1, 1, 0)) goto finish;
    injected = fault && !strcmp(fault, "preserve");
    if (injected || ratchet_write_at(report->retired_witness, old_key, advances, old_nonce)) {
        report->error = "retired_witness_publish_failed"; g_append_block_reason = report->error;
        if (injected) errno = EIO;
        (void)sigil_ratchet_event(report, callback, context, "preserve-retired-witness", report->retired_witness, source, injected ? 0 : -1, 0, 1);
        goto finish;
    }
    report->retired_published = 1;
    if (sigil_ratchet_event(report, callback, context, "preserve-retired-witness", report->retired_witness, source, 1, 1, 0)) goto finish;
    injected = fault && !strcmp(fault, "replacement");
    if (injected || ratchet_write_at(scoped, new_key, 0, report->nonce)) {
        report->error = "ratchet_replacement_failed"; g_append_block_reason = report->error;
        if (injected) errno = EIO;
        (void)sigil_ratchet_event(report, callback, context, "publish-replacement-witness", scoped, report->retired_witness, injected ? 0 : -1, 0, 1);
        goto finish;
    }
    report->replacement_published = 1;
    memcpy(g_ratchet_key, new_key, sizeof g_ratchet_key); g_ratchet_advances = 0;
    snprintf(g_ratchet_nonce, sizeof g_ratchet_nonce, "%s", report->nonce);
    g_append_block_reason = NULL; g_append_block_warned = 0;
    if (sigil_ratchet_event(report, callback, context, "publish-replacement-witness", scoped, report->retired_witness, 1, 1, 0)) goto finish;
    int attempted = 0;
    if (sigil_retirement_sidecar(report, source, &attempted)) {
        report->error = "retirement_sidecar_failed";
        (void)sigil_ratchet_event(report, callback, context, "append-retirement-record", report->sidecar, report->retired_witness, attempted ? -1 : 0, 0, 1);
        goto finish;
    }
    report->sidecar_written = 1;
    (void)sigil_ratchet_event(report, callback, context, "append-retirement-record", report->sidecar, report->retired_witness, 1, 1, 0);
finish:
    if (report->error && !report->failed_groups) report->failed_groups = 1;
    if (report->delivery_failed && !report->replacement_published) {
        /* A later append must never reuse a cached pre-boundary witness after
         * a receipt cut. The persisted pair remains available for recovery. */
        g_append_block_reason = "ratchet_transition_delivery_failed"; g_append_block_warned = 0;
    }
    chain_unlock_active(active, transaction);
    memset(old_key, 0, sizeof old_key); memset(new_key, 0, sizeof new_key); memset(nonce, 0, sizeof nonce);
    return report->delivery_failed ? 125 : report->error ? 1 : 0;
}

static int sigil_ratchet_transition(jmod_writer_t writer, void* context, int adopt) {
    sigil_ratchet_observation_t report;
    int status = sigil_ratchet_transition_observed(&report, adopt, NULL, NULL);
    if (status) {
        jash_writef(writer, context, "sigil ratchet: %s%s\n", report.error ? report.error : "transition_failed",
            report.changed ? "; recovery stages may already have applied" :
            adopt ? "; refusing transition" : "; refusing rotation");
        return status;
    }
    jash_writef(writer, context,
        "\033[1;32m✓ lineage retired\033[0m — boundary sealed at seq %llu (mode=%s)\n"
        "  segment before boundary : %llu entries — hash-chained, NO position witness\n"
        "  %s: %llu advances%s\n"
        "  new key                 : %s\n"
        "  record                  : %s\n"
        "%s",
        (unsigned long long)report.boundary_sequence, report.mode,
        (unsigned long long)report.previous_entries,
        report.from_shared ? "shared ratchet         " : "retired repo ratchet   ",
        (unsigned long long)report.previous_advances,
        report.from_shared ? " (aggregate over all repos, kept intact)" : " (this repo only)",
        ratchet_path(), report.sidecar,
        adopt ? "  note: adopt keeps lineage continuity but the new key is\n"
                "        derivable from the shared one — weaker exclusivity.\n"
              : "  note: the retired segment is NOT promoted to verified. It keeps\n"
                "        its hash chain and nothing more, which is the truth.\n");
    return 0;
}
