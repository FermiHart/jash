// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "change_store.h"
#include "eval_output.h"
#include "change_model.inc"

typedef struct {
    semantic_builtin_t* call;
    const char* operation;
    const char* name;
    const char* path;
    size_t attempted, applied, planned;
    int sink_failed, scratch_remaining;
} cn_output_t;

static const char* cn_template(const char* id) {
    if (!strcmp(id, "proposal")) return CHG_TPL_PROPOSAL;
    if (!strcmp(id, "specs")) return CHG_TPL_SPEC;
    if (!strcmp(id, "design")) return CHG_TPL_DESIGN;
    if (!strcmp(id, "tasks")) return CHG_TPL_TASKS;
    if (!strcmp(id, "reproduce")) return CHG_TPL_REPRODUCE;
    if (!strcmp(id, "fix")) return CHG_TPL_FIX;
    if (!strcmp(id, "verify")) return CHG_TPL_VERIFY_BUGFIX;
    if (!strcmp(id, "findings")) return CHG_TPL_FINDINGS;
    return NULL;
}
static int cn_failure(semantic_builtin_t* call, const char* operation, const cn_store_t* store, int status) {
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.change.v1")),
        semantic_field("record", semantic_symbol(sem, "error")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("ok", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(sem, store->error ? store->error : "operation_failed")),
        semantic_field("errno", jdv_value_i64(store->error_number)),
        semantic_field("effects_started", jdv_value_bool(call->admitted))
    };
    return semantic_finish(call, semantic_record(sem, fields, sizeof fields / sizeof fields[0]), status);
}
static jdv_value_t cn_schema_value(semantic_value_t* sem, const chg_schema_t* schema) {
    jdv_value_t artifacts[CHG_MAX_ARTIFACTS];
    for (int i = 0; i < schema->n_artifacts; i++) {
        const chg_artifact_t* artifact = &schema->artifacts[i]; jdv_value_t requires[CHG_MAX_REQUIRES];
        for (int j = 0; j < artifact->n_requires; j++) requires[j] = semantic_text(sem, artifact->requires[j]);
        jdv_record_input_t fields[] = {
            semantic_field("id", semantic_text(sem, artifact->id)),
            semantic_field("generates", semantic_path(sem, artifact->generates)),
            semantic_field("requires", semantic_list(sem, requires, (size_t)artifact->n_requires)),
            semantic_field("template_available", jdv_value_bool(cn_template(artifact->id) != NULL))
        };
        artifacts[i] = semantic_record(sem, fields, sizeof fields / sizeof fields[0]);
    }
    jdv_record_input_t fields[] = {
        semantic_field("name", semantic_text(sem, schema->name)),
        semantic_field("artifacts", semantic_list(sem, artifacts, (size_t)schema->n_artifacts))
    };
    return semantic_record(sem, fields, 2);
}
static jdv_value_t cn_change_value(semantic_value_t* sem, const cn_change_t* change, int legacy) {
    jdv_value_t artifacts[CHG_MAX_ARTIFACTS];
    for (int i = 0; i < change->schema.n_artifacts; i++) {
        const chg_artifact_t* artifact = &change->schema.artifacts[i];
        const char* status = change->done[i] ? "done" : legacy ? "pending" :
            chg_artifact_ready(&change->schema, artifact, change->done) ? "ready" : "blocked";
        jdv_value_t requires[CHG_MAX_REQUIRES];
        for (int j = 0; j < artifact->n_requires; j++) requires[j] = semantic_text(sem, artifact->requires[j]);
        jdv_record_input_t fields[] = {
            semantic_field("id", semantic_text(sem, artifact->id)),
            semantic_field("status", semantic_symbol(sem, status)),
            semantic_field("generates", semantic_path(sem, artifact->generates)),
            semantic_field("requires", semantic_list(sem, requires, (size_t)artifact->n_requires)),
            semantic_field("template_available", jdv_value_bool(cn_template(artifact->id) != NULL))
        };
        artifacts[i] = semantic_record(sem, fields, legacy ? 2u : sizeof fields / sizeof fields[0]);
    }
    jdv_record_input_t fields[12]; size_t count = 0;
    fields[count++] = semantic_field("name", semantic_text(sem, change->meta.name));
    fields[count++] = semantic_field("schema", semantic_text(sem, change->meta.schema));
    fields[count++] = semantic_field("status", semantic_text(sem, change->meta.status));
    fields[count++] = semantic_field("artifacts", semantic_list(sem, artifacts, (size_t)change->schema.n_artifacts));
    if (*change->meta.goal || !legacy) fields[count++] = semantic_field("goal", *change->meta.goal ? semantic_text(sem, change->meta.goal) : jdv_value_nothing());
    if (!legacy) {
        fields[count++] = semantic_field("uuid", semantic_text(sem, change->meta.uuid));
        fields[count++] = semantic_field("created_unix_seconds", jdv_value_i64(change->meta.created));
        fields[count++] = semantic_field("path", semantic_path(sem, change->path));
        fields[count++] = semantic_field("metadata_bytes", jdv_value_u64(change->metadata.size));
        fields[count++] = semantic_field("artifact_observation", semantic_symbol(sem, "file-presence"));
    }
    return semantic_record(sem, fields, count);
}
static int cn_query(semantic_builtin_t* call, const char* operation, const char* name, int legacy) {
    cn_store_t store;
    if (cn_open(&store, 0)) return cn_failure(call, operation, &store, 1);
    cn_names_t names = {0}, schemas = {0}; int exists = 0, status = 0;
    jdv_value_t changes[CN_ENTRIES], schema_values[CN_ENTRIES]; size_t change_count = 0, schema_count = 0;
    if (!strcmp(operation, "recall")) {
        char selected[CHG_MAX_NAME];
        if (cn_resolve(&store, name, selected)) status = -1;
        else { names.values[0] = strdup(selected); if (!names.values[0]) status = cn_fail(&store, "allocation_failed", ENOMEM); else names.count = 1; }
    } else {
        status = cn_active_names(&store, &names);
        if (!status) status = cn_list(&store, CHG_SCHEMA_DIR, &schemas, &exists);
    }
    for (size_t i = 0; i < schemas.count && !status; i++) {
        if (schemas.values[i][0] == '.') continue;
        chg_schema_t schema; cn_file_t source = {0};
        status = cn_schema_load(&store, schemas.values[i], &schema, &source);
        if (!status) schema_values[schema_count++] = legacy ? semantic_text(&call->values, schema.name) : cn_schema_value(&call->values, &schema);
        cn_file_free(&source);
    }
    for (size_t i = 0; i < names.count && !status; i++) {
        char path[CN_PATH]; cn_change_t* change = calloc(1, sizeof(*change));
        if (!change) { status = cn_fail(&store, "allocation_failed", ENOMEM); break; }
        if (cn_format(path, sizeof path, "%s/%s", CHG_DIR, names.values[i])) status = cn_fail(&store, "change_path_limit", errno);
        else status = cn_change_load(&store, path, change);
        if (!status && strcmp(change->meta.name, names.values[i])) status = cn_fail(&store, "change_name_mismatch", EINVAL);
        if (!status) changes[change_count++] = cn_change_value(&call->values, change, legacy);
        cn_file_free(&change->metadata); free(change);
    }
    cn_names_free(&names); cn_names_free(&schemas);
    if (cn_close(&store)) status = -1;
    if (status) return cn_failure(call, operation, &store, 1);
    semantic_value_t* sem = &call->values;
    if (legacy) {
        jdv_record_input_t fields[] = {
            semantic_field("schemas", semantic_list(sem, schema_values, schema_count)),
            semantic_field("changes", semantic_list(sem, changes, change_count))
        };
        return semantic_finish(call, semantic_record(sem, fields, 2), 0);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.change.v1")),
        semantic_field("record", semantic_symbol(sem, "query")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("project", semantic_path(sem, store.cwd)),
        semantic_field("schemas", semantic_list(sem, schema_values, schema_count)),
        semantic_field("changes", semantic_list(sem, changes, change_count)),
        semantic_field("scan_complete", jdv_value_bool(1)),
        semantic_field("observation", semantic_symbol(sem, "stable-file-snapshots")),
        semantic_field("lock_scope", semantic_symbol(sem, "cooperating-native-change")),
        semantic_field("max_directory_entries", jdv_value_u64(CN_ENTRIES)),
        semantic_field("max_file_bytes", jdv_value_u64(CN_FILE_BYTES))
    };
    return semantic_finish(call, semantic_record(sem, fields, sizeof fields / sizeof fields[0]), 0);
}
static char* cn_quoted(const char* text) {
    size_t size = strlen(text); if (size > 4096u || !jash_json_utf8_valid_n(text, size)) { errno = EILSEQ; return NULL; }
    size_t capacity = size * 6u + 3u; char* result = malloc(capacity); if (!result) return NULL;
    jash_json_buffer_t buffer = {result, capacity, 0}; jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer); jash_jsonw_string_n(&writer, text, size);
    if (jash_jsonw_finish(&writer)) { free(result); return NULL; }
    return result;
}
static int cn_replace(cn_store_t* store, const cn_file_t* source, const char* key, const char* value, char** output, size_t* size) {
    size_t key_size = strlen(key), value_size = strlen(value), capacity = source->size + value_size + 32u;
    if (capacity > CN_FILE_BYTES) return cn_fail(store, "metadata_output_limit", E2BIG);
    *output = malloc(capacity); if (!*output) return cn_fail(store, "allocation_failed", ENOMEM);
    size_t offset = 0, used = 0; int found = 0;
    while (offset < source->size) {
        size_t start = offset; while (offset < source->size && source->bytes[offset] != '\n') offset++;
        if (offset < source->size) offset++;
        size_t length = offset - start;
        if (length > key_size && !memcmp(source->bytes + start, key, key_size) && source->bytes[start + key_size] == ':') {
            if (found++) { free(*output); *output = NULL; return cn_fail(store, "duplicate_metadata_field", EINVAL); }
            int written = snprintf(*output + used, capacity - used, "%s: %s\n", key, value);
            if (written < 0 || (size_t)written >= capacity - used) { free(*output); *output = NULL; return cn_fail(store, "metadata_output_limit", E2BIG); }
            used += (size_t)written;
        } else { memcpy(*output + used, source->bytes + start, length); used += length; }
    }
    if (!found) { free(*output); *output = NULL; return cn_fail(store, "metadata_field_missing", EINVAL); }
    (*output)[used] = 0; *size = used; return 0;
}
static int cn_plan_status(cn_store_t* store, cn_plan_t* plan, const cn_change_t* change, const char* path, const char* status) {
    char target[CN_PATH], *bytes = NULL; size_t size = 0;
    if (cn_format(target, sizeof target, "%s/.change.yaml", path)) return cn_fail(store, "change_path_limit", errno);
    if (cn_replace(store, &change->metadata, "status", status, &bytes, &size)) return -1;
    int failed = cn_plan_publish(store, plan, target, bytes, size, &change->metadata); free(bytes); return failed;
}
static int cn_plan_schema(cn_store_t* store, cn_plan_t* plan, const char* source_name, const char* name) {
    if (!cn_name(name) || (source_name && !cn_name(source_name))) return cn_fail(store, "invalid_schema_name", EINVAL);
    char directory[CN_PATH], path[CN_PATH];
    if (cn_format(directory, sizeof directory, "%s/%s", CHG_SCHEMA_DIR, name) || cn_format(path, sizeof path, "%s/schema.yaml", directory))
        return cn_fail(store, "schema_path_limit", errno);
    char* bytes = NULL; size_t size = 0; cn_file_t source = {0}; int status = 0;
    if (source_name) {
        chg_schema_t schema;
        status = cn_schema_load(store, source_name, &schema, &source);
        if (!status) status = cn_replace(store, &source, "name", name, &bytes, &size);
    } else {
        const char* builtin = chg_builtin_schema(name);
        if (!builtin) status = cn_fail(store, "unknown_builtin_schema", EINVAL);
        else { bytes = strdup(builtin); size = strlen(builtin); if (!bytes) status = cn_fail(store, "allocation_failed", ENOMEM); }
    }
    if (!status) status = cn_plan_directory(store, plan, directory, 1);
    if (!status) status = cn_plan_publish(store, plan, path, bytes, size, NULL);
    free(bytes); cn_file_free(&source); return status;
}
static int cn_plan_propose(cn_store_t* store, cn_plan_t* plan, const char* name, const char* schema_name, const char* goal) {
    if (!cn_name(name) || !strcmp(name, "schemas") || !strcmp(name, "archive") || !cn_name(schema_name))
        return cn_fail(store, "invalid_change_name", EINVAL);
    chg_schema_t schema; cn_file_t source = {0};
    int status = cn_schema_load(store, schema_name, &schema, &source); cn_file_free(&source); if (status) return -1;
    char directory[CN_PATH], path[CN_PATH];
    if (cn_format(directory, sizeof directory, "%s/%s", CHG_DIR, name) || cn_format(path, sizeof path, "%s/.change.yaml", directory))
        return cn_fail(store, "change_path_limit", errno);
    char* quoted = goal ? cn_quoted(goal) : NULL;
    if (goal && !quoted) return cn_fail(store, "invalid_goal", errno);
    struct timespec now; static unsigned long serial;
    if (clock_gettime(CLOCK_REALTIME, &now)) { free(quoted); return cn_fail(store, "clock_failed", errno); }
    char bytes[32768];
    if (cn_format(bytes, sizeof bytes,
        "uuid: chg-%llx-%lx-%lx-%lx\nname: %s\nschema: %s\n%s%s%sstatus: proposed\ncreated: %lld\n",
        (unsigned long long)now.tv_sec, (unsigned long)now.tv_nsec, (unsigned long)getpid(), ++serial,
        name, schema_name, goal ? "goal: " : "", goal ? quoted : "", goal ? "\n" : "", (long long)now.tv_sec)) {
        free(quoted); return cn_fail(store, "metadata_output_limit", errno);
    }
    free(quoted);
    if (cn_plan_directory(store, plan, directory, 1) || cn_plan_directory(store, plan, CHG_ARCHIVE_DIR, 0)) return -1;
    return cn_plan_publish(store, plan, path, bytes, strlen(bytes), NULL);
}
static int cn_plan_artifacts(cn_store_t* store, cn_plan_t* plan, cn_change_t* change, int all) {
    int done[CHG_MAX_ARTIFACTS]; memcpy(done, change->done, sizeof done);
    int created = 0;
    for (int pass = 0; pass < change->schema.n_artifacts; pass++) {
        for (int i = 0; i < change->schema.n_artifacts; i++) {
            if (done[i] || !chg_artifact_ready(&change->schema, &change->schema.artifacts[i], done)) continue;
            chg_artifact_t* artifact = &change->schema.artifacts[i];
            const char* body = cn_template(artifact->id);
            if (!body) return cn_fail(store, "artifact_template_unavailable", ENOTSUP);
            char path[CN_PATH], bytes[8192];
            if (cn_format(path, sizeof path, "%s/%s", change->path, !strcmp(artifact->id, "specs") ? "specs/spec.md" : artifact->generates) ||
                cn_format(bytes, sizeof bytes, body, change->meta.name)) return cn_fail(store, "artifact_output_limit", errno);
            if (cn_plan_publish(store, plan, path, bytes, strlen(bytes), NULL)) return -1;
            done[i] = 1; created++;
            if (!all) {
                if (!strcmp(artifact->id, "tasks")) return cn_plan_status(store, plan, change, change->path, "ready");
                return 0;
            }
        }
    }
    for (int i = 0; i < change->schema.n_artifacts; i++) if (!done[i]) return cn_fail(store, "artifact_dependencies_blocked", EINVAL);
    return created ? cn_plan_status(store, plan, change, change->path, "ready") : 0;
}
static int cn_plan_sync(cn_store_t* store, cn_plan_t* plan, const cn_change_t* change) {
    char root[CN_PATH]; if (cn_format(root, sizeof root, "%s/specs", change->path)) return cn_fail(store, "spec_path_limit", errno);
    cn_names_t files = {0}; int status = cn_tree_markdown(store, root, 0, &files);
    chg_spec_t* specs = calloc(3, sizeof(*specs));
    if (!specs && !status) status = cn_fail(store, "allocation_failed", ENOMEM);
    for (size_t i = 0; i < files.count && !status; i++) {
        const char* relative = files.values[i] + strlen(root) + 1u;
        char target[CN_PATH]; cn_file_t delta = {0}, base = {0}; char* bytes = NULL; size_t size = 0;
        if (cn_format(target, sizeof target, "specs/%s", relative)) { status = cn_fail(store, "spec_path_limit", errno); break; }
        status = cn_read(store, files.values[i], &delta);
        if (!status) status = cn_read(store, target, &base);
        if (!status) status = cn_spec_parse(store, &base, 0, &specs[0]);
        if (!status) status = cn_spec_parse(store, &delta, 1, &specs[1]);
        if (!status && chg_apply_delta(&specs[0], &specs[1], &specs[2])) status = cn_fail(store, "delta_conflict", EEXIST);
        if (!status) status = cn_spec_bytes(store, &specs[2], &bytes, &size);
        if (!status) status = cn_plan_publish(store, plan, target, bytes, size, &base);
        free(bytes); cn_file_free(&delta); cn_file_free(&base);
    }
    free(specs); cn_names_free(&files); return status;
}
static int cn_plan_checkpoint(cn_store_t* store, cn_plan_t* plan) {
    cn_step_t* step = cn_plan_add(store, plan, CN_CHECKPOINT, NULL); if (!step) return -1;
    if (jash_checkpoint_prepare(NULL, &step->checkpoint)) return cn_fail(store,
        step->checkpoint.error ? step->checkpoint.error : "checkpoint_prepare_failed", step->checkpoint.error_number);
    if (cn_format(step->path, sizeof step->path, "%s", step->checkpoint.path)) return cn_fail(store, "checkpoint_path_limit", errno);
    return 0;
}
static int cn_plan_move(cn_store_t* store, cn_plan_t* plan, const char* source, const char* target) {
    struct stat before, exists_st; int exists, target_exists;
    if (cn_stat(store, source, &before, &exists) || cn_stat(store, target, &exists_st, &target_exists)) return -1;
    if (!exists || !S_ISDIR(before.st_mode)) return cn_fail(store, "move_source_unavailable", ENOENT);
    if (target_exists) return cn_fail(store, "move_target_exists", EEXIST);
    char parent[CN_PATH]; if (cn_format(parent, sizeof parent, "%s", target)) return cn_fail(store, "change_path_limit", errno);
    char* slash = strrchr(parent, '/');
    if (slash) { *slash = 0; if (cn_plan_directory(store, plan, parent, 0)) return -1; }
    cn_step_t* step = cn_plan_add(store, plan, CN_MOVE, target); if (!step) return -1;
    if (cn_format(step->source, sizeof step->source, "%s", source)) return cn_fail(store, "change_path_limit", errno);
    step->before = before; step->before_exists = 1; return 0;
}

