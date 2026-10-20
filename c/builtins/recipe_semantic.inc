// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "recipe_store.h"
#include "recovery_output.h"
#include "eval_output.h"

static int recipe_names_compare(const void* left, const void* right) { return strcmp(left, right); }

static int recipe_list_semantic(semantic_builtin_t* call, jash_state_t* state) {
    recipe_control_t control = {.state = state};
    if (recipe_poll(&control)) return semantic_cancelled(call, control.cancelled);
    memory_store_t store = {-1, -1}; int error = 0, present = 0;
    char names[RECIPE_LIST_MAX][RECIPE_NAME_MAX + 1u]; size_t count = 0;
    char path[PATH_MAX], failed_path[PATH_MAX] = {0};
    const char* home = getenv("HOME");
    int n = snprintf(path, sizeof(path), "%s/.jash/recipes", home ? home : "");
    if (n < 0 || (size_t)n >= sizeof(path)) return semantic_error(call, "path_limit", 75);
    if (memory_store_open(&store, "recipes", 0)) error = errno;
    else present = store.directory >= 0;
    if (!error && present) {
        int descriptor = fcntl(store.directory, F_DUPFD_CLOEXEC, 3);
        DIR* directory = descriptor < 0 ? NULL : fdopendir(descriptor);
        if (!directory) { error = errno; if (descriptor >= 0) close(descriptor); }
        else {
            while (!error) {
                if (recipe_poll(&control)) { error = ECANCELED; break; }
                errno = 0; struct dirent* entry = readdir(directory);
                if (!entry) { if (errno) error = errno; break; }
                size_t size = strlen(entry->d_name);
                if (size < 7u || strcmp(entry->d_name + size - 7u, ".jashrc")) continue;
                if (count == RECIPE_LIST_MAX || size - 7u > RECIPE_NAME_MAX) { error = E2BIG; break; }
                memcpy(names[count], entry->d_name, size - 7u); names[count][size - 7u] = 0;
                if (!recipe_name_valid(names[count])) { error = EINVAL; break; }
                count++;
            }
            if (closedir(directory) && !error) error = errno;
        }
    }
    if (memory_store_close(&store) && !error) error = errno;
    qsort(names, count, sizeof(names[0]), recipe_names_compare);
    jdv_value_t recipes[RECIPE_LIST_MAX]; size_t observed = 0; uint64_t bytes = 0;
    recipe_document_t* document = error ? NULL : calloc(1, sizeof(*document));
    if (!error && !document) error = ENOMEM;
    for (size_t i = 0; !error && i < count; i++) {
        if (recipe_poll(&control)) { error = ECANCELED; break; }
        int status = recipe_document_read(document, names[i]);
        if (status || !document->present) error = document->error ? document->error : ESTALE;
        if (!error && document->size > RECIPE_SCAN_BYTES - bytes) error = E2BIG;
        if (!error) {
            bytes += document->size;
            recipes[observed++] = recipe_value(&call->values, document, 0);
            if (call->values.error) error = E2BIG;
        }
        if (error) memcpy(failed_path, document->path, strlen(document->path) + 1u);
        recipe_document_free(document);
    }
    free(document);
    if (control.cancelled) return semantic_cancelled(call, control.cancelled);
    if (call->values.error) { call->values.arena.used = 0; call->values.error = 0; }
    jdv_record_input_t source[] = {
        semantic_field("path", semantic_path(&call->values, path)),
        semantic_field("present", jdv_value_bool(present)),
        semantic_field("read_complete", jdv_value_bool(!error)),
        semantic_field("bytes_observed", jdv_value_filesize(bytes)),
        semantic_field("failed_source", semantic_path(&call->values, failed_path[0] ? failed_path : NULL)),
        semantic_field("error", semantic_symbol(&call->values, error ? memory_store_error(error) : NULL)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "list")),
        semantic_field("complete", jdv_value_bool(!error)),
        semantic_field("consistency", semantic_symbol(&call->values, "independent-file-observations")),
        semantic_field("source", semantic_record(&call->values, source, 7)),
        semantic_field("recipes", semantic_list(&call->values, recipes, error ? 0 : observed))
    };
    return semantic_finish(call, semantic_record(&call->values, fields, 6), error ? error == E2BIG ? 75 : 1 : 0);
}

