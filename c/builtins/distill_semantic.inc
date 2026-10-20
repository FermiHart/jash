// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "recipe_store.h"

typedef struct {
    const char* name; uint64_t from, to; int have_from, have_to, keep_reads, explain;
    char parameter[RECIPE_PARAM_MAX][64]; const char* literal[RECIPE_PARAM_MAX]; size_t count;
} distill_options_t;
typedef struct {
    const memory_chain_entry_t* entry; const char* reason;
    int kept, mutation, fix, assertion;
} distill_decision_t;

static int distill_options(int argc, char** argv, distill_options_t* options) {
    memset(options, 0, sizeof(*options)); int ended = 0;
    for (int i = 0; i < argc; i++) {
        const char* token = argv[i];
        if (!ended && !strcmp(token, "--")) { ended = 1; continue; }
        if (!ended && !strcmp(token, "--keep-reads")) {
            if (options->keep_reads) return -1;
            options->keep_reads = 1; continue;
        }
        if (!ended && !strcmp(token, "--explain")) {
            if (options->explain) return -1;
            options->explain = 1; continue;
        }
        if (!ended && (!strcmp(token, "--from") || !strcmp(token, "--to"))) {
            int from = !strcmp(token, "--from"); uint64_t value;
            if (++i == argc || memory_unsigned(argv[i], UINT64_MAX, &value)) return -1;
            if (from) { if (options->have_from) return -1; options->have_from = 1; options->from = value; }
            else { if (options->have_to) return -1; options->have_to = 1; options->to = value; }
            continue;
        }
        if (!ended && !strcmp(token, "--param")) {
            if (++i == argc || options->count == RECIPE_PARAM_MAX) return -1;
            const char* equals = strchr(argv[i], '=');
            if (!equals || !recipe_parameter_valid(argv[i], (size_t)(equals - argv[i])) ||
                strlen(equals + 1) > 255u || strpbrk(equals + 1, "\r\n")) return -1;
            size_t length = (size_t)(equals - argv[i]);
            for (size_t j = 0; j < options->count; j++)
                if (strlen(options->parameter[j]) == length && !memcmp(options->parameter[j], argv[i], length)) return -1;
            memcpy(options->parameter[options->count], argv[i], length);
            options->parameter[options->count][length] = 0;
            options->literal[options->count++] = equals + 1; continue;
        }
        if (options->name || (!ended && token[0] == '-') || !recipe_name_valid(token)) return -1;
        options->name = token;
    }
    return !options->name || (options->have_from && options->have_to && options->from > options->to) ? -1 : 0;
}

static jdv_value_t distill_result(semantic_builtin_t* call, const memory_chain_t* chain,
        const recipe_document_t* candidate, const distill_decision_t* decisions, size_t count,
        size_t timeline_observed, const memory_write_t* publication, size_t serialized,
        const distill_options_t* options, int attempted, int worst) {
    semantic_value_t* sem = &call->values; jdv_value_t values[RECIPE_STEPS_MAX];
    uint64_t fixes = 0, mutations = 0, assertions = 0, dropped = 0;
    for (size_t i = 0; i < count; i++) {
        const distill_decision_t* decision = &decisions[i];
        if (!strcmp(decision->reason, "fix")) fixes++;
        else if (!strcmp(decision->reason, "mutation")) mutations++;
        else if (!strcmp(decision->reason, "assertion")) assertions++;
        else if (!strcmp(decision->reason, "read-dropped")) dropped++;
        jdv_record_input_t fields[] = {
            semantic_field("sequence", jdv_value_u64(decision->entry->sequence)),
            semantic_field("command_recorded", semantic_text(sem, decision->entry->command)),
            semantic_field("status_recorded", jdv_value_i64(decision->entry->status)),
            semantic_field("kept", jdv_value_bool(decision->kept)),
            semantic_field("reason", semantic_symbol(sem, decision->reason)),
            semantic_field("mutation_in_time_window", jdv_value_bool(decision->mutation)),
            semantic_field("similar_prior_failure", jdv_value_bool(decision->fix)),
            semantic_field("assertion_prefix", jdv_value_bool(decision->assertion))
        };
        values[i] = semantic_record(sem, fields, 8);
    }
    jdv_record_input_t classification[] = {
        semantic_field("engine", semantic_symbol(sem, "legacy-step-heuristics")),
        semantic_field("causation_verified", jdv_value_bool(0)),
        semantic_field("fix_similarity_numerator", jdv_value_u64(1)),
        semantic_field("fix_similarity_denominator", jdv_value_u64(2)),
        semantic_field("last_window_seconds", jdv_value_u64(3600)),
        semantic_field("timeline_mutations_observed", jdv_value_u64(timeline_observed)),
        semantic_field("timeline_scope", semantic_symbol(sem, "current-loaded-ring")),
        semantic_field("fixes", jdv_value_u64(fixes)),
        semantic_field("mutations", jdv_value_u64(mutations)),
        semantic_field("assertions", jdv_value_u64(assertions)),
        semantic_field("reads_dropped", jdv_value_u64(dropped))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, "distill")),
        semantic_field("ok", jdv_value_bool(!publication->error)),
        semantic_field("source", memory_chain_source(sem, chain)),
        semantic_field("candidate", recipe_value(sem, candidate, 2)),
        semantic_field("classification", semantic_record(sem, classification, 11)),
        semantic_field("decisions", semantic_list(sem, values, count)),
        semantic_field("keep_reads", jdv_value_bool(options->keep_reads)),
        semantic_field("explain_requested", jdv_value_bool(options->explain)),
        semantic_field("substitution", semantic_symbol(sem, "literal-text-to-declared-placeholder")),
        semantic_field("publication", recipe_publication(sem, candidate, publication, serialized, worst)),
        semantic_field("effects_may_have_applied", jdv_value_bool(attempted)),
        semantic_field("error", semantic_symbol(sem, worst ? "recipe_directory_sync_failed" : publication->error ? memory_store_error(publication->error) : NULL))
    };
    return semantic_record(sem, fields, 13);
}

