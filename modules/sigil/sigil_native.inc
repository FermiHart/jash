/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Optional native-v1 module entry point. The host has already admitted one
 * output session. Source records are read directly; rendered text and legacy
 * JSON are never captured or parsed back into values. */
#include "sigil_actions_native.inc"

#define SN_CHAIN_BYTES (32u * 1024u * 1024u)
#define SN_QUERY_ROWS 256u

static int sn_same_file(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime
#if defined(__linux__)
        && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec
#endif
        ;
}

/* A bounded, stable regular-file observation. Empty files are valid; missing
 * inputs and failed read/close/identity checks remain distinct from empty. */
static int sn_read_regular(const char* path, size_t limit, uint8_t** out, size_t* length) {
    *out = NULL; *length = 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after, named;
    int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_size < 0) error = EINVAL;
    else if ((uint64_t)before.st_size > limit) error = EFBIG;
    size_t size = error ? 0 : (size_t)before.st_size;
    uint8_t* bytes = error ? NULL : malloc(size + 1);
    if (!error && !bytes) error = ENOMEM;
    size_t used = 0;
    while (!error && used <= size) {
        size_t want = size + 1 - used;
        if (want > 65536) want = 65536;
        ssize_t count = read(fd, bytes + used, want);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        used += (size_t)count;
        if (used > size) error = ESTALE;
    }
    if (!error && used != size) error = ESTALE;
    if (!error && (fstat(fd, &after) || lstat(path, &named))) error = errno;
    if (!error && (!sn_same_file(&before, &after) || !sn_same_file(&before, &named))) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { free(bytes); errno = error; return -1; }
    bytes[size] = 0; *out = bytes; *length = size; return 0;
}

