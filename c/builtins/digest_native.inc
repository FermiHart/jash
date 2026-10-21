// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include <core/digest_native.h>
#include <module/blake3.h>
#include <module/jash_crypto.h>
#include "memory_chain.h"
#include "json_git_observe.h"

static int digest_mode_error(semantic_builtin_t* call, const char* message) {
    return call ? semantic_error(call, message, 2) : digest_arg_error(message);
}

static jdv_value_t digest_entry_value(semantic_value_t* values, const digest_native_entry_t* entry) {
    jdv_record_input_t fields[7] = {
        semantic_field("path", semantic_path(values, entry->path)),
        semantic_field("kind", semantic_symbol(values, entry->kind)),
        semantic_field("language", semantic_symbol(values, entry->language))
    };
    size_t count = 3;
    if (!entry->directory) {
        fields[count++] = semantic_field("blake3", semantic_text(values, entry->blake3));
        fields[count++] = semantic_field("bytes", jdv_value_filesize(entry->bytes));
        fields[count++] = semantic_field("lines", jdv_value_u64(entry->lines));
    }
    if (entry->old_blake3[0]) fields[count++] = semantic_field("old_blake3", semantic_text(values, entry->old_blake3));
    return semantic_record(values, fields, count);
}

static jdv_value_t digest_entries_value(semantic_value_t* values, digest_snapshot_t* snapshot,
                                        const char* change) {
    const digest_native_observation_t* observed = digest_snapshot_observation(snapshot);
    jdv_value_t* entries = calloc(observed->entries ? observed->entries : 1, sizeof *entries);
    if (!entries) { values->error = 1; return jdv_value_nothing(); }
    size_t count = 0;
    for (size_t i = 0; i < observed->entries; i++) {
        digest_native_entry_t entry;
        if (digest_snapshot_entry(snapshot, i, &entry)) { values->error = 1; break; }
        if (change && (!entry.change || strcmp(change, entry.change))) continue;
        entries[count++] = digest_entry_value(values, &entry);
        if (values->error) break;
    }
    jdv_value_t result = semantic_list(values, entries, count); free(entries); return result;
}

static jdv_value_t digest_coverage_value(semantic_value_t* values, const digest_native_observation_t* observed) {
    jdv_value_t ignored[64]; size_t count = 0; const char* name;
    while ((name = digest_native_ignored_directory(count)) && count < 64) {
        ignored[count] = semantic_text(values, name); count++;
    }
    jdv_record_input_t fields[] = {
        semantic_field("gitignore_evaluated", jdv_value_bool(0)),
        semantic_field("ignored_directories", semantic_list(values, ignored, count)),
        semantic_field("excluded_files", jdv_value_u64(observed->excluded_files)),
        semantic_field("excluded_special", jdv_value_u64(observed->excluded_special)),
        semantic_field("excluded_directories", jdv_value_u64(observed->excluded_directories)),
        semantic_field("scope", semantic_text(values, "recognized source, docs, build, config and directories; opaque files excluded"))
    };
    return semantic_record(values, fields, sizeof fields / sizeof fields[0]);
}

