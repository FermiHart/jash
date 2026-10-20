/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Included after the legacy engines. Receipts come from explicit engine
 * observations; the text renderers are never parsed back into values. */

static jdv_value_t sn_action_value(semantic_builtin_t* call, const char* operation,
        jdv_value_t detail, int applied, int durable, const char* error, int status) {
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.action.v1")),
        semantic_field("operation", semantic_symbol(values, operation)),
        semantic_field("detail", detail),
        semantic_field("applied", applied < 0 ? jdv_value_nothing() : jdv_value_bool(applied)),
        semantic_field("durable", durable < 0 ? jdv_value_nothing() : jdv_value_bool(durable)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("error", semantic_symbol(values, error)),
        semantic_field("exit_code", jdv_value_i64(status)),
        semantic_field("scope", semantic_symbol(values, "pinned-module-ledger")),
        semantic_field("module_initialization_may_have_written", jdv_value_bool(1))
    };
    return semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}

static int sn_action_finish(semantic_builtin_t* call, const char* operation,
        jdv_value_t detail, int applied, int durable, const char* error, int status) {
    return semantic_finish(call, sn_action_value(call, operation, detail,
                            applied, durable, error, status), status);
}

static int sn_proof_publish(semantic_builtin_t* call, int dsse, const char* requested) {
    semantic_value_t* values = &call->values;
    const char* path = requested ? requested : dsse ? sigil_identity_proof_dsse() : sigil_identity_proof();
    const char* operation = dsse ? "proof.dsse" : "proof.emit";
    if (!path || !*path || strlen(path) >= PATH_MAX - 64)
        return semantic_error(call, "invalid_proof_path", 2);
    /* The longest possible receipt is admitted before signing or publishing. */
    char maximum_hash[65]; memset(maximum_hash, 'f', 64); maximum_hash[64] = 0;
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, path)),
        semantic_field("format", semantic_symbol(values, dsse ? "dsse" : "jashproof/1")),
        semantic_field("algorithm", semantic_symbol(values, "ed25519")),
        semantic_field("bytes", jdv_value_filesize(UINT64_MAX)),
        semantic_field("sha256", semantic_text(values, maximum_hash)),
        semantic_field("chain_entries", jdv_value_u64(UINT64_MAX)),
        semantic_field("chain_tip", semantic_text(values, maximum_hash)),
        semantic_field("signer_key", semantic_text(values, maximum_hash)),
        semantic_field("errno", jdv_value_i64(INT_MAX))
    };
    jdv_value_t detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    jdv_value_t preflight = sn_action_value(call, operation, detail, 1, 1, "proof_publication_failed", 125);
    int status = semantic_preflight(call, &preflight);
    if (status) return status;
    const char* error = NULL;
    sigil_publication_t publication = {0};
    sigil_proof_snapshot_t snapshot = {0};
    char artifact[6144], body[2048], digest_hex[65] = {0}, tip[65] = {0}, key[65] = {0};
    int length = 0;
    if (ed_key_load()) error = "signing_key_unavailable";
    else if (proof_snapshot_capture(&snapshot)) error = "chain_snapshot_failed";
    else {
        int body_length = build_proof_body(body, sizeof body, g_ts_start_ns,
                                          now_ns(), snapshot.entries, snapshot.tip);
        if (body_length < 0 || (size_t)body_length >= sizeof body) error = "proof_body_limit";
        else {
            uint8_t signature[ED25519_SIG];
            uint8_t pae[2560];
            int signed_length = dsse ? dsse_pae(DSSE_PAYLOAD_TYPE, body, (size_t)body_length,
                                               pae, sizeof pae) : body_length;
            if (signed_length < 0 || ed25519_sign(signature, dsse ? pae : (uint8_t*)body,
                                                  (uint32_t)signed_length, g_ed_sk)) error = "proof_signing_failed";
            else if (dsse) {
                char payload[4096], signature64[128];
                b64enc((const uint8_t*)body, (size_t)body_length, payload);
                b64enc(signature, sizeof signature, signature64); hexenc(g_ed_pk, sizeof g_ed_pk, key);
                length = snprintf(artifact, sizeof artifact,
                    "{\"payloadType\":\"%s\",\"payload\":\"%s\",\"signatures\":[{\"keyid\":\"%s\",\"sig\":\"%s\"}]}\n",
                    DSSE_PAYLOAD_TYPE, payload, key, signature64);
            } else {
                char signature_hex[2 * ED25519_SIG + 1]; hexenc(signature, sizeof signature, signature_hex);
                hexenc(g_ed_pk, sizeof g_ed_pk, key);
                length = snprintf(artifact, sizeof artifact, "%ssig: %s\n", body, signature_hex);
            }
        }
    }
    if (!error && (length < 0 || (size_t)length >= sizeof artifact)) error = "proof_artifact_limit";
    if (!error) {
        uint8_t digest[32]; jash_sha256((uint8_t*)artifact, (size_t)length, digest);
        jash_sha256_hex(digest, digest_hex); jash_blake3_hex(snapshot.tip, tip);
        if (proof_publish_observed(path, artifact, (size_t)length, &publication)) error = "proof_publication_failed";
    }
    fields[3].value = length > 0 ? jdv_value_filesize((uint64_t)length) : jdv_value_nothing();
    fields[4].value = semantic_text(values, digest_hex[0] ? digest_hex : NULL);
    fields[5].value = tip[0] ? jdv_value_u64(snapshot.entries) : jdv_value_nothing();
    fields[6].value = semantic_text(values, tip[0] ? tip : NULL);
    fields[7].value = semantic_text(values, key[0] ? key : NULL);
    fields[8].value = publication.error_number ? jdv_value_i64(publication.error_number) : jdv_value_nothing();
    detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    return sn_action_finish(call, operation, detail, publication.published, publication.durable,
                            error, error ? 1 : 0);
}

