/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* One verifier observation feeds recovery, native values and the historical
 * JSON renderer. In particular, destructive recovery never inspects rendered
 * JSON to decide whether a failure is conclusive. */
typedef enum {
    SIGIL_VERIFY_SUCCESS,
    SIGIL_VERIFY_PRECONDITION,
    SIGIL_VERIFY_ARTIFACT,
    SIGIL_VERIFY_STRUCTURAL,
    SIGIL_VERIFY_WALK,
    SIGIL_VERIFY_WITNESS
} sigil_verify_kind_t;

typedef struct {
    sigil_verify_kind_t kind;
    int valid;
    int error_number;
    const char* failure;
    uint64_t at_sequence;
    uint64_t entries_verified, entries_unattested, entries_detected, fragments;
    uint64_t sequence_restarts, ratchet_advances, witness_entries, boundaries, missing;
    const char* witness_scope;
    int range_requested;
    uint64_t range_from, range_to, range_verified;
    uint64_t source_bytes;
} sigil_verify_observation_t;

static int sigil_observed_uint(const char* text, uint64_t* value, const char** end) {
    if (*text < '0' || *text > '9') return -1;
    errno = 0; char* tail; unsigned long long n = strtoull(text, &tail, 10);
    if (errno) return -1;
    *value = (uint64_t)n; *end = tail; return 0;
}

