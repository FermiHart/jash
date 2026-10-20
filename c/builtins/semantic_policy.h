// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_SEMANTIC_POLICY_H
#define JASH_BUILTINS_SEMANTIC_POLICY_H
#include "semantic_builtin.h"
#include "semantic_store.h"
#include <core/adopt.h>
#include <core/guard.h>
#include <core/jash_invoice.h>
#include <core/stance.h>
#include <core/token_tracker.h>
#include <stdio.h>

static inline int semantic_u64(const char* text, uint64_t* value) {
    if (!text || !*text) return -1;
    uint64_t number = 0;
    for (; *text; text++) {
        unsigned digit = (unsigned char)*text - '0';
        if (digit > 9 || number > (UINT64_MAX - digit) / 10u) return -1;
        number = number * 10u + digit;
    }
    *value = number; return 0;
}

static inline int semantic_policy(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    if (argc > 1) return semantic_error(call, "invalid_arguments", 2);
    const char* operation = argc ? argv[0] : "status";
    int is_adopt = !strcmp(call->command, "adopt"), change = strcmp(operation, "status") != 0;
    int status = 0, requested = -2;
    if (change) {
        if (is_adopt) {
            if (!strcmp(operation, "off")) requested = 0;
            else if (!strcmp(operation, "on") || !strcmp(operation, "nudge")) requested = 1;
            else if (!strcmp(operation, "rewrite")) requested = 2;
            else if (!strcmp(operation, "max")) requested = 3;
            else if (!strcmp(operation, "clear")) requested = -1;
        } else {
            if (!strcmp(operation, "off")) requested = 0;
            else if (!strcmp(operation, "on") || !strcmp(operation, "relaxed")) requested = 1;
            else if (!strcmp(operation, "strict")) requested = 2;
        }
        if (requested == -2) return semantic_error(call, "invalid_operation", 2);
        if (semantic_admit(call)) return 125;
        status = is_adopt ? jash_adopt_set(requested) : jash_guard_bypass_set(requested != 0, requested == 2);
        status = status ? 1 : 0;
    }
    unsigned total = 0, native = 0, bypass = 0, blocked = 0, upgraded = 0;
    jash_guard_metrics_snapshot(&total, &native, &bypass, &blocked, &upgraded);
    semantic_value_t* sem = &call->values;
    int level = jash_adopt_level();
    static const char* levels[] = {"off", "nudge", "rewrite", "aggressive"};
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("changed", status && change ? jdv_value_nothing() : jdv_value_bool(change)),
        semantic_field("error", semantic_symbol(sem, status ? "policy_write_failed" : NULL)),
        semantic_field("total", jdv_value_u64(total)), semantic_field("native", jdv_value_u64(native)),
        semantic_field("bypass", jdv_value_u64(bypass)), semantic_field("blocked", jdv_value_u64(blocked)),
        semantic_field("upgraded", jdv_value_u64(upgraded)),
        semantic_field("pct", total ? jdv_value_u64(((uint64_t)native * 100u + total / 2u) / total) : jdv_value_nothing()),
        semantic_field("metrics_scope", semantic_symbol(sem, "home-cumulative")),
        semantic_field("monitoring", jdv_value_bool(jash_guard_bypass_active())),
        semantic_field("strict", jdv_value_bool(jash_guard_bypass_strict())),
        semantic_field("level", jdv_value_i64(level)),
        semantic_field("level_name", semantic_symbol(sem, level >= 0 && level < 4 ? levels[level] : "unknown")),
        semantic_field("source", semantic_symbol(sem, jash_adopt_source_name(jash_adopt_source())))
    };
    jdv_record_input_t report[19]; size_t count = is_adopt ? 17 : 14;
    memcpy(report, fields, count * sizeof(*fields));
    report[count++] = semantic_field("effects_may_have_applied", jdv_value_bool(change));
    report[count++] = semantic_field("persisted", change ? jdv_value_bool(!status) : jdv_value_nothing());
    return semantic_finish(call, semantic_record(sem, report, count), status);
}

static inline jdv_value_t semantic_stance_record(semantic_value_t* sem, jash_stance_t id) {
    const jash_stance_cfg_t* cfg = stance_cfg(id);
    if (!cfg) { sem->error = 1; return jdv_value_nothing(); }
    jdv_value_t tags[8];
    if (cfg->n_priority_tags < 0 || cfg->n_priority_tags > 8) { sem->error = 1; return jdv_value_nothing(); }
    for (int i = 0; i < cfg->n_priority_tags; i++) tags[i] = jdv_value_i64(cfg->journal_priority_tags[i]);
    jdv_record_input_t fields[] = {
        semantic_field("id", jdv_value_i64(id)), semantic_field("name", semantic_symbol(sem, cfg->name)),
        semantic_field("description", semantic_text(sem, cfg->description)),
        semantic_field("active", jdv_value_bool(id == stance_get())),
        semantic_field("context_budget_tokens", jdv_value_u64(cfg->context_budget)),
        semantic_field("digest_level", semantic_symbol(sem, cfg->digest_level == SESH_BRIEF ? "brief" : cfg->digest_level == SESH_STANDARD ? "standard" : "deep")),
        semantic_field("priority_tags", semantic_list(sem, tags, (size_t)cfg->n_priority_tags))
    };
    return semantic_record(sem, fields, 7);
}