static jdv_value_t recipe_forget_result(semantic_builtin_t* call, const recipe_document_t* recipe,
                                       const memory_write_t* publication, int attempted, int worst) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "forget")),
        semantic_field("name", semantic_symbol(&call->values, recipe->name)),
        semantic_field("ok", jdv_value_bool(!publication->error)),
        semantic_field("source", recipe_source(&call->values, recipe)),
        semantic_field("publication", recipe_publication(&call->values, recipe, publication, 0, worst)),
        semantic_field("effects_may_have_applied", jdv_value_bool(attempted))
    };
    return semantic_record(&call->values, fields, 7);
}

static jdv_value_t recipe_checkpoint_value(semantic_value_t* sem, const jash_checkpoint_result_t* checkpoint) {
    jdv_record_input_t fields[] = {
        semantic_field("name", semantic_symbol(sem, checkpoint->name)),
        semantic_field("path", semantic_path(sem, checkpoint->path)),
        semantic_field("cwd", semantic_path(sem, checkpoint->cwd)),
        semantic_field("depth", checkpoint->state_observed ? jdv_value_i64(checkpoint->depth) : jdv_value_nothing()),
        semantic_field("timestamp_seconds", checkpoint->state_observed ? jdv_value_i64(checkpoint->timestamp) : jdv_value_nothing()),
        semantic_field("applied", jdv_value_bool(checkpoint->applied)),
        semantic_field("durable", jdv_value_bool(checkpoint->durable)),
        semantic_field("error", semantic_symbol(sem, checkpoint->error)),
        semantic_field("errno", checkpoint->error_number ? jdv_value_i64(checkpoint->error_number) : jdv_value_nothing()),
        semantic_field("coverage", semantic_symbol(sem, "existing-jash-timeline-only"))
    };
    return semantic_record(sem, fields, 10);
}

static jdv_value_t recipe_checkpoint_row(semantic_builtin_t* call, const jash_checkpoint_result_t* checkpoint) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "run")),
        semantic_field("row_kind", semantic_symbol(&call->values, "checkpoint")),
        semantic_field("checkpoint", recipe_checkpoint_value(&call->values, checkpoint)),
        semantic_field("effects_may_have_applied", jdv_value_bool(1))
    };
    return semantic_record(&call->values, fields, 5);
}

static jdv_value_t recipe_step_row(semantic_builtin_t* call, const recipe_document_t* recipe,
                                  size_t index, int status, int attempted, const char* error) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "run")),
        semantic_field("row_kind", semantic_symbol(&call->values, "step")),
        semantic_field("index", jdv_value_u64(index + 1u)),
        semantic_field("kind", semantic_symbol(&call->values, recipe->kinds[index] == 'A' ? "assertion" : "command")),
        semantic_field("command", semantic_text(&call->values, recipe->commands[index])),
        semantic_field("command_status", attempted ? jdv_value_i64(status) : jdv_value_nothing()),
        semantic_field("attempted", jdv_value_bool(attempted)),
        semantic_field("output_destination", semantic_symbol(&call->values, "stderr")),
        semantic_field("effects_may_have_applied", jdv_value_bool(attempted)),
        semantic_field("error", semantic_symbol(&call->values, error))
    };
    return semantic_record(&call->values, fields, 11);
}

typedef struct {
    char values[RECIPE_PARAM_MAX][256]; int bound[RECIPE_PARAM_MAX];
    char ignored[64][64]; size_t ignored_count;
} recipe_bindings_t;