static int sn_record(semantic_builtin_t* call, const char* label) {
    /* Escaping can expand a source byte twice. Refuse truncation
     * before the real chain writer, preserving the whole supplied label. */
    size_t encoded = 0;
    for (const unsigned char* p = (const unsigned char*)label; *p; p++) {
        size_t n = *p == '\\' || *p == '\n' || *p == '\r' ? 2 : 1;
        if (n > SIGIL_PAYLOAD_CMD_MAX(12) - encoded) return semantic_error(call, "record_label_limit", 2);
        encoded += n;
    }
    if (!encoded) return semantic_error(call, "empty_record_label", 2);
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("label", semantic_text(values, label)),
        semantic_field("sequence", jdv_value_u64(UINT64_MAX)),
        semantic_field("chain", semantic_path(values, chain_path())),
        semantic_field("write_attempted", jdv_value_bool(1)),
        semantic_field("chain_synced", jdv_value_bool(1)),
        semantic_field("ratchet_published", jdv_value_bool(1))
    };
    jdv_value_t detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    jdv_value_t preflight = sn_action_value(call, "record", detail, -1, -1, "ratchet_position_mismatch", 125);
    int status = semantic_preflight(call, &preflight); if (status) return status;
    sigil_record_observation_t observation;
    status = chain_append_observed(label, 0, &observation);
    fields[1].value = observation.write_attempted ? jdv_value_u64(observation.sequence) : jdv_value_nothing();
    fields[3].value = jdv_value_bool(observation.write_attempted);
    fields[4].value = jdv_value_bool(observation.chain_synced);
    fields[5].value = jdv_value_bool(observation.ratchet_published);
    return sn_action_finish(call, "record", semantic_record(values, fields, sizeof fields / sizeof fields[0]),
        observation.chain_synced ? 1 : observation.write_attempted ? -1 : 0,
        observation.chain_synced && observation.ratchet_published,
        status ? g_append_block_reason ? g_append_block_reason : "chain_append_failed" : NULL, status);
}

static int sn_exec_child(char* const* argv, pid_t* child_pid) {
    pid_t pid = fork(); *child_pid = pid > 0 ? pid : 0;
    if (pid < 0) return 125;
    if (!pid) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(125);
        execvp(argv[0], argv); _exit(errno == ENOENT ? 127 : 126);
    }
    int status; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited < 0 ? 125 : WIFEXITED(status) ? WEXITSTATUS(status) :
        WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}