static jdv_value_t digest_project_value(semantic_builtin_t* call, const digest_cfg_t* cfg,
                                        digest_snapshot_t* snapshot, int preflight) {
    semantic_value_t* values = &call->values;
    const digest_native_observation_t* observed = digest_snapshot_observation(snapshot);
    jdv_record_input_t fields[20]; size_t count = 0;
    const char* schema = cfg->manifest ? "jash.project-observation.v1" : cfg->diff_base ? "jash.project-diff.v1" :
                         cfg->fingerprint ? "jash.digest.fingerprint.v1" : "jash.digest.project.v1";
    fields[count++] = semantic_field("schema", semantic_symbol(values, schema));
    fields[count++] = semantic_field("generator", semantic_symbol(values, DIGEST_GENERATOR_VERSION));
    fields[count++] = semantic_field("root", semantic_path(values, observed->root));
    fields[count++] = semantic_field("fingerprint", semantic_text(values, observed->fingerprint));
    if (cfg->fingerprint) return semantic_record(values, fields, count);
    if (cfg->diff_base) {
        fields[count++] = semantic_field("base_fingerprint", semantic_text(values, observed->base_fingerprint));
        for (size_t i = 0; i < 3; i++) {
            const char* change = i == 0 ? "added" : i == 1 ? "modified" : "removed";
            fields[count++] = semantic_field(change, digest_entries_value(values, snapshot, change));
        }
        return semantic_record(values, fields, count);
    }
    fields[count++] = semantic_field("consistency", semantic_symbol(values, "best-effort"));
    fields[count++] = semantic_field("coverage", digest_coverage_value(values, observed));
    fields[count++] = semantic_field("entries", digest_entries_value(values, snapshot, NULL));
    if (cfg->manifest) return semantic_record(values, fields, count);
    fields[count++] = semantic_field("level", semantic_symbol(values, observed->level));
    jdv_record_input_t counts[] = {
        semantic_field("files", jdv_value_u64(observed->files)),
        semantic_field("directories", jdv_value_u64(observed->directories)),
        semantic_field("visited", jdv_value_u64(observed->visited)),
        semantic_field("bytes", jdv_value_filesize(observed->bytes)),
        semantic_field("lines", jdv_value_u64(observed->lines))
    };
    fields[count++] = semantic_field("counts", semantic_record(values, counts, 5));
    jdv_record_input_t readme[] = {
        semantic_field("path", semantic_path(values, observed->readme_path)),
        semantic_field("prefix", semantic_text_n(values, observed->readme, observed->readme_bytes)),
        semantic_field("retained_bytes", jdv_value_filesize(observed->readme_bytes)),
        semantic_field("prefix_limit", jdv_value_filesize(8192))
    };
    fields[count++] = semantic_field("readme", semantic_record(values, readme, 4));
    jdv_record_input_t effects[] = {
        semantic_field("requested", jdv_value_bool(observed->cache_requested)),
        semantic_field("key", semantic_text(values, observed->cache_key[0] ? observed->cache_key : NULL)),
        semantic_field("hit", jdv_value_bool(observed->cache_hit)),
        semantic_field("effects_started", jdv_value_bool(preflight || observed->effects_started)),
        semantic_field("published", jdv_value_bool(preflight || observed->cache_published)),
        semantic_field("durable", jdv_value_bool(preflight || observed->cache_durable)),
        semantic_field("cas_attempted", jdv_value_bool(preflight || observed->cas_attempted)),
        semantic_field("cas_complete", jdv_value_bool(preflight || observed->cas_complete)),
        semantic_field("cas_verified_objects", jdv_value_u64(preflight ? UINT64_MAX : observed->cas_verified_objects)),
        semantic_field("ledger_written", jdv_value_bool(preflight || observed->ledger_written)),
        semantic_field("ledger_durable", jdv_value_bool(preflight || observed->ledger_durable))
    };
    fields[count++] = semantic_field("cache", semantic_record(values, effects, sizeof effects / sizeof effects[0]));
    fields[count++] = semantic_field("complete", jdv_value_bool(!observed->error));
    fields[count++] = semantic_field("error", semantic_symbol(values, preflight ? "content_store_incomplete" : observed->error));
    return semantic_record(values, fields, count);
}