static inline int semantic_stance(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "show";
    int change = !strcmp(operation, "set"), status = 0;
    if (change) {
        if (argc != 2) return semantic_error(call, "invalid_arguments", 2);
        jash_stance_t id = stance_from_name(argv[1]);
        if (id < 0 || id >= JASH_STANCE_COUNT) return semantic_error(call, "unknown_stance", 2);
        if (semantic_admit(call)) return 125;
        status = stance_set(id) ? 1 : 0;
    } else if ((argc > 1) || (strcmp(operation, "list") && strcmp(operation, "show"))) return semantic_error(call, "invalid_arguments", 2);
    semantic_value_t* sem = &call->values;
    jdv_value_t items[JASH_STANCE_COUNT]; size_t count = 0;
    if (!strcmp(operation, "list")) {
        for (int i = 0; i < JASH_STANCE_COUNT; i++) items[count++] = semantic_stance_record(sem, (jash_stance_t)i);
    } else items[count++] = semantic_stance_record(sem, stance_get());
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("scope", semantic_symbol(sem, "shell-process")),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("changed", jdv_value_bool(change && !status)),
        semantic_field("stances", semantic_list(sem, items, count))
    };
    return semantic_finish(call, semantic_record(sem, fields, 6), status);
}

static inline jdv_value_t semantic_invoice_stats(semantic_value_t* sem, const jash_invoice_stats_t* stats) {
    static const char* categories[] = {"recall", "digest", "state", "guard", "brain", "bypass"};
    jdv_record_input_t saved[JASH_INVOICE_CATEGORY_COUNT];
    for (int i = 0; i < JASH_INVOICE_CATEGORY_COUNT; i++) saved[i] = semantic_field(categories[i], jdv_value_u64(stats->saved[i]));
    jdv_record_input_t fields[] = {
        semantic_field("saved_tokens_estimated", semantic_record(sem, saved, JASH_INVOICE_CATEGORY_COUNT)),
        semantic_field("commands_predicted", jdv_value_u64(stats->commands_predicted)),
        semantic_field("suggestions_offered", jdv_value_u64(stats->suggestions_offered)),
        semantic_field("suggestions_accepted", jdv_value_u64(stats->suggestions_accepted)),
        semantic_field("guard_blocks", jdv_value_u64(stats->guard_blocks)),
        semantic_field("naive_input_tokens_estimated", jdv_value_u64(stats->naive_input_tokens)),
        semantic_field("jash_input_tokens_estimated", jdv_value_u64(stats->jash_input_tokens))
    };
    return semantic_record(sem, fields, 7);
}

static inline int semantic_invoice(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "status";
    int change = strcmp(operation, "status") != 0, status = 0;
    if ((!strcmp(operation, "begin") && (argc < 1 || argc > 2 || (argc == 2 && strlen(argv[1]) >= 256))) ||
        (strcmp(operation, "begin") && argc > 1) ||
        (strcmp(operation, "begin") && strcmp(operation, "status") && strcmp(operation, "close") && strcmp(operation, "reset")))
        return semantic_error(call, "invalid_arguments", 2);
    if (change && semantic_admit(call)) return 125;
    if (change ? jash_invoice_init(NULL) : jash_invoice_init_readonly(NULL)) return semantic_error(call, "invoice_unavailable", 1);
    if (!strcmp(operation, "begin")) status = jash_invoice_begin(argc == 2 ? argv[1] : NULL) ? 1 : 0;
    else if (!strcmp(operation, "close")) status = jash_invoice_close(NULL) ? 1 : 0;
    else if (!strcmp(operation, "reset")) {
        memset((void*)jash_invoice_lifetime(), 0, sizeof(jash_invoice_stats_t));
        status = jash_invoice_save() ? 1 : 0;
    }
    semantic_value_t* sem = &call->values;
    const jash_invoice_session_t* current = jash_invoice_current();
    jdv_record_input_t session[] = {
        semantic_field("id", semantic_text(sem, current->id)),
        semantic_field("intent", semantic_text(sem, current->intent)),
        semantic_field("active", jdv_value_bool(current->active)),
        semantic_field("started_ns", jdv_value_u64(current->started_ns)),
        semantic_field("closed_ns", current->closed_ns ? jdv_value_u64(current->closed_ns) : jdv_value_nothing()),
        semantic_field("delta", semantic_invoice_stats(sem, &current->delta)),
        semantic_field("clock", semantic_symbol(sem, "monotonic"))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("changed", status && change ? jdv_value_nothing() : jdv_value_bool(change)),
        semantic_field("scope", semantic_symbol(sem, "home-invoice-store")),
        semantic_field("token_estimator", semantic_symbol(sem, "bytes-divided-by-four-with-native-discount")),
        semantic_field("lifetime", semantic_invoice_stats(sem, jash_invoice_lifetime())),
        semantic_field("session", semantic_record(sem, session, 7)),
        semantic_field("error", semantic_symbol(sem, status ? "invoice_operation_failed" : NULL)),
        semantic_field("effects_may_have_applied", jdv_value_bool(change)),
        semantic_field("persisted", change ? jdv_value_bool(!status) : jdv_value_nothing())
    };
    return semantic_finish(call, semantic_record(sem, fields, 11), status);
}