static int sn_chain_bytes(uint8_t** bytes, size_t* length, int* present) {
    *bytes = NULL; *length = 0; *present = 0;
    int transaction = chain_transaction_lock();
    if (transaction < 0) return -1;
    int fd = open(chain_path(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int error = 0;
    if (fd < 0) { if (errno != ENOENT) error = errno; }
    else {
        struct stat st, named;
        if (fstat(fd, &st)) error = errno;
        else if (!S_ISREG(st.st_mode)) error = EINVAL;
        else if (flock(fd, LOCK_SH)) error = errno;
        else {
            *present = 1;
            if (lstat(chain_path(), &named)) error = errno;
            else if (!sn_same_file(&st, &named)) error = ESTALE;
            else if (sn_read_regular(chain_path(), SN_CHAIN_BYTES, bytes, length)) error = errno;
            if (!error && (fstat(fd, &st) || lstat(chain_path(), &named))) error = errno;
            if (!error && !sn_same_file(&st, &named)) error = ESTALE;
        }
        if (flock(fd, LOCK_UN) && !error) error = errno;
        if (close(fd) && !error) error = errno;
    }
    chain_transaction_unlock(transaction);
    if (error) { free(*bytes); *bytes = NULL; *length = 0; errno = error; return -1; }
    return 0;
}

static int sn_query_finish(semantic_builtin_t* call, const char* operation,
                           jdv_value_t result, int complete, const char* error, int status) {
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.query.v1")),
        semantic_field("operation", semantic_symbol(values, operation)),
        semantic_field("result", result),
        semantic_field("complete", jdv_value_bool(complete)),
        semantic_field("error", semantic_symbol(values, error)),
        semantic_field("exit_code", jdv_value_i64(status)),
        semantic_field("scope", semantic_symbol(values, "pinned-module-ledger")),
        semantic_field("module_initialization_may_have_written", jdv_value_bool(1))
    };
    jdv_value_t value = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    if (complete || status || values->error) return semantic_finish(call, value, status);
    if (call->mode == 2) {
        if (!call->push || jdv_wire_push_v2_item(call->push, &value, 0)) return 125;
        jdv_stream_end_t outcome, final; (void)jdv_stream_end_init(&outcome);
        outcome.state = JDV_STREAM_INCOMPLETE;
        outcome.completeness = JDV_COMPLETENESS_PARTIAL;
        outcome.truncated = 1;
        outcome.reason = (jdv_slice_t){(const uint8_t*)"page_window", 11};
        if (jdv_wire_push_v2_end(call->push, &outcome, &final)) return 125;
    } else {
        if (jdv_json_write(&call->output.json, &value, &values->limits)) return 125;
        jash_jsonw_raw(&call->output.json, "\n");
    }
    return jash_structured_finish(&call->output, status);
}

typedef struct {
    sn_entry_t entry;
    char tag[80];
    int attested;
    uint64_t physical_line;
} sn_chain_record_t;

/* Parse the canonical stored record, retaining full escaped and decoded
 * command bytes. A parser failure is never silently omitted from a query. */
static int sn_parse_chain_record(char* line, sn_chain_record_t* record) {
    memset(record, 0, sizeof *record);
    const char *after_sequence, *after_seconds, *after_nanos;
    uint64_t seconds, nanos;
    if (sn_unsigned(line, &record->entry.sequence, &after_sequence) || *after_sequence != ' ' ||
        sn_unsigned(after_sequence + 1, &seconds, &after_seconds) || *after_seconds != '.' ||
        sn_unsigned(after_seconds + 1, &nanos, &after_nanos) || *after_nanos != ' ' ||
        nanos >= UINT64_C(1000000000) || seconds > (UINT64_MAX - nanos) / UINT64_C(1000000000)) return -1;
    if (strlen(after_nanos + 1) < 65 || after_nanos[65] != ' ') return -1;
    memcpy(record->entry.hash, after_nanos + 1, 64); record->entry.hash[64] = 0;
    if (!sigil_hex64(record->entry.hash)) return -1;
    const char* tag = strstr(after_nanos + 65, " tag=");
    const char* code = strstr(after_nanos + 65, " rc=");
    const char* command = strstr(after_nanos + 65, " cmd=");
    if (!tag || !code || !command || tag >= code || code >= command) return -1;
    tag += 5; const char* end_tag = strchr(tag, ' ');
    if (!end_tag || end_tag == tag || (size_t)(end_tag - tag) >= sizeof record->tag) return -1;
    memcpy(record->tag, tag, (size_t)(end_tag - tag));
    errno = 0; char* after_code; long rc = strtol(code + 4, &after_code, 10);
    if (errno || after_code == code + 4 || *after_code != ' ' || rc < INT_MIN || rc > INT_MAX) return -1;
    record->attested = !sigil_line_is_unattested_log(line);
    if (record->attested && !sigil_hex64(record->tag)) return -1;
    if (strlen(command + 5) >= sizeof record->entry.command) return -1;
    sigil_wire_unescape(command + 5, record->entry.command, sizeof record->entry.command);
    record->entry.timestamp_ns = seconds * UINT64_C(1000000000) + nanos;
    record->entry.exit_code = (int)rc; record->entry.found = 1;
    return 0;
}

static jdv_value_t sn_chain_record_value(semantic_builtin_t* call, const sn_chain_record_t* record) {
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("sequence", jdv_value_u64(record->entry.sequence)),
        semantic_field("timestamp_ns", jdv_value_u64(record->entry.timestamp_ns)),
        semantic_field("command", semantic_text(values, record->entry.command)),
        semantic_field("stored_exit_code", jdv_value_i64(record->entry.exit_code)),
        semantic_field("chain_hash", semantic_text(values, record->entry.hash)),
        semantic_field("tag", semantic_text(values, record->tag)),
        semantic_field("attested_record", jdv_value_bool(record->attested)),
        semantic_field("physical_line", jdv_value_u64(record->physical_line)),
        semantic_field("verified", jdv_value_bool(0))
    };
    return semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}

static int sn_range(int argc, char** argv, uint64_t* first, uint64_t* last) {
    const char* tail;
    if (argc == 2) return sn_unsigned(argv[0], first, NULL) || sn_unsigned(argv[1], last, NULL) || *last < *first ? -1 : 0;
    if (argc != 1 || sn_unsigned(argv[0], first, &tail) || tail[0] != '.' || tail[1] != '.' ||
        sn_unsigned(tail + 2, last, NULL) || *last < *first) return -1;
    return 0;
}

/* Lists use one complete source snapshot and explicit bounded pages. History
 * excludes closing sentinels; blame matches decoded command bytes. Diff and
 * bisect describe recorded command/exit-code state, never filesystem state. */