static int digest_native_output(semantic_builtin_t* call, jdv_value_t value, int status, const char* path, int preview) {
    if (!path) return preview ? semantic_preflight(call, &value) : semantic_finish(call, value, status);
    if (call->values.error) return semantic_finish(call, value, 125);
    jash_structured_buffer_t bytes; jash_structured_buffer_init(&bytes, SEMANTIC_OUTPUT_BYTES);
    int encoded;
    if (call->mode == 1) encoded = value_output_json(jash_structured_buffer_sink, &bytes, "digest", &value,
        &call->values.limits, SEMANTIC_OUTPUT_BYTES, status);
    else {
        jdv_schema_t record = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        encoded = value_output_data(jash_structured_buffer_sink, &bytes, &value, 1, NULL, &record,
            &call->values.limits, SEMANTIC_OUTPUT_BYTES, status, status ? "operation_failed" : NULL);
    }
    if (encoded != status || bytes.error) { jash_structured_buffer_free(&bytes); return 125; }
    uint8_t hash[32]; char hex[65]; jash_blake3(bytes.data, bytes.used, hash); jash_blake3_hex(hash, hex);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.digest.output.v1")),
        semantic_field("path", semantic_path(&call->values, path)),
        semantic_field("format", semantic_symbol(&call->values, call->mode == 1 ? "json" : "jtv2")),
        semantic_field("bytes", jdv_value_filesize(bytes.used)),
        semantic_field("blake3", semantic_text(&call->values, hex)),
        semantic_field("command_status", jdv_value_i64(status)),
        semantic_field("applied", jdv_value_bool(1)),
        semantic_field("durable", jdv_value_bool(1)),
        semantic_field("error", semantic_symbol(&call->values, "output_publication_failed")),
        semantic_field("prior_effects_may_have_applied", jdv_value_bool(call->admitted))
    };
    jdv_value_t receipt = semantic_record(&call->values, fields, sizeof fields / sizeof fields[0]);
    int result = semantic_preflight(call, &receipt);
    if (preview) { jash_structured_buffer_free(&bytes); return result; }
    if (!result) result = semantic_admit(call);
    if (result) { jash_structured_buffer_free(&bytes); return result; }
    char parent[PATH_MAX], name[PATH_MAX];
    if (strlen(path) >= sizeof parent) { jash_structured_buffer_free(&bytes); return semantic_error(call, "output_path_limit", 2); }
    if (jash_copy_cstr(parent, sizeof(parent), path)) { jash_structured_buffer_free(&bytes); return semantic_error(call, "output_path_limit", 2); } char* slash = strrchr(parent, '/');
    if (slash) { if (jash_copy_cstr(name, sizeof(name), slash + 1)) { jash_structured_buffer_free(&bytes); return semantic_error(call, "output_path_limit", 2); } if (slash == parent) slash[1] = 0; else *slash = 0; }
    else { if (jash_copy_cstr(name, sizeof(name), path)) { jash_structured_buffer_free(&bytes); return semantic_error(call, "output_path_limit", 2); } if (jash_copy_cstr(parent, sizeof(parent), ".")) { jash_structured_buffer_free(&bytes); return semantic_error(call, "output_path_limit", 2); } }
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int error = directory < 0 ? errno : 0;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) error = EINVAL;
    struct stat existing;
    if (!error && fstatat(directory, name, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode)) error = EINVAL;
    } else if (!error && errno != ENOENT) error = errno;
    char temporary[96]; int fd = -1; static unsigned serial;
    if (!error) {
        for (int i = 0; i < 32; i++) {
            snprintf(temporary, sizeof temporary, ".digest-output-%ld-%u.tmp", (long)getpid(), ++serial);
            fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0 || errno != EEXIST) break;
        }
        if (fd < 0) error = errno;
    }
    int applied = 0, durable = 0;
    if (fd >= 0) {
        if (memory_store_write_all(fd, bytes.data, bytes.used) || fsync(fd)) error = errno;
        if (close(fd) && !error) error = errno;
        if (!error && renameat(directory, temporary, directory, name)) error = errno;
        else if (!error) { applied = 1; if (fsync(directory)) error = errno; else durable = 1; }
        if (!applied) (void)unlinkat(directory, temporary, 0);
    }
    if (directory >= 0 && close(directory) && !error) error = errno;
    jash_structured_buffer_free(&bytes);
    fields[6].value = jdv_value_bool(applied); fields[7].value = jdv_value_bool(durable);
    fields[8].value = semantic_symbol(&call->values, error ? "output_publication_failed" : NULL);
    return semantic_finish(call, semantic_record(&call->values, fields, sizeof fields / sizeof fields[0]), error ? 1 : status);
}

static int digest_json_equals(const jdj_value_t* value, const char* expected) {
    return value && value->kind == JDJ_STRING && value->scalar.len == strlen(expected) &&
        !memcmp(value->scalar.data, expected, value->scalar.len);
}

static int digest_ledger_unsigned(const jdj_value_t* value) {
    if (!value || value->kind != JDJ_NUMBER || !value->scalar.len) return 0;
    uint64_t number = 0;
    for (size_t i = 0; i < value->scalar.len; i++) {
        unsigned digit = value->scalar.data[i] - '0';
        if (digit > 9 || number > (UINT64_MAX - digit) / 10) return 0;
        number = number * 10 + digit;
    }
    return 1;
}

static int digest_ledger_root(const jdj_value_t* root) {
    if (!root) return 0;
    if (root->kind == JDJ_STRING)
        return root->scalar.len && !memchr(root->scalar.data, 0, root->scalar.len);
    if (root->kind != JDJ_OBJECT || root->count != 2 ||
        !digest_json_equals(jdj_get(root, "encoding", 8), "base64")) return 0;
    const jdj_value_t* data = jdj_get(root, "data", 4);
    if (!data || data->kind != JDJ_STRING || !data->scalar.len || data->scalar.len % 4) return 0;
    size_t length = data->scalar.len;
    const uint8_t* bytes = data->scalar.data;
    size_t padding = (bytes[length - 1] == '=') + (bytes[length - 2] == '=');
    for (size_t i = 0; i < length - padding; i++)
        if (!((bytes[i] >= 'A' && bytes[i] <= 'Z') || (bytes[i] >= 'a' && bytes[i] <= 'z') ||
              (bytes[i] >= '0' && bytes[i] <= '9') || bytes[i] == '+' || bytes[i] == '/')) return 0;
    if (padding == 2 && bytes[length - 1] != '=') return 0;
    return 1;
}