static inline int semantic_token(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "count";
    semantic_value_t* sem = &call->values;
    if (!strcmp(operation, "budget")) {
        uint64_t requested;
        if (argc != 2 || semantic_u64(argv[1], &requested) || !requested || requested > LONG_MAX)
            return semantic_error(call, "invalid_budget", 2);
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(sem, call->schema_id)),
            semantic_field("operation", semantic_symbol(sem, operation)),
            semantic_field("requested_budget", jdv_value_u64(requested)),
            semantic_field("applied", jdv_value_bool(0)),
            semantic_field("scope", semantic_symbol(sem, "proposal")),
            semantic_field("environment_variable", semantic_symbol(sem, "JASH_LLM_BUDGET"))
        };
        return semantic_finish(call, semantic_record(sem, fields, 6), 0);
    }
    if (argc > 1 || (strcmp(operation, "count") && strcmp(operation, "optimize") && strcmp(operation, "history")))
        return semantic_error(call, "invalid_arguments", 2);
    jash_session_tokens_t tokens = {0}; int exists = 0;
    int status = jash_session_tokens_readonly(&tokens, &exists) ? 1 : 0;
    jdv_value_t budget = jdv_value_nothing(); uint64_t number;
    const char* setting = getenv("JASH_LLM_BUDGET");
    if (setting && !semantic_u64(setting, &number) && number) budget = jdv_value_u64(number);
    jdv_record_input_t fields[16]; size_t count = 0;
    fields[count++] = semantic_field("schema", semantic_symbol(sem, call->schema_id));
    fields[count++] = semantic_field("operation", semantic_symbol(sem, operation));
    fields[count++] = semantic_field("source", semantic_symbol(sem, "home-token-usage-session"));
    fields[count++] = semantic_field("estimator", semantic_symbol(sem, "bytes-divided-by-four"));
    fields[count++] = semantic_field("available", jdv_value_bool(!status));
    fields[count++] = semantic_field("source_exists", jdv_value_bool(exists));
    fields[count++] = semantic_field("input_tokens_estimated", status ? jdv_value_nothing() : jdv_value_i64(tokens.input_tokens));
    fields[count++] = semantic_field("output_tokens_estimated", status ? jdv_value_nothing() : jdv_value_i64(tokens.output_tokens));
    fields[count++] = semantic_field("commands", status ? jdv_value_nothing() : jdv_value_i64(tokens.total_commands));
    fields[count++] = semantic_field("execution_ms", status ? jdv_value_nothing() : jdv_value_i64(tokens.total_ms));
    fields[count++] = semantic_field("budget", budget);
    if (!strcmp(operation, "optimize")) {
        static const char* advice[] = {"page large output", "use narrow search patterns", "reuse attested recall", "query structured state", "inspect context maps"};
        jdv_value_t list[5]; for (size_t i = 0; i < 5; i++) list[i] = semantic_text(sem, advice[i]);
        fields[count++] = semantic_field("suggestions", semantic_list(sem, list, 5));
        fields[count++] = semantic_field("suggestion_source", semantic_symbol(sem, "static-heuristic"));
    } else if (!strcmp(operation, "history")) {
        char path[4096]; const char* home = getenv("HOME");
        if (!home || snprintf(path, sizeof(path), "%s/.jash/token_usage.log", home) >= (int)sizeof(path))
            return semantic_error(call, "history_path_unavailable", 1);
        char* bytes = NULL; size_t size = 0; int history_exists = 0;
        if (semantic_read_file(path, 16u * 1024u * 1024u, &bytes, &size, &history_exists))
            return semantic_error(call, "history_limit_or_read_error", 75);
        jdv_value_t* entries = calloc(4096, sizeof(*entries));
        if (!entries) { free(bytes); return semantic_error(call, "allocation_failed", 125); }
        size_t used = 0, offset = 0; int failed = 0;
        while (offset < size) {
            size_t start = offset; while (offset < size && bytes[offset] != '\n') offset++;
            size_t length = offset - start; if (offset < size) offset++;
            if (used == 4096 || length > 4096) { failed = 1; break; }
            entries[used++] = semantic_text_n(sem, bytes + start, length);
        }
        fields[count++] = semantic_field("entries", semantic_list(sem, entries, used));
        fields[count++] = semantic_field("entry_format", semantic_symbol(sem, "stored-legacy-log-line"));
        free(entries); free(bytes);
        if (failed) return semantic_error(call, "history_limit_or_read_error", 75);
    }
    /* Missing optional session counters are explicitly unknown, not command failure. */
    return semantic_finish(call, semantic_record(sem, fields, count), 0);
}
#endif