static jdv_value_t recipe_run_result(semantic_builtin_t* call, const recipe_document_t* recipe,
        const recipe_bindings_t* bindings, const jash_checkpoint_result_t* checkpoint,
        size_t completed, size_t failed, size_t assertion, int status, int attempted, const char* error) {
    jdv_value_t parameters[RECIPE_PARAM_MAX], ignored[64];
    for (size_t i = 0; i < recipe->parameter_count; i++) {
        jdv_record_input_t fields[] = {
            semantic_field("name", semantic_symbol(&call->values, recipe->parameters[i])),
            semantic_field("value", semantic_text(&call->values, bindings->values[i])),
            semantic_field("bound", jdv_value_bool(bindings->bound[i]))
        };
        parameters[i] = semantic_record(&call->values, fields, 3);
    }
    for (size_t i = 0; i < bindings->ignored_count; i++) ignored[i] = semantic_symbol(&call->values, bindings->ignored[i]);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "run")),
        semantic_field("row_kind", semantic_symbol(&call->values, "result")),
        semantic_field("name", semantic_symbol(&call->values, recipe->name)),
        semantic_field("completed", jdv_value_bool(!status && completed == recipe->step_count)),
        semantic_field("steps_planned", jdv_value_u64(recipe->step_count)),
        semantic_field("steps_completed", jdv_value_u64(completed)),
        semantic_field("failed_commands", jdv_value_u64(failed)),
        semantic_field("assertion_failed_index", assertion ? jdv_value_u64(assertion) : jdv_value_nothing()),
        semantic_field("command_status", jdv_value_i64(status)),
        semantic_field("checkpoint_name", semantic_symbol(&call->values, checkpoint->applied ? checkpoint->name : NULL)),
        semantic_field("bindings", semantic_list(&call->values, parameters, recipe->parameter_count)),
        semantic_field("ignored_binding_names", semantic_list(&call->values, ignored, bindings->ignored_count)),
        semantic_field("binding_policy", semantic_symbol(&call->values, "declared-only-last-wins-missing-empty")),
        semantic_field("substitution", semantic_symbol(&call->values, "literal-text-before-shell-evaluation")),
        semantic_field("failure_policy", semantic_symbol(&call->values, "abort-assertion-continue-command")),
        semantic_field("output_destination", semantic_symbol(&call->values, "stderr")),
        semantic_field("effects_may_have_applied", jdv_value_bool(attempted)),
        semantic_field("error", semantic_symbol(&call->values, error))
    };
    return semantic_record(&call->values, fields, 19);
}

/* Execute in the current shell, preserving its variables/cwd and ordinary
 * command semantics. Structured stdout is reserved for semantic receipts;
 * the existing evaluator's output (builtins and children) goes to stderr. */
static int recipe_execute(jash_state_t* state, const char* command, int* status, int* attempted) {
    jash_eval_observation_t observed;
    int result = jash_eval_with_output(state, command, STDERR_FILENO, -1, &observed);
    *status = observed.command_status; *attempted = observed.attempted;
    return result;
}

static int recipe_run_semantic(semantic_builtin_t* call, jash_state_t* state,
                               recipe_document_t* recipe, int argc, char** argv) {
    recipe_bindings_t bindings = {0};
    for (int i = 0; i < argc; i++) {
        const char* equal = strchr(argv[i], '=');
        if (!equal || !recipe_parameter_valid(argv[i], (size_t)(equal - argv[i])) || strlen(equal + 1) > 255u)
            return semantic_error(call, "invalid_binding", 2);
        size_t size = (size_t)(equal - argv[i]); int bound = 0;
        for (size_t p = 0; p < recipe->parameter_count; p++) {
            if (strlen(recipe->parameters[p]) == size && !memcmp(recipe->parameters[p], argv[i], size)) {
                memcpy(bindings.values[p], equal + 1, strlen(equal + 1) + 1u); bindings.bound[p] = 1; bound = 1; break;
            }
        }
        if (!bound) {
            if (bindings.ignored_count == 64u) return semantic_error(call, "binding_limit", 75);
            memcpy(bindings.ignored[bindings.ignored_count], argv[i], size);
            bindings.ignored[bindings.ignored_count++][size] = 0;
        }
    }
    for (size_t i = 0; i < recipe->step_count; i++) {
        for (size_t p = 0; p < recipe->parameter_count; p++) {
            char needle[68], expanded[RECIPE_COMMAND_MAX + 1u];
            snprintf(needle, sizeof(needle), "${%s}", recipe->parameters[p]);
            if (recipe_replace(recipe->commands[i], needle, bindings.values[p], expanded, sizeof(expanded)))
                return semantic_error(call, "expanded_command_limit", 75);
            memcpy(recipe->commands[i], expanded, strlen(expanded) + 1u);
        }
    }
    char name[64]; snprintf(name, sizeof(name), "recipe-%s", recipe->name);
    jash_checkpoint_result_t checkpoint;
    if (jash_checkpoint_prepare(name, &checkpoint)) return semantic_error(call, checkpoint.error, 1);
    jash_checkpoint_result_t worst = checkpoint;
    worst.applied = 1; worst.state_observed = 1; worst.depth = worst.timestamp = INT64_MAX;
    worst.error_number = INT_MAX; worst.error = "checkpoint_directory_sync_failed";
    jdv_value_t rows[RECIPE_STEPS_MAX + 2u];
    rows[0] = recipe_checkpoint_row(call, &worst);
    for (size_t i = 0; i < recipe->step_count; i++)
        rows[i + 1u] = recipe_step_row(call, recipe, i, INT_MIN, 1, "execution_output_redirect_failed");
    rows[recipe->step_count + 1u] = recipe_run_result(call, recipe, &bindings, &worst, recipe->step_count,
        recipe->step_count, recipe->step_count, INT_MIN, 1, "checkpoint_directory_sync_failed");
    int invalid = semantic_events_preflight(call, rows, recipe->step_count + 2u);
    call->values.arena.used = 0; call->values.error = 0;
    if (invalid) return semantic_error(call, "output_or_value_limit", invalid);
    if (semantic_admit_rows(call, recipe->step_count + 2u)) return 125;
    recipe_control_t control = {.state = state};
    size_t completed = 0, failed = 0, assertion = 0;
    int status = 0, effects = 0; const char* error = NULL;
    if (recipe_poll(&control)) { status = control.cancelled; error = "cancelled"; }
    else {
        effects = 1; status = jash_checkpoint_create_prepared(&checkpoint); error = checkpoint.error;
        if (semantic_events_emit(call, recipe_checkpoint_row(call, &checkpoint))) return 125;
        call->values.arena.used = 0;
    }
    for (size_t i = 0; !status && i < recipe->step_count; i++) {
        if (recipe_poll(&control)) { status = control.cancelled; error = "cancelled"; break; }
        int command_status = 0, attempted = 0;
        int adapter_failed = recipe_execute(state, recipe->commands[i], &command_status, &attempted);
        const char* step_error = adapter_failed ? "execution_output_redirect_failed" : NULL;
        if (attempted) { completed++; if (command_status) failed++; }
        if (semantic_events_emit(call, recipe_step_row(call, recipe, i, command_status, attempted, step_error))) return 125;
        call->values.arena.used = 0;
        if (adapter_failed) { status = 125; error = step_error; }
        else if (recipe->kinds[i] == 'A' && command_status) {
            assertion = i + 1u; status = command_status; error = "assertion_failed";
        }
    }
    if (control.cancelled) call->cancellation_status = control.cancelled;
    if (semantic_events_emit(call, recipe_run_result(call, recipe, &bindings, &checkpoint,
                             completed, failed, assertion, status, effects, error))) return 125;
    return semantic_events_end(call, status);
}