static int digest_ledger_receipt_valid(const jdj_value_t* receipt) {
    if (receipt->kind != JDJ_OBJECT || receipt->count != 11 ||
        !digest_json_equals(jdj_get(receipt, "schema", 6), "jash.digest-receipt.v1")) return 0;
    const jdj_value_t* generator = jdj_get(receipt, "generator", 9);
    const jdj_value_t* level = jdj_get(receipt, "level", 5);
    const jdj_value_t* fingerprint = jdj_get(receipt, "fingerprint", 11);
    const jdj_value_t* hit = jdj_get(receipt, "served_from_cache", 17);
    if (!generator || generator->kind != JDJ_STRING || !generator->scalar.len ||
        !digest_ledger_root(jdj_get(receipt, "root", 4)) ||
        (!digest_json_equals(level, "brief") && !digest_json_equals(level, "standard") &&
         !digest_json_equals(level, "deep")) || !hit || hit->kind != JDJ_BOOL ||
        !fingerprint || fingerprint->kind != JDJ_STRING || fingerprint->scalar.len != 64) return 0;
    for (size_t i = 0; i < fingerprint->scalar.len; i++) {
        unsigned char c = fingerprint->scalar.data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    const char* integers[] = {"ts", "bytes_out", "files", "directories", "lines"};
    for (size_t i = 0; i < sizeof integers / sizeof integers[0]; i++)
        if (!digest_ledger_unsigned(jdj_get(receipt, integers[i], strlen(integers[i])))) return 0;
    return 1;
}

static int digest_ledger_value(semantic_builtin_t* call, long count, jdv_value_t* result) {
    char* bytes = NULL; size_t length = 0; int present = 0;
    if (digest_native_ledger(&bytes, &length, &present)) return semantic_error(call, "ledger_read_failed", 1);
    size_t starts[1000], ends[1000], rows = 0, offset = 0;
    int invalid = length && (memchr(bytes, 0, length) || bytes[length - 1] != '\n');
    while (!invalid && offset < length) {
        const char* end = memchr(bytes + offset, '\n', length - offset);
        if (!end || end == bytes + offset) { invalid = 1; break; }
        starts[rows % (size_t)count] = offset; ends[rows % (size_t)count] = (size_t)(end - bytes);
        rows++; offset = (size_t)(end - bytes) + 1;
    }
    size_t keep = rows < (size_t)count ? rows : (size_t)count;
    jdv_value_t* receipts = calloc(keep ? keep : 1, sizeof *receipts);
    if (!receipts) invalid = 1;
    for (size_t i = 0; !invalid && i < keep; i++) {
        size_t slot = (rows - keep + i) % (size_t)count;
        jdj_value_t parsed;
        if (jdj_parse(&call->values.arena, bytes + starts[slot], ends[slot] - starts[slot], &call->values.limits, &parsed) ||
            !digest_ledger_receipt_valid(&parsed)) { invalid = 1; break; }
        if (!invalid && jdj_to_value(&call->values.arena, &parsed, &call->values.limits, &receipts[i])) invalid = 1;
    }
    free(bytes);
    if (invalid) { free(receipts); return semantic_error(call, "ledger_invalid_or_over_limit", 1); }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.digest.ledger.v1")),
        semantic_field("present", jdv_value_bool(present)),
        semantic_field("total_rows", jdv_value_u64(rows)),
        semantic_field("returned_rows", jdv_value_u64(keep)),
        semantic_field("receipts", semantic_list(&call->values, receipts, keep)),
        semantic_field("complete", jdv_value_bool(1))
    };
    *result = semantic_record(&call->values, fields, 6); free(receipts); return -1;
}