static int sn_export(semantic_builtin_t* call, int argc, char** argv, const char* push_reference) {
    const char* directory = NULL; const char* manifest = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest") && !manifest && i + 1 < argc) manifest = argv[++i];
        else if (argv[i][0] == '-' || directory) return semantic_error(call, "invalid_export_arguments", 2);
        else directory = argv[i];
    }
    if (push_reference && !sigil_registry_ref_valid(push_reference)) return semantic_error(call, "invalid_registry_reference", 2);
    char default_path[PATH_MAX];
    if (!directory) {
        int n = snprintf(default_path, sizeof default_path, "%s/oci-export", sigil_identity_jash_dir());
        if (n < 0 || (size_t)n >= sizeof default_path) return semantic_error(call, "export_path_limit", 2);
        directory = default_path;
    }
    if (strlen(directory) >= PATH_MAX - 80 || (manifest && strlen(manifest) >= PATH_MAX))
        return semantic_error(call, "export_path_limit", 2);
    semantic_value_t* values = &call->values;
    const char* operation = push_reference ? "push" : "export";
    char hash[65]; memset(hash, 'f', 64); hash[64] = 0;
    jdv_record_input_t fields[] = {
        semantic_field("directory", semantic_path(values, directory)),
        semantic_field("caller_manifest", semantic_path(values, manifest)),
        semantic_field("chain_sha256", semantic_text(values, hash)),
        semantic_field("manifest_sha256", semantic_text(values, hash)),
        semantic_field("chain_entries", jdv_value_u64(UINT64_MAX)),
        semantic_field("chain_bytes", jdv_value_filesize(UINT64_MAX)),
        semantic_field("layout_written", jdv_value_bool(1)),
        semantic_field("reference", semantic_text(values, push_reference)),
        semantic_field("child_pid", jdv_value_i64(INT_MAX)),
        semantic_field("child_exit_code", jdv_value_i64(255)),
        semantic_field("remote_state_verified", jdv_value_bool(0)),
        semantic_field("child_output", semantic_symbol(values, "stderr"))
    };
    jdv_value_t detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    jdv_value_t preflight = sn_action_value(call, operation, detail, -1, -1, "export_preparation_failed", 125);
    int status = semantic_preflight(call, &preflight); if (status) return status;
    char* operands[] = {(char*)directory, "--manifest", (char*)manifest};
    char* joined = jash_argv_join(manifest ? 3 : 1, operands);
    if (!joined) return semantic_error(call, "allocation_failed", 125);
    sigil_export_observation_t observation;
    status = cmd_export_observed(sigil_discard_writer, NULL, joined, &observation);
    free(joined);
    pid_t child = 0; int child_status = -1;
    const char* error = observation.error;
    if (!status && push_reference) {
        char reference[320]; int n = snprintf(reference, sizeof reference, "%s:latest", push_reference);
        if (n < 0 || (size_t)n >= sizeof reference) { status = 2; error = "registry_reference_limit"; }
        else {
            char* command[] = {"oras", "push", reference, "--layout", observation.directory, NULL};
            child_status = sn_exec_child(command, &child);
            if (child_status) { status = child_status; error = "push_child_failed"; }
        }
    }
    fields[2].value = semantic_text(values, observation.chain_sha256[0] ? observation.chain_sha256 : NULL);
    fields[3].value = semantic_text(values, observation.manifest_sha256[0] ? observation.manifest_sha256 : NULL);
    fields[4].value = observation.chain_sha256[0] ? jdv_value_u64(observation.chain_entries) : jdv_value_nothing();
    fields[5].value = observation.chain_sha256[0] ? jdv_value_filesize(observation.chain_bytes) : jdv_value_nothing();
    fields[6].value = jdv_value_bool(observation.layout_written);
    fields[8].value = child ? jdv_value_i64(child) : jdv_value_nothing();
    fields[9].value = child_status >= 0 ? jdv_value_i64(child_status) : jdv_value_nothing();
    detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    return sn_action_finish(call, operation, detail, observation.layout_written ? 1 :
        observation.effects_started ? -1 : 0, -1, error, status);
}