static int recipe_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = argc ? argv[0] : "list";
    if (!strcmp(operation, "list")) {
        if (argc > 1) return semantic_error(call, "invalid_arguments", 2);
        return recipe_list_semantic(call, state);
    }
    if ((strcmp(operation, "show") && strcmp(operation, "forget") && strcmp(operation, "run")) || argc < 2 ||
        (strcmp(operation, "run") && argc != 2) || !recipe_name_valid(argv[1]))
        return semantic_error(call, "invalid_arguments", 2);
    recipe_document_t* recipe = calloc(1, sizeof(*recipe));
    if (!recipe) return semantic_error(call, "allocation_failed", 125);
    int status = recipe_document_read(recipe, argv[1]);
    int result;
    if (status) result = semantic_error(call, "recipe_observation_failed", status);
    else if (!recipe->present) result = semantic_error(call, "recipe_not_found", 2);
    else if (!strcmp(operation, "run")) result = recipe_run_semantic(call, state, recipe, argc - 2, argv + 2);
    else if (!strcmp(operation, "show")) {
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
            semantic_field("operation", semantic_symbol(&call->values, "show")),
            semantic_field("recipe", recipe_value(&call->values, recipe, 1))
        };
        result = semantic_finish(call, semantic_record(&call->values, fields, 3), 0);
    } else {
        memory_write_t worst = {.applied = 1, .error = INT_MAX};
        jdv_value_t proposed = recipe_forget_result(call, recipe, &worst, 1, 1);
        int invalid = semantic_preflight(call, &proposed);
        call->values.arena.used = 0; call->values.error = 0;
        if (invalid) result = semantic_error(call, "output_or_value_limit", invalid);
        else if (semantic_admit(call)) result = 125;
        else {
            recipe_control_t control = {.state = state};
            memory_write_t publication = recipe_publish(recipe, NULL, 1, &control);
            if (control.cancelled) call->cancellation_status = control.cancelled;
            status = control.cancelled ? control.cancelled : publication.error ? 1 : 0;
            result = semantic_finish(call, recipe_forget_result(call, recipe, &publication, 1, 0), status);
        }
    }
    recipe_document_free(recipe); free(recipe); return result;
}