static int sn_chain_query(semantic_builtin_t* call, const char* operation, int argc, char** argv) {
    uint64_t limit = !strcmp(operation, "history") ? 20 : SN_QUERY_ROWS, offset = 0;
    uint64_t first = 0, last = UINT64_MAX;
    int tail = !strcmp(operation, "history"), have_range = 0;
    const char* pattern = NULL;
    int diff = !strcmp(operation, "diff"), bisect = !strcmp(operation, "bisect");
    if (diff || bisect) {
        if (argc && !strcmp(argv[0], "--")) { argc--; argv++; }
        if (diff || argc) {
            if (sn_range(argc, argv, &first, &last)) return semantic_error(call, "invalid_sequence_range", 2);
            have_range = 1;
        }
    } else {
        int literal = 0;
        for (int i = 0; i < argc; i++) {
            if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
            if (!literal && (!strcmp(argv[i], "--limit") || !strcmp(argv[i], "--offset") || !strcmp(argv[i], "--tail"))) {
                const char* flag = argv[i]; uint64_t number;
                if (++i == argc || sn_unsigned(argv[i], &number, NULL)) return semantic_error(call, "invalid_page", 2);
                if (!strcmp(flag, "--offset")) offset = number;
                else {
                    if (!number || number > SN_QUERY_ROWS) return semantic_error(call, "page_limit", 2);
                    limit = number;
                    if (!strcmp(flag, "--tail")) tail = 1;
                }
            } else if (!strcmp(operation, "blame") && !pattern && (literal || argv[i][0] != '-')) pattern = argv[i];
            else if (!strcmp(operation, "list") && argc == 1 && !sn_unsigned(argv[i], &limit, NULL) && limit && limit <= SN_QUERY_ROWS) {
                tail = 1;
            } else return semantic_error(call, "invalid_query_arguments", 2);
        }
        if ((!strcmp(operation, "blame") && (!pattern || !*pattern)) || (tail && offset))
            return semantic_error(call, "invalid_query_arguments", 2);
    }
    uint8_t* bytes = NULL; size_t length = 0; int present = 0;
    if (sn_chain_bytes(&bytes, &length, &present))
        return semantic_error(call, errno == EFBIG ? "chain_byte_limit" : "chain_snapshot_failed", 1);
    if ((length && bytes[length - 1] != '\n') || (length && memchr(bytes, 0, length))) {
        free(bytes); return semantic_error(call, "malformed_chain", 1);
    }
    sn_chain_record_t* records = calloc(SN_QUERY_ROWS, sizeof *records);
    if (!records) { free(bytes); return semantic_error(call, "allocation_failed", 125); }
    uint64_t matched = 0, physical = 0, scanned = 0;
    size_t saved = 0; int parse_error = 0, duplicate = 0, found_a = 0, found_b = 0, first_failure = 0;
    char* current = (char*)bytes;
    while (current && *current) {
        char* newline = strchr(current, '\n');
        if (!newline || (size_t)(newline - current) >= SIGIL_LINE_MAX) { parse_error = 1; break; }
        *newline = 0;
        sn_chain_record_t record;
        if (sn_parse_chain_record(current, &record)) { parse_error = 1; break; }
        record.physical_line = ++physical;
        int sentinel = !strncmp(record.entry.command, "SESSION_END", 11);
        if (diff) {
            if (record.entry.sequence == first && record.attested) {
                if (found_a++) duplicate = 1; else records[0] = record;
            }
            if (record.entry.sequence == last && record.attested) {
                if (found_b++) duplicate = 1; else records[1] = record;
            }
        } else if (bisect) {
            if (!sentinel && (!have_range || (record.entry.sequence >= first && record.entry.sequence <= last))) {
                matched++;
                if (!first_failure) {
                    scanned++;
                    if (record.entry.exit_code) { records[0] = record; first_failure = 1; saved = 1; }
                }
            }
        } else if ((!tail || strcmp(operation, "history") || !sentinel) && (!pattern || strstr(record.entry.command, pattern))) {
            if (tail) { records[matched % limit] = record; if (saved < limit) saved++; }
            else if (matched >= offset && saved < limit) records[saved++] = record;
            matched++;
        }
        current = newline + 1;
    }
    free(bytes);
    if (parse_error || duplicate) {
        free(records); return semantic_error(call, duplicate ? "ambiguous_sequence" : "malformed_chain", 1);
    }
    semantic_value_t* values = &call->values;
    jdv_value_t result = jdv_value_nothing(); int complete = 1;
    if (diff) {
        if (!found_a || !found_b) { free(records); return semantic_error(call, "chain_entry_unavailable", 1); }
        jdv_record_input_t fields[] = {
            semantic_field("before", sn_chain_record_value(call, &records[0])),
            semantic_field("after", sn_chain_record_value(call, &records[1])),
            semantic_field("command_equal", jdv_value_bool(!strcmp(records[0].entry.command, records[1].entry.command))),
            semantic_field("exit_code_equal", jdv_value_bool(records[0].entry.exit_code == records[1].entry.exit_code)),
            semantic_field("comparison_scope", semantic_symbol(values, "recorded-command-and-exit-code; no filesystem comparison"))
        };
        result = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    } else if (bisect) {
        jdv_record_input_t fields[] = {
            semantic_field("first_failure", first_failure ? sn_chain_record_value(call, &records[0]) : jdv_value_nothing()),
            semantic_field("records_scanned_until_result", jdv_value_u64(scanned)),
            semantic_field("matching_records", jdv_value_u64(matched)),
            semantic_field("range_from", have_range ? jdv_value_u64(first) : jdv_value_nothing()),
            semantic_field("range_to", have_range ? jdv_value_u64(last) : jdv_value_nothing()),
            semantic_field("executed", jdv_value_bool(0)),
            semantic_field("search_scope", semantic_symbol(values, "first recorded nonzero exit code in file order"))
        };
        result = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    } else {
        jdv_value_t rows[SN_QUERY_ROWS];
        uint64_t start = tail && matched > limit ? matched - limit : tail ? 0 : offset;
        for (size_t i = 0; i < saved; i++) {
            size_t index = tail ? (size_t)((start + i) % limit) : i;
            rows[i] = sn_chain_record_value(call, &records[index]);
        }
        int more = !tail && start <= matched && (uint64_t)saved < matched - start;
        complete = !more;
        jdv_record_input_t fields[] = {
            semantic_field("path", semantic_path(values, chain_path())),
            semantic_field("present", jdv_value_bool(present)),
            semantic_field("records", semantic_list(values, rows, saved)),
            semantic_field("matching_records", jdv_value_u64(matched)),
            semantic_field("physical_lines", jdv_value_u64(physical)),
            semantic_field("source_bytes", jdv_value_filesize(length)),
            semantic_field("offset", jdv_value_u64(start)),
            semantic_field("limit", jdv_value_u64(limit)),
            semantic_field("next_offset", more ? jdv_value_u64(start + saved) : jdv_value_nothing()),
            semantic_field("selection", semantic_symbol(values, tail ? "tail" : "page")),
            semantic_field("pattern", semantic_text(values, pattern)),
            semantic_field("verification_scope", semantic_symbol(values, "syntax-only; hashes and tags not verified"))
        };
        result = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
    }
    free(records);
    return sn_query_finish(call, operation, result, complete, NULL, 0);
}