static int digest_session_value(semantic_builtin_t* call, digest_level_t level, int last, int no_git,
                                 jdv_value_t* result, int* status) {
    memory_chain_t chain; (void)memory_chain_load(&chain, 0, 0);
    size_t wanted = last ? (size_t)last : level == DIGEST_BRIEF ? 10 : level == DIGEST_DEEP ? 100 : 25;
    size_t keep = chain.count < wanted ? chain.count : wanted;
    jdv_value_t* rows = calloc(keep ? keep : 1, sizeof *rows);
    if (!rows) { memory_chain_free(&chain); return semantic_error(call, "allocation_failed", 125); }
    uint64_t failed = 0, command_bytes = 0;
    for (size_t i = 0; i < keep; i++) {
        const memory_chain_entry_t* entry = &chain.entries[chain.count - keep + i];
        rows[i] = memory_chain_value(&call->values, entry);
        failed += entry->status != 0; command_bytes += strlen(entry->command);
    }
    jdv_value_t git = jdv_value_nothing(); jash_git_observation_t git_observation = {0};
    if (!no_git) (void)jash_git_observe(&call->values, 1, &git, &git_observation);
    *status = chain.error ? 1 : git_observation.adapter_status ? git_observation.adapter_status : git_observation.command_status;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, "jash.digest.session.v1")),
        semantic_field("level", semantic_symbol(&call->values, level == DIGEST_BRIEF ? "brief" : level == DIGEST_DEEP ? "deep" : "standard")),
        semantic_field("scope", semantic_symbol(&call->values, "recorded repository chain; not proof of a single session")),
        semantic_field("source", memory_chain_source(&call->values, &chain)),
        semantic_field("commands", semantic_list(&call->values, rows, keep)),
        semantic_field("returned_commands", jdv_value_u64(keep)),
        semantic_field("failed_commands", jdv_value_u64(failed)),
        semantic_field("estimated_command_tokens", jdv_value_u64((command_bytes + 3) / 4)),
        semantic_field("token_estimate_scope", semantic_symbol(&call->values, "recorded command bytes divided by four; no model tokenizer")),
        semantic_field("git_requested", jdv_value_bool(!no_git)),
        semantic_field("git", git),
        semantic_field("complete", jdv_value_bool(!*status)),
        semantic_field("status", jdv_value_i64(*status))
    };
    *result = semantic_record(&call->values, fields, sizeof fields / sizeof fields[0]);
    free(rows); memory_chain_free(&chain); return -1;
}

static int digest_native_run(semantic_builtin_t* call, digest_cfg_t* cfg, int session,
                              int no_git, int last, int ledger, long ledger_count, int cache_status) {
    jdv_value_t value = jdv_value_nothing(); int status = 0, result;
    if (ledger) {
        result = digest_ledger_value(call, ledger_count, &value);
        if (result >= 0) return result;
    } else if (cache_status) {
        digest_native_cache_t observed;
        if (digest_native_cache_status(&observed)) return semantic_error(call, "cache_status_read_failed", 1);
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(&call->values, "jash.digest-cache-status.v1")),
            semantic_field("generator", semantic_symbol(&call->values, DIGEST_GENERATOR_VERSION)),
            semantic_field("entries", jdv_value_u64(observed.entries)),
            semantic_field("bytes", jdv_value_filesize(observed.bytes)),
            semantic_field("oldest", jdv_value_u64(observed.oldest)),
            semantic_field("newest", jdv_value_u64(observed.newest))
        };
        value = semantic_record(&call->values, fields, 6);
    } else if (cfg->fetch_hash) {
        char* bytes = NULL; size_t length = 0;
        if (digest_native_fetch(cfg->fetch_hash, &bytes, &length)) return semantic_error(call, "content_unavailable_or_invalid", 1);
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(&call->values, "jash.digest.content.v1")),
            semantic_field("blake3", semantic_text(&call->values, cfg->fetch_hash)),
            semantic_field("bytes", jdv_value_filesize(length)),
            semantic_field("content", semantic_bytes(&call->values, bytes, length)),
            semantic_field("hash_verified", jdv_value_bool(1))
        };
        value = semantic_record(&call->values, fields, 5); free(bytes);
    } else if (session) {
        result = digest_session_value(call, cfg->level, last, no_git, &value, &status);
        if (result >= 0) return result;
    } else {
        digest_snapshot_t* snapshot = NULL;
        if (!cfg->root_dir) cfg->root_dir = ".";
        if (digest_snapshot_prepare(cfg, &snapshot)) return semantic_error(call, "project_observation_failed", 1);
        value = digest_project_value(call, cfg, snapshot, 1);
        if (call->values.error) { digest_snapshot_close(snapshot); return semantic_finish(call, value, 125); }
        result = digest_native_output(call, value, 0, cfg->output_path, 1);
        if (!result && (digest_snapshot_observation(snapshot)->cache_requested || cfg->output_path)) result = semantic_admit(call);
        if (result) { digest_snapshot_close(snapshot); return result; }
        status = digest_snapshot_apply(snapshot) ? 1 : 0;
        call->values.arena.used = 0; call->values.error = 0;
        value = digest_project_value(call, cfg, snapshot, 0);
        result = digest_native_output(call, value, status, cfg->output_path, 0);
        digest_snapshot_close(snapshot); return result;
    }
    return digest_native_output(call, value, status, cfg->output_path, 0);
}