/* More than one replay action requires a receipt boundary after each child.
 * The host opens this module session with a bounded multi-record allowance. */
typedef struct {
    uint64_t sequence, timestamp_ns;
    int exit_code, found;
    char command[SIGIL_CMD_ESC_MAX];
    char hash[65];
} sn_entry_t;

static int sn_unsigned(const char* text, uint64_t* value, const char** tail) {
    if (!text || *text < '0' || *text > '9') return -1;
    errno = 0; char* end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || (tail ? 0 : *end)) return -1;
    *value = (uint64_t)parsed; if (tail) *tail = end;
    return 0;
}

static int sn_entry_snapshot(uint64_t first, uint64_t last, sn_entry_t* entries) {
    int transaction = -1;
    FILE* lock = sigil_verify_snapshot_open(&transaction);
    if (!lock) return -1;
    uint8_t* bytes = NULL; long length = read_file_alloc(chain_path(), &bytes);
    int error = length < 0 || (length > 0 && (memchr(bytes, 0, (size_t)length) || bytes[length - 1] != '\n'));
    if (!error) {
        bytes[length] = 0;
        char* current = (char*)bytes;
        while (*current && !error) {
            char* newline = strchr(current, '\n');
            if (!newline || (size_t)(newline - current) >= SIGIL_LINE_MAX) { error = 1; break; }
            *newline = 0;
            /* Fallback LOG lines are outside the attested sequence space. */
            if (sigil_line_is_unattested_log(current)) { current = newline + 1; continue; }
            uint64_t sequence; const char* after_sequence;
            if (sn_unsigned(current, &sequence, &after_sequence) || *after_sequence != ' ') { error = 1; break; }
            if (sequence >= first && sequence <= last) {
                sn_entry_t* entry = &entries[sequence - first];
                uint64_t seconds, nanos; const char *after_seconds, *after_nanos;
                const char* rc = strstr(current, " rc=");
                const char* command = strstr(current, " cmd=");
                if (entry->found || !rc || !command || rc > command ||
                    sn_unsigned(after_sequence + 1, &seconds, &after_seconds) || *after_seconds != '.' ||
                    sn_unsigned(after_seconds + 1, &nanos, &after_nanos) || *after_nanos != ' ' ||
                    nanos >= UINT64_C(1000000000) || seconds > (UINT64_MAX - nanos) / UINT64_C(1000000000) ||
                    strlen(after_nanos + 1) < 65 || after_nanos[65] != ' ' ||
                    strlen(command + 5) >= sizeof entry->command) { error = 1; break; }
                memcpy(entry->hash, after_nanos + 1, 64); entry->hash[64] = 0;
                errno = 0; char* end; long code = strtol(rc + 4, &end, 10);
                if (errno || end == rc + 4 || *end != ' ' || code < INT_MIN || code > INT_MAX || !sigil_hex64(entry->hash)) {
                    error = 1; break;
                }
                entry->sequence = sequence; entry->timestamp_ns = seconds * UINT64_C(1000000000) + nanos;
                entry->exit_code = (int)code; entry->found = 1;
                sigil_wire_unescape(command + 5, entry->command, sizeof entry->command);
            }
            current = newline + 1;
        }
    }
    free(bytes); sigil_verify_snapshot_close(lock, transaction);
    if (error) { errno = EINVAL; return -1; }
    for (uint64_t i = 0; i <= last - first; i++) if (!entries[i].found) { errno = ENOENT; return -1; }
    return 0;
}