static jdv_value_t sn_cap_names(semantic_value_t* values, uint32_t bits) {
    jdv_value_t names[32]; size_t count = 0;
    for (size_t i = 0; CAP_NAMES[i].name; i++)
        if (bits & CAP_NAMES[i].bit) names[count++] = semantic_symbol(values, CAP_NAMES[i].name);
    return semantic_list(values, names, count);
}
typedef struct { semantic_builtin_t* call; jdv_value_t modules[256]; size_t count; int error; } sn_caps_t;
static void sn_caps_callback(const jmod_entry_t* module, void* context) {
    sn_caps_t* snapshot = context;
    if (snapshot->count == 256) { snapshot->error = 1; return; }
    semantic_value_t* values = &snapshot->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("name", semantic_text(values, module->name)),
        semantic_field("version", semantic_text(values, module->version)),
        semantic_field("declared_mask", jdv_value_u64(module->caps_declared)),
        semantic_field("used_mask", jdv_value_u64(module->caps_used)),
        semantic_field("declared", sn_cap_names(values, module->caps_declared)),
        semantic_field("used", sn_cap_names(values, module->caps_used)),
        semantic_field("undeclared_mask", jdv_value_u64(module->caps_used & ~module->caps_declared)),
        semantic_field("declaration_matches", jdv_value_bool(!(module->caps_used & ~module->caps_declared)))
    };
    snapshot->modules[snapshot->count++] = semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}