static int sigil_verify_observe_range(sigil_verify_observation_t* result,
        int held_transaction_fd, int has_range, uint64_t range_from, uint64_t range_to) {
    memset(result, 0, sizeof *result);
    result->kind = SIGIL_VERIFY_PRECONDITION;
    result->range_requested = has_range;
    result->range_from = range_from; result->range_to = range_to;
    result->witness_scope = "unavailable";
    if (g_invalid_v1_artifact) {
        result->kind = SIGIL_VERIFY_ARTIFACT;
        result->failure = "invalid_migration_v1";
        result->entries_detected = g_v1_entries; result->fragments = g_v1_fragments;
        return 1;
    }
    if (g_append_block_reason &&
        (!strcmp(g_append_block_reason, "legacy_global_ratchet_scope_mismatch") ||
         !strcmp(g_append_block_reason, "missing_scoped_ratchet") ||
         !strcmp(g_append_block_reason, "malformed_ratchet") ||
         !strcmp(g_append_block_reason, "orphaned_ratchet_without_chain"))) {
        result->failure = g_append_block_reason; return 1;
    }
    struct stat named;
    if (lstat(chain_path(), &named)) {
        result->error_number = errno;
        result->failure = errno == ENOENT ? "no_chain_file" : "snapshot_unavailable";
        return 1;
    }
    if (!S_ISREG(named.st_mode) || named.st_size < 0 || (uint64_t)named.st_size > 32u * 1024u * 1024u) {
        result->failure = S_ISREG(named.st_mode) ? "chain_byte_limit" : "nonregular_chain";
        result->error_number = S_ISREG(named.st_mode) ? EFBIG : EINVAL;
        return 1;
    }
    int transaction_fd = -1;
    FILE* stream = held_transaction_fd >= 0 ? sigil_verify_snapshot_open_locked()
                                          : sigil_verify_snapshot_open(&transaction_fd);
    if (!stream) { result->failure = "snapshot_unavailable"; result->error_number = errno; return 1; }
    struct stat before = {0}, after;
    if (fstat(fileno(stream), &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > 32u * 1024u * 1024u) {
        result->failure = "snapshot_unavailable"; result->error_number = errno ? errno : EINVAL;
        goto finish;
    }
    result->source_bytes = (uint64_t)before.st_size;
    sigil_chain_scan_t scan;
    memset(&scan, 0, sizeof scan); scan.first_fragment_offset = -1;
    const char* recovery = sigil_scan_chain(chain_path(), &scan) == 0
        ? sigil_scan_recovery_reason(&scan) : "snapshot_unavailable";
    if (recovery) {
        result->kind = SIGIL_VERIFY_STRUCTURAL; result->failure = recovery;
        result->entries_detected = scan.s7_entries; result->fragments = scan.fragments;
        goto finish;
    }
    result->kind = SIGIL_VERIFY_WALK;
    uint8_t running_hash[32] = {0};
    uint64_t expected_sequence = 0;
    int base_set = 0;
    sigil_boundary_t boundary = {0};
    char* line = NULL; size_t capacity = 0; ssize_t size;
    while ((size = getline(&line, &capacity, stream)) >= 0) {
        if (!size || size >= SIGIL_LINE_MAX || line[size - 1] != '\n' || memchr(line, 0, (size_t)size)) {
            result->failure = "malformed_line"; result->at_sequence = result->entries_verified + 1; break;
        }
        if (sigil_line_is_unattested_log(line)) { result->entries_unattested++; continue; }
        uint64_t sequence, seconds, nanos;
        const char *after_sequence, *after_seconds, *after_nanos;
        if (sigil_observed_uint(line, &sequence, &after_sequence) || *after_sequence != ' ' ||
            sigil_observed_uint(after_sequence + 1, &seconds, &after_seconds) || *after_seconds != '.' ||
            sigil_observed_uint(after_seconds + 1, &nanos, &after_nanos) || *after_nanos != ' ' ||
            nanos >= UINT64_C(1000000000) || seconds > (UINT64_MAX - nanos) / UINT64_C(1000000000) ||
            strlen(after_nanos + 1) < 65 || after_nanos[65] != ' ') {
            result->failure = "malformed_line"; result->at_sequence = result->entries_verified + 1; break;
        }
        char hash[65], tag[65];
        memcpy(hash, after_nanos + 1, 64); hash[64] = 0;
        const char* tag_field = strstr(after_nanos + 65, " tag=");
        const char* rc_field = strstr(after_nanos + 65, " rc=");
        const char* command_field = strstr(after_nanos + 65, " cmd=");
        int after_tag = 0;
        errno = 0; char* after_rc = NULL;
        long rc = rc_field ? strtol(rc_field + 4, &after_rc, 10) : 0;
        if (!sigil_hex64(hash) || !tag_field || !rc_field || !command_field ||
            tag_field >= rc_field || rc_field >= command_field ||
            sscanf(tag_field, " tag=%64s%n", tag, &after_tag) != 1 || !sigil_hex64(tag) ||
            (tag_field[after_tag] != ' ' && tag_field[after_tag] != '\t') ||
            errno || after_rc == rc_field + 4 || *after_rc != ' ' || rc < INT_MIN || rc > INT_MAX) {
            result->failure = "malformed_line"; result->at_sequence = sequence; break;
        }
        if (!base_set) { expected_sequence = sequence; base_set = 1; }
        if (sequence < expected_sequence) {
            if (sequence <= 1) result->sequence_restarts++;
            else { result->failure = "seq_rollback"; result->at_sequence = sequence; break; }
        }
        if (sequence == UINT64_MAX) { result->failure = "sequence_limit"; result->at_sequence = sequence; break; }
        uint64_t timestamp = seconds * UINT64_C(1000000000) + nanos;
        const char* command = command_field + 5;
        size_t command_length = strlen(command);
        if (command_length && command[command_length - 1] == '\n') command_length--;
        uint8_t payload[SIGIL_PAYLOAD_MAX]; size_t used = 0;
        for (int i = 7; i >= 0; i--) payload[used++] = (uint8_t)(timestamp >> (i * 8));
        uint32_t exit_code = (uint32_t)(int32_t)rc;
        for (unsigned i = 0; i < 4; i++) payload[used++] = (uint8_t)(exit_code >> (i * 8));
        if (command_length > SIGIL_PAYLOAD_CMD_MAX(used)) {
            result->failure = "command_byte_limit"; result->at_sequence = sequence; break;
        }
        memcpy(payload + used, command, command_length); used += command_length;
        uint8_t digest[32]; char expected[65];
        jash_blake3_chain(running_hash, payload, used, digest);
        memcpy(running_hash, digest, sizeof running_hash); jash_blake3_hex(digest, expected);
        if (strcmp(expected, hash)) { result->failure = "hash_mismatch"; result->at_sequence = sequence; break; }
        expected_sequence = sequence + 1;
        result->entries_verified++;
        if (has_range && sequence >= range_from && sequence <= range_to) result->range_verified++;
        sigil_boundary_track(&boundary, command);
    }
    free(line);
    if (ferror(stream) && !result->failure) {
        result->failure = "snapshot_read_failed"; result->error_number = errno ? errno : EIO;
    }
    result->ratchet_advances = g_ratchet_advances;
    result->boundaries = boundary.bnd_count;
    result->witness_entries = sigil_witness_entries(&boundary, result->entries_verified, &result->witness_scope);
    if (!result->failure) {
        if (g_ratchet_advances > result->witness_entries) {
            result->failure = "log_truncated";
            result->missing = g_ratchet_advances - result->witness_entries;
        } else if (result->witness_entries > g_ratchet_advances) {
            result->failure = "entries_forged";
            result->missing = result->witness_entries - g_ratchet_advances;
        }
        if (result->failure) result->kind = SIGIL_VERIFY_WITNESS;
    }
    if (!result->failure && has_range && !result->range_verified) result->failure = "range_has_no_attested_entries";
finish:
    if (fstat(fileno(stream), &after) || lstat(chain_path(), &named)) {
        if (!result->failure) result->failure = "snapshot_unavailable";
        result->error_number = errno;
    } else if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
               before.st_size != after.st_size || before.st_mtime != after.st_mtime ||
               before.st_ctime != after.st_ctime || before.st_dev != named.st_dev || before.st_ino != named.st_ino
#if defined(__linux__)
               || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec
#endif
               ) {
        result->failure = "snapshot_changed"; result->error_number = ESTALE;
    }
    if (flock(fileno(stream), LOCK_UN) && !result->failure) {
        result->failure = "snapshot_unlock_failed"; result->error_number = errno;
    }
    if (fclose(stream) && !result->failure) {
        result->failure = "snapshot_close_failed"; result->error_number = errno;
    }
    if (held_transaction_fd < 0) chain_transaction_unlock(transaction_fd);
    result->valid = !result->failure;
    if (result->valid) result->kind = SIGIL_VERIFY_SUCCESS;
    return result->valid ? 0 : 1;
}

static int sigil_verify_observe(sigil_verify_observation_t* result, int held_transaction_fd) {
    return sigil_verify_observe_range(result, held_transaction_fd, 0, 0, 0);
}