static jdv_value_t sn_entry_value(semantic_builtin_t* call, const sn_entry_t* entry,
                                  int execute, pid_t pid, int actual) {
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("sequence", jdv_value_u64(entry->sequence)),
        semantic_field("timestamp_ns", jdv_value_u64(entry->timestamp_ns)),
        semantic_field("command", semantic_text(values, entry->command)),
        semantic_field("stored_exit_code", jdv_value_i64(entry->exit_code)),
        semantic_field("chain_hash", semantic_text(values, entry->hash)),
        semantic_field("executed", jdv_value_bool(execute && pid > 0)),
        semantic_field("pid", pid > 0 ? jdv_value_i64(pid) : jdv_value_nothing()),
        semantic_field("actual_exit_code", execute ? jdv_value_i64(actual) : jdv_value_nothing()),
        semantic_field("exit_code_matches", execute && pid > 0 ? jdv_value_bool(actual == entry->exit_code) : jdv_value_nothing()),
        semantic_field("comparison_scope", semantic_symbol(values, "exit-code-only; not state or output determinism")),
        semantic_field("execution_shell", execute ? semantic_symbol(values, "/bin/sh") : jdv_value_nothing()),
        semantic_field("child_output", execute ? semantic_symbol(values, "stderr") : jdv_value_nothing())
    };
    return semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}

static int sn_restore(semantic_builtin_t* call, const char* selector) {
    uint64_t sequence;
    if (sn_unsigned(selector, &sequence, NULL)) return semantic_error(call, "invalid_sequence", 2);
    sn_entry_t* entry = calloc(1, sizeof *entry);
    if (!entry) return semantic_error(call, "allocation_failed", 125);
    if (sn_entry_snapshot(sequence, sequence, entry)) {
        free(entry); return semantic_error(call, "chain_entry_unavailable", 1);
    }
    int seal = jash_sigil_get() >= SIGIL_SEAL;
    int chain_status = seal ? cmd_chain_verify_range(sigil_discard_writer, NULL, 0, 0, 0) : -1;
    int timestamp_valid = entry->timestamp_ns <= now_ns() + UINT64_C(2000000000);
    struct stat st;
    errno = 0;
    int proof_present = lstat(sigil_identity_proof(), &st) == 0;
    int proof_lookup_error = proof_present || errno == ENOENT ? 0 : errno;
    int proof_status = -1;
    if (seal && proof_present) proof_status = S_ISREG(st.st_mode) ? jef_verify_proof(sigil_identity_proof()) : 1;
    semantic_value_t* values = &call->values;
    int status = proof_lookup_error || (seal && (chain_status || !timestamp_valid || (proof_present && proof_status))) ? 1 : 0;
    jdv_record_input_t fields[] = {
        semantic_field("entry", sn_entry_value(call, entry, 0, 0, 0)),
        semantic_field("seal_enabled", jdv_value_bool(seal)),
        semantic_field("chain_verified", seal ? jdv_value_bool(!chain_status) : jdv_value_nothing()),
        semantic_field("timestamp_not_future", seal ? jdv_value_bool(timestamp_valid) : jdv_value_nothing()),
        semantic_field("proof_present", proof_lookup_error ? jdv_value_nothing() : jdv_value_bool(proof_present)),
        semantic_field("proof_lookup_errno", jdv_value_u64((uint64_t)proof_lookup_error)),
        semantic_field("proof_signature_valid", seal && proof_present ? jdv_value_bool(!proof_status) : jdv_value_nothing()),
        semantic_field("proof_identity_trusted", jdv_value_nothing()),
        semantic_field("restore_performed", jdv_value_bool(0)),
        semantic_field("operation_scope", semantic_symbol(values, "inspect/propose recorded command; no state restoration or command execution"))
    };
    jdv_value_t detail = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    free(entry);
    return sn_action_finish(call, "restore", detail, 0, -1, status ? "restore_checks_failed" : NULL, status);
}

