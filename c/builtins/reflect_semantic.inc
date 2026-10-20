// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_store.h"

static int reflect_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    int improve = argc && !strcmp(argv[0], "improve");
    int apply = improve && argc > 1 && !strcmp(argv[1], "apply");
    if ((!improve && argc != 1) || (improve && ((!apply && (argc != 2 || strcmp(argv[1], "suggest"))) || (apply && argc != 3))))
        return semantic_error(call, "invalid_arguments", 2);
    const char* question = apply ? argv[2] : improve ? NULL : argv[0];
    if (question && (!*question || strlen(question) > 4095u)) return semantic_error(call, "question_limit", 2);
    semantic_value_t* sem = &call->values;
    if (apply) {
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(sem, call->schema_id)),
            semantic_field("operation", semantic_symbol(sem, "improve-apply")),
            semantic_field("suggestion", semantic_text(sem, question)),
            semantic_field("status", semantic_symbol(sem, "proposal-only")),
            semantic_field("applied", jdv_value_bool(0)), semantic_field("recorded", jdv_value_bool(0))
        };
        return semantic_finish(call, semantic_record(sem, fields, 6), 0);
    }
    char timestamp[64], date[64], path[4096], mistakes[4096], suffix[128];
    if (semantic_timestamp(timestamp, date) || semantic_home_path(mistakes, sizeof(mistakes), "mistakes.log"))
        return semantic_error(call, "source_unavailable", 1);
    snprintf(suffix, sizeof(suffix), "reflections/%s.txt", date);
    if (semantic_home_path(path, sizeof(path), suffix)) return semantic_error(call, "store_path_limit", 1);
    uint64_t mistake_count = 0, reflections = 0; int mistakes_exist, reflections_exist;
    if (semantic_store_line_count(mistakes, NULL, &mistake_count, &mistakes_exist) ||
        semantic_store_line_count(path, "---", &reflections, &reflections_exist))
        return semantic_error(call, "source_read_failed", 1);
    jash_session_tokens_t tokens = {0};
    int counters = jash_session_tokens_readonly(&tokens, NULL) == 0 && tokens.input_tokens >= 0 && tokens.output_tokens >= 0 &&
        tokens.total_commands >= 0 && tokens.total_ms >= 0;
    const char* focus = "general-session-review";
    if (question) {
        if (strstr(question, "performance") || strstr(question, "speed") || strstr(question, "lento")) focus = "performance";
        else if (strstr(question, "error") || strstr(question, "bug") || strstr(question, "erro")) focus = "errors";
        else if (strstr(question, "token") || strstr(question, "cost") || strstr(question, "budget")) focus = "token-economy";
    }
    static const char* suggestions[] = {"review stored mistakes", "use token optimize", "reuse attested recall", "query state snapshots", "inspect context maps"};
    jdv_value_t advice[5]; for (size_t i = 0; i < 5; i++) advice[i] = semantic_text(sem, suggestions[i]);
    jdv_record_input_t analysis_fields[] = {
        semantic_field("question", semantic_text(sem, question)),
        semantic_field("focus", semantic_symbol(sem, focus)),
        semantic_field("analysis_engine", semantic_symbol(sem, "keyword-and-counter-heuristics")),
        semantic_field("token_estimator", semantic_symbol(sem, "bytes-divided-by-four")),
        semantic_field("counters_available", jdv_value_bool(counters)),
        semantic_field("commands", counters ? jdv_value_i64(tokens.total_commands) : jdv_value_nothing()),
        semantic_field("input_tokens_estimated", counters ? jdv_value_i64(tokens.input_tokens) : jdv_value_nothing()),
        semantic_field("output_tokens_estimated", counters ? jdv_value_i64(tokens.output_tokens) : jdv_value_nothing()),
        semantic_field("execution_ms", counters ? jdv_value_i64(tokens.total_ms) : jdv_value_nothing()),
        semantic_field("mistake_log_lines", jdv_value_u64(mistake_count)),
        semantic_field("reflections_today_before", jdv_value_u64(reflections)),
        semantic_field("suggestions", semantic_list(sem, advice, 5))
    };
    jdv_value_t analysis = semantic_record(sem, analysis_fields, 12);
    if (improve) {
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(sem, call->schema_id)),
            semantic_field("operation", semantic_symbol(sem, "improve-suggest")),
            semantic_field("analysis", analysis), semantic_field("recorded", jdv_value_bool(0))
        };
        return semantic_finish(call, semantic_record(sem, fields, 4), 0);
    }
    char* quoted = semantic_quoted(question);
    if (!quoted) return semantic_error(call, "invalid_store_text", 2);
    size_t capacity = strlen(quoted) + 384u; char* record = malloc(capacity);
    if (!record) { free(quoted); return semantic_error(call, "allocation_failed", 125); }
    int length = snprintf(record, capacity,
        "---\nTime: %s\nQuestion: %s\nCommands: %ld\nTokens: %ld in, %ld out\nMistakes: %llu\n",
        timestamp, quoted, tokens.total_commands, tokens.input_tokens, tokens.output_tokens, (unsigned long long)mistake_count);
    free(quoted);
    if (length < 0 || (size_t)length >= capacity) { free(record); return semantic_error(call, "store_record_limit", 75); }
    int preflight = semantic_store_preflight(call, "reflect", path, analysis);
    if (preflight) { free(record); return semantic_error(call, "receipt_limit_before_store", preflight); }
    if (semantic_admit(call)) { free(record); return 125; }
    int failed = semantic_store_directory("reflections") || semantic_write_file(path, record, (size_t)length, 1);
    free(record);
    return semantic_store_receipt(call, "reflect", path, analysis, failed ? 1 : 0);
}