static jdv_value_t cn_receipt(cn_output_t* output, const char* phase, const char* path, const char* source,
                               const cn_effect_t* observed, jdv_value_t detail) {
    semantic_value_t* sem = &output->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.change.v1")),
        semantic_field("record", semantic_symbol(sem, "receipt")),
        semantic_field("operation", semantic_symbol(sem, output->operation)),
        semantic_field("step", jdv_value_u64(output->attempted)),
        semantic_field("phase", semantic_symbol(sem, phase)),
        semantic_field("path", semantic_path(sem, path)),
        semantic_field("source", semantic_path(sem, source)),
        semantic_field("applied", observed->applied < 0 ? jdv_value_nothing() : jdv_value_bool(observed->applied)),
        semantic_field("applied_scope", semantic_symbol(sem, "reported-target")),
        semantic_field("durable", observed->durable < 0 ? jdv_value_nothing() : jdv_value_bool(observed->durable)),
        semantic_field("error", semantic_symbol(sem, observed->error)),
        semantic_field("errno", observed->error_number ? jdv_value_i64(observed->error_number) : jdv_value_nothing()),
        semantic_field("cleanup_errno", observed->cleanup_error ? jdv_value_i64(observed->cleanup_error) : jdv_value_nothing()),
        semantic_field("retained_temporary_name", semantic_path(sem, *observed->temporary ? observed->temporary : NULL)),
        semantic_field("detail", detail)
    };
    return semantic_record(sem, fields, sizeof fields / sizeof fields[0]);
}
static int cn_emit(cn_output_t* output, const char* phase, const char* path, const char* source,
                     const cn_effect_t* observed, jdv_value_t detail) {
    output->attempted++;
    if (observed->applied > 0) output->applied++;
    if (observed->cleanup_error || observed->applied < 0) output->scratch_remaining = 1;
    int result = semantic_events_emit(output->call, cn_receipt(output, phase, path, source, observed, detail));
    if (result) output->sink_failed = 1;
    return result;
}
static jdv_value_t cn_result_value(cn_output_t* output, const cn_store_t* store, int status,
                                   const char* error, jdv_value_t detail) {
    semantic_value_t* sem = &output->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.change.v1")),
        semantic_field("record", semantic_symbol(sem, "result")),
        semantic_field("operation", semantic_symbol(sem, output->operation)),
        semantic_field("name", semantic_text(sem, output->name)),
        semantic_field("path", semantic_path(sem, output->path)),
        semantic_field("project", semantic_path(sem, store->cwd)),
        semantic_field("ok", jdv_value_bool(status == 0)),
        semantic_field("exit_code", jdv_value_i64(status)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("receipts", jdv_value_u64(output->attempted)),
        semantic_field("applied_receipts", jdv_value_u64(output->applied)),
        semantic_field("receipt_capacity", jdv_value_u64(output->planned)),
        semantic_field("effects_may_have_applied", jdv_value_bool(output->attempted != 0)),
        semantic_field("atomic_batch", jdv_value_bool(0)),
        semantic_field("lock_scope", semantic_symbol(sem, "cooperating-native-change")),
        semantic_field("recovery_scope", semantic_symbol(sem, "timeline-inverses-only")),
        semantic_field("detail", detail)
    };
    return semantic_record(sem, fields, sizeof fields / sizeof fields[0]);
}
static int cn_admit(cn_output_t* output, cn_store_t* store, const cn_plan_t* plan, int rewind) {
    size_t extra = rewind ? JASH_TIMELINE_MAX + 2u : 0u;
    size_t count = plan->count + extra + 1u;
    jdv_value_t* rows = calloc(count, sizeof(*rows));
    if (!rows) return cn_fail(store, "allocation_failed", ENOMEM);
    semantic_value_t* sem = &output->call->values;
    cn_effect_t maximum = {.applied = -1, .durable = 0, .error_number = INT_MAX, .cleanup_error = INT_MAX,
        .error = "publication_directory_fsync_or_source_identity_validation_failed"};
    memset(maximum.temporary, 'x', sizeof maximum.temporary - 1u);
    char controls[2049]; memset(controls, 1, sizeof controls - 1u); controls[sizeof controls - 1u] = 0;
    jdv_value_t large_detail = semantic_text(sem, controls);
    size_t used = 0; output->planned = count - 1u; output->attempted = UINT64_MAX;
    for (size_t i = 0; i < plan->count; i++) {
        const cn_step_t* step = &plan->steps[i];
        rows[used++] = cn_receipt(output, "checkpoint", step->path, *step->source ? step->source : NULL,
            &maximum, step->kind == CN_CHECKPOINT ? large_detail : jdv_value_u64(UINT64_MAX));
    }
    char old = controls[512]; controls[512] = 0;
    for (size_t i = 0; i < extra; i++) rows[used++] = cn_receipt(output, "checkpoints", controls, controls, &maximum, large_detail);
    controls[512] = old;
    rows[used++] = cn_result_value(output, store, 255, maximum.error, large_detail);
    int result = semantic_events_preflight(output->call, rows, used); free(rows);
    output->attempted = 0;
    if (result) return cn_fail(store, result == 75 ? "output_budget_exceeded" : "receipt_limit", result == 75 ? E2BIG : EOVERFLOW);
    result = semantic_admit_rows(output->call, count);
    if (result) { output->sink_failed = 1; return -1; }
    return 0;
}
static jdv_value_t cn_checkpoint_value(semantic_value_t* sem, const jash_checkpoint_result_t* checkpoint) {
    jdv_record_input_t fields[] = {
        semantic_field("name", semantic_text(sem, *checkpoint->name ? checkpoint->name : NULL)),
        semantic_field("cwd", semantic_path(sem, checkpoint->cwd)),
        semantic_field("depth", checkpoint->state_observed ? jdv_value_i64(checkpoint->depth) : jdv_value_nothing()),
        semantic_field("timestamp_seconds", checkpoint->state_observed ? jdv_value_i64(checkpoint->timestamp) : jdv_value_nothing()),
        semantic_field("record_fields", semantic_symbol(sem, "legacy-bounded"))
    };
    return semantic_record(sem, fields, sizeof fields / sizeof fields[0]);
}
static int cn_apply_plan(cn_output_t* output, cn_store_t* store, cn_plan_t* plan) {
    for (size_t i = 0; i < plan->count; i++) {
        cn_step_t* step = &plan->steps[i]; cn_effect_t result = {0}; const char* phase;
        jdv_value_t detail = jdv_value_nothing();
        if (step->kind == CN_MKDIR) { phase = "mkdir"; result = cn_directory_create(store, step); }
        else if (step->kind == CN_PUBLISH) { phase = "publish"; result = cn_publish(store, step); detail = jdv_value_u64(step->size); }
        else if (step->kind == CN_MOVE) { phase = "move"; result = cn_move(store, step); }
        else {
            phase = "checkpoint"; int status = jash_checkpoint_create_prepared(&step->checkpoint);
            result.applied = step->checkpoint.applied; result.durable = step->checkpoint.durable;
            result.error_number = step->checkpoint.error_number;
            result.error = status ? step->checkpoint.error : NULL;
            detail = cn_checkpoint_value(&output->call->values, &step->checkpoint);
        }
        if (cn_emit(output, phase, step->path, *step->source ? step->source : NULL, &result, detail)) return 125;
        if (result.error) { cn_fail(store, result.error, result.error_number); return 1; }
    }
    return 0;
}
static int cn_rewind_action(void* context, const jash_timeline_action_t* action) {
    cn_output_t* output = context; semantic_value_t* sem = &output->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("entry_id", jdv_value_u64(action->entry.id)),
        semantic_field("kind", semantic_symbol(sem, action->entry.kind_name)),
        semantic_field("description", semantic_text(sem, action->entry.description)),
        semantic_field("a", action->entry.a_is_path ? semantic_path(sem, action->entry.a) : semantic_text(sem, action->entry.a)),
        semantic_field("b", action->entry.b_is_path ? semantic_path(sem, action->entry.b) : semantic_text(sem, action->entry.b)),
        semantic_field("status", semantic_symbol(sem, action->status)),
        semantic_field("effect", semantic_symbol(sem, action->effect)),
        semantic_field("retained", jdv_value_bool(action->retained)),
        semantic_field("redo_available", jdv_value_bool(action->redo_available)),
        semantic_field("depth_before", jdv_value_i64(action->depth_before)),
        semantic_field("depth_after", jdv_value_i64(action->depth_after)),
        semantic_field("guards", semantic_symbol(sem, "legacy-inverse"))
    };
    cn_effect_t observed = {.applied = !strcmp(action->effect, "possible") ? -1 : !strcmp(action->effect, "applied"),
        .durable = -1, .error = action->error};
    return cn_emit(output, "timeline", NULL, NULL, &observed, semantic_record(sem, fields, sizeof fields / sizeof fields[0]));
}
static int cn_rewind_auxiliary(void* context, const jash_rewind_auxiliary_t* action) {
    cn_output_t* output = context;
    cn_effect_t observed = {.applied = action->applied, .durable = action->durable,
        .error = action->error, .error_number = action->error_number};
    return cn_emit(output, action->record, action->path, NULL, &observed, jdv_value_nothing());
}
static jdv_value_t cn_rewind_summary(semantic_value_t* sem, const jash_timeline_apply_summary_t* summary) {
    jdv_record_input_t fields[] = {
        semantic_field("requested", jdv_value_u64(summary->requested)),
        semantic_field("completed", jdv_value_u64(summary->completed)),
        semantic_field("failed", jdv_value_u64(summary->failed)),
        semantic_field("kept", jdv_value_u64(summary->kept)),
        semantic_field("lost", jdv_value_u64((uint64_t)summary->lost)),
        semantic_field("depth_before", jdv_value_i64(summary->depth_before)),
        semantic_field("depth_after", jdv_value_i64(summary->depth_after)),
        semantic_field("stopped", jdv_value_bool(summary->stopped))
    };
    return semantic_record(sem, fields, sizeof fields / sizeof fields[0]);
}