static int sn_replay(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    int execute = argc && !strcmp(argv[0], "--exec");
    if (execute) { argc--; argv++; }
    uint64_t first, last; const char* tail;
    if (argc != 1 || sn_unsigned(argv[0], &first, &tail)) return semantic_error(call, "invalid_replay_arguments", 2);
    if (!*tail) last = first;
    else if (tail[0] != '.' || tail[1] != '.' || sn_unsigned(tail + 2, &last, NULL)) return semantic_error(call, "invalid_replay_range", 2);
    if (last < first || last - first >= 128) return semantic_error(call, "replay_range_limit", 2);
    size_t count = (size_t)(last - first + 1);
    sn_entry_t* entries = calloc(count, sizeof *entries);
    if (!entries) return semantic_error(call, "allocation_failed", 125);
    if (sn_entry_snapshot(first, last, entries)) { free(entries); return semantic_error(call, "chain_entry_unavailable", 1); }
    /* Validate the full selected command inventory and its maximum receipts
     * before the first child. A small budget never starts a partial batch. */
    jdv_value_t planned[128];
    for (size_t i = 0; i < count; i++) planned[i] = sn_entry_value(call, &entries[i], execute, INT_MAX, 255);
    jdv_record_input_t planned_fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.sigil.replay.v1")),
        semantic_field("results", semantic_list(&call->values, planned, count)),
        semantic_field("complete", jdv_value_bool(1)),
        semantic_field("status", jdv_value_i64(125))
    };
    jdv_value_t plan = semantic_record(&call->values, planned_fields, 4);
    int status = semantic_preflight(call, &plan);
    if (status) { free(entries); return status; }
    if (call->mode == 1) {
        jash_jsonw_raw(&call->output.json, "{\"schema\":\"jash.sigil.replay.v1\",\"results\":[");
        if (jash_jsonw_finish(&call->output.json)) { free(entries); return 125; }
    }
    size_t delivered = 0; uint64_t errors = 0;
    for (size_t i = 0; i < count; i++) {
        pid_t pid = 0; int actual = 0;
        if (execute) {
            char* command[] = {"/bin/sh", "-c", entries[i].command, NULL};
            actual = sn_exec_child(command, &pid);
        }
        int failed = execute && (!pid || actual != entries[i].exit_code);
        jdv_value_t value = sn_entry_value(call, &entries[i], execute, pid, actual);
        if (call->values.error) { status = 125; break; }
        int delivery;
        if (call->mode == 2) delivery = jdv_wire_push_v2_item(call->push, &value, 0);
        else {
            if (i) jash_jsonw_raw(&call->output.json, ",");
            delivery = jdv_json_write(&call->output.json, &value, &call->values.limits) || jash_jsonw_finish(&call->output.json);
        }
        if (delivery) { status = 125; break; }
        delivered++; errors += (unsigned)failed;
        /* No subsequent replay after a failed comparison or child setup. */
        if (failed) { status = 1; break; }
    }
    free(entries);
    if (status == 125) return 125;
    if (call->mode == 2) {
        jdv_stream_end_t outcome, final; (void)jdv_stream_end_init(&outcome);
        outcome.state = status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
        outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
        outcome.producer_status = status; outcome.errors_seen = errors;
        if (jdv_wire_push_v2_end(call->push, &outcome, &final)) return 125;
    } else {
        jash_jsonw_raw(&call->output.json, "],\"complete\":");
        jash_jsonw_raw(&call->output.json, delivered == count ? "true" : "false");
        jash_jsonw_raw(&call->output.json, ",\"status\":"); jash_jsonw_i64(&call->output.json, status);
        jash_jsonw_raw(&call->output.json, "}\n");
    }
    return jash_structured_finish(&call->output, status);
}

static int sn_actions(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    if (!argc) return -1;
    if (!strcmp(argv[0], "record")) return argc == 2 ? sn_record(call, argv[1]) : semantic_error(call, "invalid_record_arguments", 2);
    if (!strcmp(argv[0], "export")) return sn_export(call, argc - 1, argv + 1, NULL);
    if (!strcmp(argv[0], "push")) return argc == 2 ? sn_export(call, 0, NULL, argv[1]) : semantic_error(call, "invalid_push_arguments", 2);
    if (!strcmp(argv[0], "restore")) return argc == 2 ? sn_restore(call, argv[1]) : semantic_error(call, "invalid_restore_arguments", 2);
    if (!strcmp(argv[0], "replay")) return sn_replay(call, state, argc - 1, argv + 1);
    if (!strcmp(argv[0], "proof") && (argc == 1 || !strcmp(argv[1], "emit") || !strcmp(argv[1], "dsse")))
        return argc <= 3 ? sn_proof_publish(call, argc > 1 && !strcmp(argv[1], "dsse"), argc == 3 ? argv[2] : NULL)
                        : semantic_error(call, "invalid_proof_arguments", 2);
    return -1;
}