static int distill_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    distill_options_t options;
    if (distill_options(argc, argv, &options)) return semantic_error(call, "invalid_arguments", 2);
    recipe_control_t control = {.state = state};
    if (recipe_poll(&control)) return semantic_cancelled(call, control.cancelled);
    memory_chain_t chain;
    const char* global = getenv("JASH_RECALL_GLOBAL");
    if (memory_chain_load(&chain, global && !strcmp(global, "1"), 0)) {
        int status = chain.error == E2BIG ? 75 : 1;
        memory_chain_free(&chain); return semantic_error(call, "chain_observation_failed", status);
    }
    size_t selected[RECIPE_STEPS_MAX], count = 0;
    int status = 0; const char* error = NULL;
    size_t start = !options.have_from && !options.have_to && chain.count > DIST_DEFAULT_RANGE ? chain.count - DIST_DEFAULT_RANGE : 0;
    for (size_t i = start; i < chain.count; i++) {
        const memory_chain_entry_t* entry = &chain.entries[i];
        if ((options.have_from && entry->sequence < options.from) || (options.have_to && entry->sequence > options.to)) continue;
        if (count == RECIPE_STEPS_MAX || strlen(entry->command) > RECIPE_COMMAND_MAX) { status = 75; error = "chain_selection_limit"; break; }
        if (strpbrk(entry->command, "\r\n") || (count && (entry->seconds < chain.entries[selected[count - 1u]].seconds ||
            entry->sequence <= chain.entries[selected[count - 1u]].sequence))) { status = 1; error = "chain_order_or_command_unrepresentable"; break; }
        char scratch[RECIPE_COMMAND_MAX + 1u]; char* tokens[65];
        int token_count = dist_tok(entry->command, scratch, sizeof(scratch), tokens, 65);
        if (token_count < 0 || token_count >= 65) { status = 75; error = "command_token_limit"; break; }
        selected[count++] = i;
    }
    if (!status && !count) { status = 2; error = "empty_chain_range"; }
    recipe_document_t* before = NULL; recipe_document_t* candidate = NULL;
    if (!status) {
        before = calloc(1, sizeof(*before)); candidate = calloc(1, sizeof(*candidate));
        if (!before || !candidate) { status = 125; error = "allocation_failed"; }
    }
    if (!status) {
        status = recipe_document_read(before, options.name);
        if (status) error = "recipe_observation_failed";
    }
    long mutation_times[JASH_TIMELINE_MAX]; size_t mutation_count = 0;
    for (int i = 0; !status && i < JASH_TIMELINE_MAX; i++) {
        int kind; const char *a = NULL, *b = NULL; long when = 0;
        if (!jash_timeline_peek2(i, &kind, &a, &b, &when)) break;
        if (kind == JASH_UNDO_FILE_OVERWRITE || kind == JASH_UNDO_FILE_CREATE ||
            kind == JASH_UNDO_TRASH || kind == JASH_UNDO_MOVE) mutation_times[mutation_count++] = when;
    }
    distill_decision_t decisions[RECIPE_STEPS_MAX]; memset(decisions, 0, sizeof(decisions));
    if (!status) {
        memcpy(candidate->name, options.name, strlen(options.name) + 1u);
        memcpy(candidate->path, before->path, strlen(before->path) + 1u);
        time_t now = time(NULL);
        const char* session = getenv("JASH_SESSION");
        if (!session) session = getenv("CLAUDE_CODE_SESSION_ID");
        if (!session) session = "-";
        if (now < 0 || !*session || strlen(session) >= sizeof(candidate->session) || strpbrk(session, "\r\n")) {
            status = 2; error = "recipe_metadata_unrepresentable";
        } else {
            candidate->created = (int64_t)now; memcpy(candidate->session, session, strlen(session) + 1u);
            candidate->from = chain.entries[selected[0]].sequence; candidate->to = chain.entries[selected[count - 1u]].sequence;
            candidate->parameter_count = options.count;
            for (size_t i = 0; i < options.count; i++) memcpy(candidate->parameters[i], options.parameter[i], strlen(options.parameter[i]) + 1u);
        }
    }
    for (size_t i = 0; !status && i < count; i++) {
        if (recipe_poll(&control)) { status = control.cancelled; error = "cancelled"; break; }
        size_t index = selected[i]; const memory_chain_entry_t* entry = &chain.entries[index];
        distill_decision_t* decision = &decisions[i]; decision->entry = entry;
        decision->reason = "bookkeeping-skipped";
        if (!entry->command[0] || !strncmp(entry->command, "distill", 7) || !strncmp(entry->command, "recipe", 6)) continue;
        uint64_t until;
        if (index + 1u < chain.count) until = chain.entries[index + 1u].seconds;
        else if (entry->seconds > UINT64_MAX - 3600u) { status = 75; error = "timestamp_range"; break; }
        else until = entry->seconds + 3600u;
        if (until < entry->seconds) { status = 1; error = "chain_order_or_command_unrepresentable"; break; }
        for (size_t m = 0; m < mutation_count; m++)
            if (mutation_times[m] >= 0 && (uint64_t)mutation_times[m] >= entry->seconds && (uint64_t)mutation_times[m] < until) decision->mutation = 1;
        decision->assertion = is_assertion(entry->command);
        if (entry->status == 0) {
            char scratch[RECIPE_COMMAND_MAX + 1u]; char* tokens[64];
            int tokens_count = dist_tok(entry->command, scratch, sizeof(scratch), tokens, 64);
            for (size_t j = 0; j < i; j++) {
                const memory_chain_entry_t* prior = &chain.entries[selected[j]];
                if (!prior->status) continue;
                char previous[RECIPE_COMMAND_MAX + 1u]; char* old_tokens[64];
                int old_count = dist_tok(prior->command, previous, sizeof(previous), old_tokens, 64);
                if (jash_chain_jaccard(tokens, tokens_count, old_tokens, old_count) >= DIST_FIX_SIM) { decision->fix = 1; break; }
            }
        }
        decision->kept = decision->assertion || decision->fix || decision->mutation || options.keep_reads;
        decision->reason = decision->assertion ? "assertion" : decision->fix ? "fix" : decision->mutation ? "mutation" :
            options.keep_reads ? "read-kept" : "read-dropped";
        if (!decision->kept) continue;
        char command[RECIPE_COMMAND_MAX + 1u]; memcpy(command, entry->command, strlen(entry->command) + 1u);
        for (size_t p = 0; p < options.count; p++) {
            char placeholder[68], replaced[RECIPE_COMMAND_MAX + 1u];
            snprintf(placeholder, sizeof(placeholder), "${%s}", options.parameter[p]);
            if (recipe_replace(command, options.literal[p], placeholder, replaced, sizeof(replaced))) { status = 75; error = "expanded_command_limit"; break; }
            memcpy(command, replaced, strlen(replaced) + 1u);
        }
        if (!status) {
            candidate->kinds[candidate->step_count] = decision->assertion ? 'A' : 'C';
            memcpy(candidate->commands[candidate->step_count++], command, strlen(command) + 1u);
        }
    }
    if (!status && !candidate->step_count) { status = 2; error = "no_load_bearing_steps"; }
    jash_structured_buffer_t bytes = {0};
    if (!status && recipe_serialize(candidate, &bytes)) { status = 75; error = "recipe_serialization_limit"; }
    if (!status) {
        memory_write_t worst = {.applied = 1, .error = INT_MAX};
        jdv_value_t proposed = distill_result(call, &chain, candidate, decisions, count, mutation_count, &worst, bytes.used, &options, 1, 1);
        status = semantic_preflight(call, &proposed);
        if (status) error = "output_or_value_limit";
        call->values.arena.used = 0; call->values.error = 0;
    }
    int result;
    if (status) result = control.cancelled ? semantic_cancelled(call, control.cancelled) : semantic_error(call, error, status);
    else if (semantic_admit(call)) result = 125;
    else {
        memory_write_t publication = recipe_publish(before, &bytes, 0, &control);
        if (control.cancelled) call->cancellation_status = control.cancelled;
        status = control.cancelled ? control.cancelled : publication.error ? 1 : 0;
        result = semantic_finish(call, distill_result(call, &chain, candidate, decisions, count, mutation_count,
            &publication, bytes.used, &options, 1, 0), status);
    }
    jash_structured_buffer_free(&bytes); memory_chain_free(&chain);
    if (before) recipe_document_free(before);
    free(before); free(candidate); return result;
}