typedef struct {
    const char *operation, *name, *source, *schema, *goal;
} cn_arguments_t;
static int cn_parse(int argc, char* const* argv, cn_arguments_t* args) {
    *args = (cn_arguments_t){.operation = argc ? argv[0] : "status", .schema = "spec-driven"};
    if (!argc) return 0;
    int first = 1;
    if (!strcmp(args->operation, "schema")) {
        if (argc < 2) return -1;
        if (!strcmp(argv[1], "init")) args->operation = "schema.init";
        else if (!strcmp(argv[1], "fork")) args->operation = "schema.fork";
        else return -1;
        first = 2;
    } else if (strcmp(args->operation, "status") && strcmp(args->operation, "recall") &&
               strcmp(args->operation, "propose") && strcmp(args->operation, "continue") &&
               strcmp(args->operation, "ff") && strcmp(args->operation, "apply") &&
               strcmp(args->operation, "verify") && strcmp(args->operation, "sync") &&
               strcmp(args->operation, "archive") && strcmp(args->operation, "rewind")) return -1;
    int ended = 0, schema_given = 0, goal_given = 0;
    for (int i = first; i < argc; i++) {
        const char* value = argv[i];
        if (!ended && !strcmp(value, "--")) { ended = 1; continue; }
        if (!ended && !strcmp(args->operation, "propose") && (!strcmp(value, "--schema") || !strcmp(value, "--goal"))) {
            int schema = !strcmp(value, "--schema");
            if (++i == argc || !*argv[i] || (schema ? schema_given++ : goal_given++)) return -1;
            if (schema) args->schema = argv[i]; else args->goal = argv[i];
            continue;
        }
        if ((!ended && *value == '-') || !*value) return -1;
        if (!strcmp(args->operation, "schema.fork") && !args->source) args->source = value;
        else if (!args->name) args->name = value;
        else return -1;
    }
    if (!strcmp(args->operation, "status")) return args->name ? -1 : 0;
    if (!strcmp(args->operation, "schema.init") || !strcmp(args->operation, "schema.fork") ||
        !strcmp(args->operation, "propose") || !strcmp(args->operation, "rewind")) {
        if (!args->name) return -1;
    }
    if (!strcmp(args->operation, "rewind")) return strlen(args->name) <= NAME_MAX && chg_name_valid(args->name) ? 0 : -1;
    if ((args->name && !cn_name(args->name)) || (args->source && !cn_name(args->source)) || !cn_name(args->schema)) return -1;
    return 0;
}
const char* jash_change_effect_argv(int argc, char* const* argv) {
    if (argc == 2 && !strcmp(argv[0], "status") &&
        (!strcmp(argv[1], "--json") || !strcmp(argv[1], "--data") || !strcmp(argv[1], "--data-v2"))) return "read";
    if (!argc || (strcmp(argv[0], "--json") && strcmp(argv[0], "--data") && strcmp(argv[0], "--data-v2"))) return "exec";
    cn_arguments_t args;
    if (cn_parse(argc - 1, argv + 1, &args)) return "exec";
    if (!strcmp(args.operation, "status") || !strcmp(args.operation, "recall")) return "read";
    return !strcmp(args.operation, "verify") ? "exec" : "mutate";
}
static void cn_count_markdown(const cn_file_t* file, uint64_t* total, uint64_t* done, uint64_t* requirements, uint64_t* scenarios) {
    size_t offset = 0;
    while (offset < file->size) {
        size_t start = offset; while (offset < file->size && file->bytes[offset] != '\n') offset++;
        size_t length = offset - start; if (offset < file->size) offset++;
        const char* line = file->bytes + start;
        if (requirements && length >= 16 && !memcmp(line, "### Requirement:", 16)) (*requirements)++;
        if (scenarios && length >= 14 && !memcmp(line, "#### Scenario:", 14)) (*scenarios)++;
        if (total) {
            size_t skip = 0; while (skip < length && (line[skip] == ' ' || line[skip] == '\t')) skip++;
            if (length - skip >= 5 && (!memcmp(line + skip, "- [ ]", 5) || !memcmp(line + skip, "- [x]", 5) || !memcmp(line + skip, "- [X]", 5))) {
                (*total)++; if (done && line[skip + 3] != ' ') (*done)++;
            }
        }
    }
}
static int cn_verify(cn_output_t* output, cn_store_t* store, jash_state_t* state, const cn_change_t* change) {
    char path[CN_PATH]; cn_file_t tasks = {0}; uint64_t total = 0, done = 0, requirements = 0, scenarios = 0;
    if (cn_format(path, sizeof path, "%s/tasks.md", change->path)) {
        cn_fail(store, "change_path_limit", errno); (void)cn_close(store);
        return cn_failure(output->call, output->operation, store, 1);
    }
    int status = cn_read(store, path, &tasks);
    if (!status && tasks.size && memchr(tasks.bytes, 0, tasks.size)) status = cn_fail(store, "invalid_task_file", EINVAL);
    if (!status) cn_count_markdown(&tasks, &total, &done, NULL, NULL);
    int tasks_present = tasks.exists; cn_file_free(&tasks);
    cn_names_t files = {0};
    if (!status && cn_format(path, sizeof path, "%s/specs", change->path)) status = cn_fail(store, "spec_path_limit", errno);
    if (!status) status = cn_tree_markdown(store, path, 0, &files);
    for (size_t i = 0; i < files.count && !status; i++) {
        cn_file_t spec = {0}; status = cn_read(store, files.values[i], &spec);
        if (!status && (!spec.exists || (spec.size && memchr(spec.bytes, 0, spec.size)))) status = cn_fail(store, "invalid_spec", EINVAL);
        if (!status) cn_count_markdown(&spec, NULL, NULL, &requirements, &scenarios);
        cn_file_free(&spec);
    }
    size_t spec_files = files.count; cn_names_free(&files);
    struct stat st; int test_exists = 0, make_exists = 0;
    if (!status) status = cn_stat(store, "tests/test_suite.sh", &st, &test_exists);
    if (!status && test_exists && !S_ISREG(st.st_mode)) status = cn_fail(store, "test_target_not_regular", EINVAL);
    if (!status && !test_exists) status = cn_stat(store, "Makefile", &st, &make_exists);
    if (!status && make_exists && !S_ISREG(st.st_mode)) status = cn_fail(store, "build_target_not_regular", EINVAL);
    if (status) { (void)cn_close(store); return cn_failure(output->call, output->operation, store, 1); }
    const char* command = test_exists ? "bash tests/test_suite.sh" : make_exists ? "make" : NULL;
    cn_plan_t* preflight = calloc(1, sizeof(*preflight));
    if (!preflight) { cn_fail(store, "allocation_failed", ENOMEM); (void)cn_close(store); return cn_failure(output->call, output->operation, store, 125); }
    if (command) (void)cn_plan_add(store, preflight, CN_CHECKPOINT, test_exists ? "tests/test_suite.sh" : "Makefile");
    if (cn_admit(output, store, preflight, 0)) {
        cn_plan_free(preflight); (void)cn_close(store);
        return output->sink_failed ? 125 : cn_failure(output->call, output->operation, store, 75);
    }
    cn_plan_free(preflight);
    jash_eval_observation_t child = {0}; int child_error = 0, child_code = 0;
    if (command) {
        /* A project test may itself inspect change state. Its execution is
         * outside the snapshot lock; reported inputs are explicitly prior. */
        if (flock(store->root, LOCK_UN)) child_error = errno;
        int nullfd = child_error ? -1 : open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullfd < 0) { if (!child_error) child_error = errno; }
        else {
            if (jash_eval_with_output(state, command, nullfd, nullfd, &child)) child_error = child.adapter_error;
            if (close(nullfd) && !child_error) child_error = errno;
            child_code = child.command_status;
        }
        semantic_value_t* sem = &output->call->values;
        jdv_record_input_t fields[] = {
            semantic_field("command", semantic_text(sem, command)),
            semantic_field("attempted", jdv_value_bool(child.attempted)),
            semantic_field("command_status", child.attempted ? jdv_value_i64(child_code) : jdv_value_nothing()),
            semantic_field("output", semantic_symbol(sem, "discarded")),
            semantic_field("adapter_errno", child_error ? jdv_value_i64(child_error) : jdv_value_nothing()),
            semantic_field("restore_errno", child.restore_error ? jdv_value_i64(child.restore_error) : jdv_value_nothing())
        };
        cn_effect_t observed = {.applied = child.attempted ? -1 : 0, .durable = -1,
            .error_number = child_error, .error = child_error ? "verification_execution_failed" : NULL};
        if (cn_emit(output, "execute", test_exists ? "tests/test_suite.sh" : "Makefile", NULL, &observed,
                    semantic_record(sem, fields, sizeof fields / sizeof fields[0]))) { (void)cn_close(store); return 125; }
    }
    int close_error = cn_close(store);
    int ready = total > 0 && total == done && (!command || child_code == 0) && !child_error && !close_error;
    semantic_value_t* sem = &output->call->values;
    jdv_record_input_t fields[] = {
        semantic_field("tasks_present", jdv_value_bool(tasks_present)),
        semantic_field("tasks_total", jdv_value_u64(total)), semantic_field("tasks_done", jdv_value_u64(done)),
        semantic_field("spec_files", jdv_value_u64(spec_files)), semantic_field("requirements", jdv_value_u64(requirements)),
        semantic_field("scenarios", jdv_value_u64(scenarios)), semantic_field("target", semantic_text(sem, command)),
        semantic_field("target_status", command && child.attempted ? jdv_value_i64(child_code) : jdv_value_nothing()),
        semantic_field("input_observation", semantic_symbol(sem, "before-execution")),
        semantic_field("ready_to_archive", jdv_value_bool(ready)),
        semantic_field("readiness_rule", semantic_symbol(sem, "nonempty-complete-tasks-and-selected-target-success")),
        semantic_field("semantic_correctness_verified", jdv_value_bool(0))
    };
    jdv_stream_end_t outcome; jdv_stream_end_init(&outcome);
    outcome.state = child_error || close_error ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = child_error || close_error ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = ready ? 0 : 1; outcome.errors_seen = child_error || close_error ? 1 : 0;
    const char* error = child_error ? "verification_execution_failed" : close_error ? store->error : NULL;
    return semantic_finish_outcome(output->call, cn_result_value(output, store, outcome.producer_status, error,
        semantic_record(sem, fields, sizeof fields / sizeof fields[0])), &outcome);
}
static int cn_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    cn_arguments_t args;
    if (cn_parse(argc, argv, &args)) return semantic_error(call, "invalid_arguments", 2);
    if (!strcmp(args.operation, "status") || !strcmp(args.operation, "recall")) return cn_query(call, args.operation, args.name, 0);
    cn_store_t store;
    if (cn_open(&store, 1)) return cn_failure(call, args.operation, &store, 1);
    cn_plan_t* plan = calloc(1, sizeof(*plan)); cn_change_t* change = calloc(1, sizeof(*change));
    if (!plan || !change) {
        free(plan); free(change); cn_fail(&store, "allocation_failed", ENOMEM); (void)cn_close(&store);
        return cn_failure(call, args.operation, &store, 125);
    }
    int status = 0; char name[CHG_MAX_NAME] = "", path[CN_PATH] = "", destination[CN_PATH] = "";
    jash_rewind_plan_t* rewind = NULL; jdv_value_t result_detail = jdv_value_nothing();
    cn_output_t output = {.call = call, .operation = args.operation, .name = args.name};
    if (!strcmp(args.operation, "schema.init") || !strcmp(args.operation, "schema.fork")) {
        status = cn_plan_schema(&store, plan, args.source, args.name);
        if (!status && cn_format(path, sizeof path, "%s/%s", CHG_SCHEMA_DIR, args.name)) status = cn_fail(&store, "schema_path_limit", errno);
    } else if (!strcmp(args.operation, "propose")) {
        status = cn_plan_propose(&store, plan, args.name, args.schema, args.goal);
        if (!status && cn_format(path, sizeof path, "%s/%s", CHG_DIR, args.name)) status = cn_fail(&store, "change_path_limit", errno);
    } else {
        if (!strcmp(args.operation, "rewind")) {
            if (cn_format(path, sizeof path, "%s/%s", CHG_ARCHIVE_DIR, args.name)) status = cn_fail(&store, "change_path_limit", errno);
        } else {
            status = cn_resolve(&store, args.name, name);
            if (!status && cn_format(path, sizeof path, "%s/%s", CHG_DIR, name)) status = cn_fail(&store, "change_path_limit", errno);
        }
        if (!status) status = cn_change_load(&store, path, change);
        if (!status && strcmp(args.operation, "rewind") && strcmp(name, change->meta.name)) status = cn_fail(&store, "change_name_mismatch", EINVAL);
        if (!status) { output.name = change->meta.name; output.path = path; }
        if (!status && !strcmp(args.operation, "verify")) {
            cn_plan_free(plan);
            int result = cn_verify(&output, &store, state, change);
            cn_file_free(&change->metadata); free(change); return result;
        }
        if (!status && (!strcmp(args.operation, "continue") || !strcmp(args.operation, "ff")))
            status = cn_plan_artifacts(&store, plan, change, !strcmp(args.operation, "ff"));
        else if (!status && !strcmp(args.operation, "apply")) {
            for (int i = 0; i < change->schema.n_artifacts; i++) if (!change->done[i]) { status = cn_fail(&store, "artifacts_incomplete", EINVAL); break; }
            if (!status) status = cn_plan_checkpoint(&store, plan);
            if (!status) status = cn_plan_status(&store, plan, change, path, "applying");
        } else if (!status && (!strcmp(args.operation, "sync") || !strcmp(args.operation, "archive"))) {
            if (!strcmp(args.operation, "archive")) status = cn_plan_checkpoint(&store, plan);
            if (!status) status = cn_plan_sync(&store, plan, change);
            if (!status && !strcmp(args.operation, "archive")) {
                time_t now = time(NULL); struct tm utc; char date[32];
                if (now == (time_t)-1 || !gmtime_r(&now, &utc) || !strftime(date, sizeof date, "%Y-%m-%d", &utc)) status = cn_fail(&store, "clock_failed", EIO);
                else if (cn_format(destination, sizeof destination, "%s/%s-%s", CHG_ARCHIVE_DIR, date, name)) status = cn_fail(&store, "archive_path_limit", errno);
                if (!status) status = cn_plan_move(&store, plan, path, destination);
            }
        } else if (!status && !strcmp(args.operation, "rewind")) {
            if (cn_format(destination, sizeof destination, "%s/%s", CHG_DIR, change->meta.name)) status = cn_fail(&store, "change_path_limit", errno);
            if (!status) status = cn_plan_move(&store, plan, path, destination);
            if (!status) status = cn_plan_status(&store, plan, change, destination, "proposed");
            if (!status) {
                const char* error = NULL; int prepared = jash_rewind_prepare(NULL, 0, 0, NULL, &rewind, &error);
                if (prepared) status = cn_fail(&store, error ? error : "rewind_prepare_failed", errno ? errno : ENOENT);
            }
        }
    }
    output.path = *destination ? destination : *path ? path : NULL;
    if (!status && cn_admit(&output, &store, plan, rewind != NULL)) status = output.sink_failed ? 125 : 75;
    if (!status && rewind) {
        jash_timeline_apply_summary_t summary;
        status = jash_rewind_apply(rewind, state, cn_rewind_action, cn_rewind_auxiliary, &output, &summary);
        result_detail = cn_rewind_summary(&call->values, &summary);
        if (status && !output.sink_failed) cn_fail(&store, summary.error ? summary.error : "rewind_failed", EIO);
    }
    if (!status) status = cn_apply_plan(&output, &store, plan);
    jash_rewind_plan_free(rewind); cn_plan_free(plan);
    if (cn_close(&store) && !status) status = 1;
    int result;
    if (output.sink_failed) result = 125;
    else if (!call->admitted) result = cn_failure(call, args.operation, &store, status == 75 ? 75 : 1);
    else result = semantic_finish(call, cn_result_value(&output, &store, status, status ? store.error : NULL, result_detail), status);
    cn_file_free(&change->metadata); free(change); return result;
}
static int cn_legacy_status_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state; (void)argc; (void)argv; return cn_query(call, "status", NULL, 1);
}