static int sn_caps(semantic_builtin_t* call) {
    sn_caps_t snapshot = {.call = call}; jmod_each(sn_caps_callback, &snapshot);
    if (snapshot.error) return semantic_error(call, "module_count_limit", 1);
    jdv_record_input_t fields[] = {
        semantic_field("modules", semantic_list(&call->values, snapshot.modules, snapshot.count)),
        semantic_field("count", jdv_value_u64(snapshot.count))
    };
    return sn_query_finish(call, "caps", semantic_record(&call->values, fields, 2), 1, NULL, 0);
}

static int sn_proof_identity(semantic_builtin_t* call) {
    int status = ed_key_load() ? 1 : 0; char fingerprint[65] = {0};
    if (!status) hexenc(g_ed_pk, sizeof g_ed_pk, fingerprint);
    jdv_record_input_t fields[] = {
        semantic_field("available", jdv_value_bool(!status)),
        semantic_field("algorithm", semantic_symbol(&call->values, "ed25519")),
        semantic_field("key_fingerprint", semantic_text(&call->values, status ? NULL : fingerprint))
    };
    return sn_query_finish(call, "proof.identity", semantic_record(&call->values, fields, 3), !status,
                            status ? "identity_unavailable" : NULL, status);
}

static int sn_proof_show(semantic_builtin_t* call, const char* path) {
    uint8_t* bytes = NULL; size_t length = 0;
    if (sn_read_regular(path, 8191, &bytes, &length))
        return semantic_error(call, errno == EFBIG ? "proof_byte_limit" : "proof_read_failed", 1);
    uint8_t digest[32]; char hash[65]; jash_sha256(bytes, length, digest); jash_sha256_hex(digest, hash);
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(&call->values, path)),
        semantic_field("content", semantic_bytes(&call->values, bytes, length)),
        semantic_field("bytes", jdv_value_filesize(length)),
        semantic_field("sha256", semantic_text(&call->values, hash)),
        semantic_field("verified", jdv_value_bool(0))
    };
    free(bytes);
    return sn_query_finish(call, "proof.show", semantic_record(&call->values, fields, 5), 1, NULL, 0);
}

static int sn_hex_equal(const char* a, const char* b) {
    uint8_t first[32], second[32];
    return !hexdec(a, first, sizeof first) && !hexdec(b, second, sizeof second) &&
        !memcmp(first, second, sizeof first);
}

/* Trust files are bounded semantic inputs, not a source of silently accepted
 * partial prefixes. Absent trust is unknown origin; I/O failure is explicit. */
static int sn_proof_trust(const char* key, int* trusted, const char** source) {
    *trusted = 0; *source = "none";
    if (g_ed_ready) {
        char own[65]; hexenc(g_ed_pk, sizeof g_ed_pk, own);
        if (sn_hex_equal(key, own)) { *trusted = 1; *source = "local-identity"; return 0; }
    } else {
        uint8_t* seed = NULL; size_t size = 0;
        if (!sn_read_regular(sigil_identity_ed_seed(), ED25519_SEED, &seed, &size)) {
            struct stat metadata;
            int bad = size != ED25519_SEED || lstat(sigil_identity_ed_seed(), &metadata) ||
                      (metadata.st_mode & 077) || metadata.st_uid != geteuid();
            if (!bad) {
                uint8_t public_key[ED25519_PUBKEY], secret[ED25519_SECKEY]; char own[65];
                ed25519_keypair_from_seed(seed, public_key, secret);
                memset(secret, 0, sizeof secret); hexenc(public_key, sizeof public_key, own);
                if (sn_hex_equal(key, own)) { *trusted = 1; *source = "local-identity"; }
            }
            memset(seed, 0, size); free(seed);
            if (bad) { errno = EINVAL; return -1; }
            if (*trusted) return 0;
        } else if (errno != ENOENT) return -1;
    }
    uint8_t* bytes = NULL; size_t length = 0;
    if (sn_read_regular(sigil_identity_trusted_pub(), 65536, &bytes, &length)) return errno == ENOENT ? 0 : -1;
    int error = memchr(bytes, 0, length) != NULL;
    char* current = (char*)bytes;
    while (!error && *current) {
        char* newline = strchr(current, '\n'); if (newline) *newline = 0;
        size_t size = strlen(current);
        if (size && current[size - 1] == '\r') current[--size] = 0;
        if (size && current[0] != '#') {
            if (size != 64 || !sigil_hex64(current)) error = 1;
            else if (sn_hex_equal(current, key)) { *trusted = 1; *source = "trusted-public-keys"; }
        }
        if (!newline) break;
        current = newline + 1;
    }
    free(bytes); if (error) { *trusted = 0; errno = EINVAL; return -1; } return 0;
}

static int sn_proof_binding(const jef_proof_observation_t* proof, uint64_t* count, char tip[65]) {
    const char* body = (const char*)proof->payload;
    size_t length = proof->payload_length;
    if (length < 12 || memcmp(body, "jashproof/1\n", 12) || memchr(body, 0, length)) return -1;
    unsigned seen = 0; size_t offset = 12;
    while (offset < length) {
        const char* line = body + offset;
        const char* lf = memchr(line, '\n', length - offset);
        if (!lf) return -1;
        size_t size = (size_t)(lf - line);
        if (size >= 15 && !memcmp(line, "chain_entries: ", 15)) {
            if (seen & 1) return -1;
            char number[32]; size_t digits = size - 15;
            if (!digits || digits >= sizeof number) return -1;
            memcpy(number, line + 15, digits); number[digits] = 0;
            if (sn_unsigned(number, count, NULL)) return -1;
            seen |= 1;
        } else if (size >= 11 && !memcmp(line, "chain_tip: ", 11)) {
            if ((seen & 2) || size != 75) return -1;
            memcpy(tip, line + 11, 64); tip[64] = 0;
            if (!sigil_hex64(tip)) return -1;
            seen |= 2;
        }
        offset += size + 1;
    }
    return seen == 3 ? 0 : -1;
}

static int sn_proof_verify(semantic_builtin_t* call, int argc, char** argv) {
    const char* path = NULL; const char* pinned = NULL; int literal = 0;
    for (int i = 0; i < argc; i++) {
        if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argv[i], "--pubkey")) {
            if (pinned || ++i == argc || strlen(argv[i]) != 64 || !sigil_hex64(argv[i]))
                return semantic_error(call, "invalid_public_key", 2);
            pinned = argv[i];
        } else if ((!literal && argv[i][0] == '-') || path) return semantic_error(call, "invalid_proof_arguments", 2);
        else path = argv[i];
    }
    if (!path) path = sigil_identity_proof();
    uint8_t* bytes = NULL; size_t length = 0;
    if (sn_read_regular(path, 8191, &bytes, &length))
        return semantic_error(call, errno == EFBIG ? "proof_byte_limit" : "proof_read_failed", 1);
    jef_proof_observation_t proof;
    int parsed = jef_proof_inspect_bytes((const char*)bytes, (unsigned)length, &proof) == 0;
    free(bytes);
    if (!parsed) return semantic_error(call, "malformed_public_proof", 1);
    int trusted = 0; const char* trust_source = "none";
    int trust_error = pinned ? 0 : sn_proof_trust(proof.public_key, &trusted, &trust_source);
    if (pinned) { trusted = sn_hex_equal(proof.public_key, pinned); trust_source = "explicit-public-key"; }
    uint64_t count = 0; char tip[65] = {0}, live_tip[65] = {0};
    int binding = (!proof.dsse || !strcmp(proof.payload_type, DSSE_PAYLOAD_TYPE)) && !sn_proof_binding(&proof, &count, tip);
    int freshness = -1;
    if (proof.signature_valid && binding) freshness = chain_tip_status(tip, live_tip, count);
    const char* error = !proof.signature_valid ? "invalid_signature" : pinned && !trusted ? "unexpected_identity" :
        trust_error ? "trust_source_unavailable" : freshness == 3 ? "proof_obsolete" : freshness == 2 ? "live_chain_diverged_or_unavailable" : NULL;
    int status = !error ? 0 : freshness == 3 && proof.signature_valid && (!pinned || trusted) && !trust_error ? 3 :
        freshness == 2 && proof.signature_valid && (!pinned || trusted) && !trust_error ? 2 : 1;
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, path)),
        semantic_field("format", semantic_symbol(values, proof.dsse ? "dsse" : "jashproof/1")),
        semantic_field("algorithm", semantic_symbol(values, "ed25519")),
        semantic_field("signature_valid", jdv_value_bool(proof.signature_valid)),
        semantic_field("public_key", semantic_text(values, proof.public_key)),
        semantic_field("identity_trusted", trust_error ? jdv_value_nothing() : jdv_value_bool(trusted)),
        semantic_field("identity_authenticated", trust_error ? jdv_value_nothing() : jdv_value_bool(trusted && proof.signature_valid)),
        semantic_field("trust_source", semantic_symbol(values, trust_source)),
        semantic_field("payload_type", proof.dsse ? semantic_text(values, proof.payload_type) : jdv_value_nothing()),
        semantic_field("signed_payload", semantic_bytes(values, proof.payload, proof.payload_length)),
        semantic_field("chain_binding_available", jdv_value_bool(binding)),
        semantic_field("chain_entries", binding ? jdv_value_u64(count) : jdv_value_nothing()),
        semantic_field("chain_tip", binding ? semantic_text(values, tip) : jdv_value_nothing()),
        semantic_field("live_chain_tip", live_tip[0] ? semantic_text(values, live_tip) : jdv_value_nothing()),
        semantic_field("live_chain_status", semantic_symbol(values, freshness < 0 ? "not-checked" : !freshness ? "matches" : freshness == 3 ? "obsolete" : "diverged-or-unavailable")),
        semantic_field("proof_scope", semantic_symbol(values, "signature over payload; no execution correctness guarantee"))
    };
    return sn_query_finish(call, "proof.verify", semantic_record(values, fields, sizeof fields / sizeof fields[0]),
                            !status, error, status);
}