int jash_b_change_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int result = semantic_dispatch_argv(state, writer, context, "change", "jash.change.v1", argc, argv, cn_provider);
    if (result >= 0) return result;
    if (argc && !strcmp(argv[0], "status")) {
        const char* selected = NULL; int selectors = 0;
        for (int i = 1; i < argc && strcmp(argv[i], "--"); i++)
            if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) { selected = argv[i]; selectors++; }
        if (selected) {
            char* adapted[] = {(char*)selected, (char*)(selectors == 1 && argc == 2 ? "status" : "invalid-status-options")};
            return semantic_dispatch_argv(state, writer, context, "change", "jash.change.v1", 2, adapted,
                selectors == 1 && argc == 2 && !strcmp(selected, "--json") ? cn_legacy_status_provider : cn_provider);
        }
    }
    char* text = semantic_legacy_join(argc, argv); if (!text) return 125;
    result = change_legacy(state, writer, context, text); free(text); return result;
}
int jash_b_change(jash_state_t* state, jash_writer_t writer, void* context, const char* arguments) {
    int result = semantic_dispatch(state, writer, context, "change", "jash.change.v1", arguments, cn_provider);
    if (result >= 0) return result;
    jash_argv_t parsed = {0};
    if (arguments && !jash_argv_build(arguments, &parsed)) {
        int status_mode = parsed.argc && !strcmp(parsed.argv[0], "status");
        if (status_mode) result = jash_b_change_argv(state, writer, context, parsed.argc, parsed.argv);
        jash_argv_free(&parsed);
        if (status_mode) return result;
    } else jash_argv_free(&parsed);
    return change_legacy(state, writer, context, arguments);
}