static int sn_verify(semantic_builtin_t* call, int argc, char** argv) {
    int range = argc != 0; uint64_t first = 0, last = 0;
    if (range && sn_range(argc, argv, &first, &last)) return semantic_error(call, "invalid_sequence_range", 2);
    sigil_verify_observation_t observation;
    int status = sigil_verify_observe_range(&observation, -1, range, first, last);
    semantic_value_t* values = &call->values;
    jdv_record_input_t checks[] = {
        semantic_field("hash_chain", semantic_symbol(values, observation.valid || observation.kind == SIGIL_VERIFY_WITNESS ? "pass" :
            observation.failure && !strcmp(observation.failure, "hash_mismatch") ? "fail" : "not-established")),
        semantic_field("attested_seq_monotonicity", semantic_symbol(values, observation.valid || observation.kind == SIGIL_VERIFY_WITNESS ? "pass" :
            observation.failure && !strcmp(observation.failure, "seq_rollback") ? "fail" : "not-established")),
        semantic_field("ratchet_position", semantic_symbol(values, observation.valid ? "pass" :
            observation.kind == SIGIL_VERIFY_WITNESS ? "fail" : "not-established")),
        semantic_field("per_entry_tags", semantic_symbol(values, "not-verified"))
    };
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(values, chain_path())),
        semantic_field("valid", jdv_value_bool(observation.valid)),
        semantic_field("failure", semantic_symbol(values, observation.failure)),
        semantic_field("at_sequence", observation.kind == SIGIL_VERIFY_WALK && status ? jdv_value_u64(observation.at_sequence) : jdv_value_nothing()),
        semantic_field("entries_verified", jdv_value_u64(observation.entries_verified)),
        semantic_field("entries_unattested", jdv_value_u64(observation.entries_unattested)),
        semantic_field("entries_detected", jdv_value_u64(observation.entries_detected)),
        semantic_field("fragments", jdv_value_u64(observation.fragments)),
        semantic_field("sequence_restarts", jdv_value_u64(observation.sequence_restarts)),
        semantic_field("ratchet_advances", jdv_value_u64(observation.ratchet_advances)),
        semantic_field("witness_entries", jdv_value_u64(observation.witness_entries)),
        semantic_field("witness_scope", semantic_symbol(values, observation.witness_scope)),
        semantic_field("boundaries", jdv_value_u64(observation.boundaries)),
        semantic_field("missing", jdv_value_u64(observation.missing)),
        semantic_field("source_bytes", jdv_value_filesize(observation.source_bytes)),
        semantic_field("errno", observation.error_number ? jdv_value_i64(observation.error_number) : jdv_value_nothing()),
        semantic_field("checks", semantic_record(values, checks, sizeof checks / sizeof checks[0])),
        semantic_field("range_from", range ? jdv_value_u64(first) : jdv_value_nothing()),
        semantic_field("range_to", range ? jdv_value_u64(last) : jdv_value_nothing()),
        semantic_field("range_verified", range ? jdv_value_u64(observation.range_verified) : jdv_value_nothing()),
        semantic_field("verification_scope", semantic_symbol(values, "whole-chain hash and sequence walk plus current ratchet witness; requested range is an observation within that walk")),
        semantic_field("limits", semantic_symbol(values, "forward-secure historical tags cannot be recomputed; deleting chain and witness together is indistinguishable from fresh state without external proof"))
    };
    (void)sigil_chain_verify_result(status);
    return sn_query_finish(call, "verify", semantic_record(values, fields, sizeof fields / sizeof fields[0]),
                            !status, observation.failure, status);
}

#include "sigil_recovery_native.inc"

int jash_sigil_native_v1(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    if (!call || !call->admitted) return 125;
    g_read_only_pending = 0;
    g_native_read_only_argc = -1;
    if (!argc) return semantic_error(call, "missing_sigil_operation", 2);
    int action = sn_actions(call, state, argc, argv);
    if (action >= 0) return action;
    if (!strcmp(argv[0], "quarantine") && argc == 2 && !strcmp(argv[1], "apply"))
        return sn_quarantine_apply(call);
    if (!strcmp(argv[0], "ratchet")) {
        if (argc == 2 && (!strcmp(argv[1], "rekey") || !strcmp(argv[1], "adopt")))
            return sn_ratchet_transition(call, !strcmp(argv[1], "adopt"));
        return semantic_error(call, "invalid_ratchet_arguments", 2);
    }
    sigil_native_read_only_mark(argc, argv);
    const char* operation = argv[0]; argc--; argv++;
    if (!strcmp(operation, "quarantine")) {
        if (!argc || (argc == 1 && !strcmp(argv[0], "status"))) return sn_quarantine_status(call);
        return semantic_error(call, "invalid_quarantine_arguments", 2);
    }
    if (!strcmp(operation, "verify")) return sn_verify(call, argc, argv);
    if (!strcmp(operation, "chain") && argc && (!strcmp(argv[0], "verify") || !strcmp(argv[0], "check")))
        return sn_verify(call, argc - 1, argv + 1);
    if (!strcmp(operation, "list") || !strcmp(operation, "history") || !strcmp(operation, "blame") ||
        !strcmp(operation, "diff") || !strcmp(operation, "bisect")) return sn_chain_query(call, operation, argc, argv);
    if (!strcmp(operation, "chain") && (!argc || (strcmp(argv[0], "verify") && strcmp(argv[0], "check"))))
        return sn_chain_query(call, "list", argc, argv);
    if (!strcmp(operation, "caps")) return argc ? semantic_error(call, "invalid_caps_arguments", 2) : sn_caps(call);
    if (!strcmp(operation, "proof") && argc) {
        const char* verb = argv[0]; argc--; argv++;
        if (!strcmp(verb, "identity")) return argc ? semantic_error(call, "invalid_identity_arguments", 2) : sn_proof_identity(call);
        if (!strcmp(verb, "show")) {
            int literal = argc && !strcmp(argv[0], "--");
            if (literal) { argc--; argv++; }
            if (argc > 1 || (argc && !literal && argv[0][0] == '-')) return semantic_error(call, "invalid_proof_arguments", 2);
            return sn_proof_show(call, argc ? argv[0] : sigil_identity_proof());
        }
        if (!strcmp(verb, "verify")) return sn_proof_verify(call, argc, argv);
    }
    return semantic_error(call, "unsupported_sigil_operation", 2);
}
