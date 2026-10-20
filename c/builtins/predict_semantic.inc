// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_store.h"

static int predict_pattern_score(semantic_value_t* sem, const char* path, const char* command,
                                 int once, uint64_t* matches, int* exists) {
    char* data = NULL; size_t bytes = 0, count = 0;
    if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, exists)) return -1;
    semantic_memory_row_t* rows = calloc(SEMANTIC_STORE_ROWS, sizeof(*rows));
    if (!rows) { free(data); return -1; }
    int failed = semantic_memory_parse(sem, data, bytes, 0, rows, &count);
    if (!failed) for (size_t i = 0; i < count; i++) {
        jdv_slice_t pattern = rows[i].content.as.slice;
        if (!pattern.len) continue;
        size_t length = strlen(command); int found = 0;
        for (size_t j = 0; j <= length && pattern.len <= length - j; j++)
            if (!memcmp(command + j, pattern.data, pattern.len)) { found = 1; break; }
        if (found) { (*matches)++; if (once) break; }
    }
    free(data); free(rows); return failed;
}

static int predict_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    int is_risk = !strcmp(call->command, "risk");
    if (!is_risk && argc && !strcmp(argv[0], "risk")) { is_risk = 1; argc--; argv++; }
    semantic_value_t* sem = &call->values;
    if (is_risk) {
        const char* operation = argc ? argv[0] : "assess";
        if (!strcmp(operation, "avoid")) {
            if (argc != 2) return semantic_error(call, "invalid_arguments", 2);
            char path[4096];
            if (semantic_home_path(path, sizeof(path), "risk_patterns.yaml")) return semantic_error(call, "store_path_limit", 1);
            return semantic_memory_append(call, operation, path, "", argv[1], NULL);
        }
        if (strcmp(operation, "assess") || argc > 1) return semantic_error(call, "invalid_arguments", 2);
        const char* home = getenv("HOME"); char path[4096];
        if (!home || snprintf(path, sizeof(path), "%s/.bash_history", home) >= (int)sizeof(path))
            return semantic_error(call, "history_path_unavailable", 1);
        char* data = NULL; size_t bytes = 0; int exists;
        if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, &exists)) return semantic_error(call, "history_read_failed", 1);
        if (bytes && memchr(data, 0, bytes)) { free(data); return semantic_error(call, "invalid_history_text", 1); }
        uint64_t total = 0, high = 0; size_t offset = 0;
        while (offset < bytes) {
            size_t start = offset; while (offset < bytes && data[offset] != '\n') offset++;
            if (offset < bytes) data[offset++] = 0;
            const char* line = data + start; total++;
            if (strstr(line, "rm -rf") || strstr(line, "rm -r") || strstr(line, "chmod 777") || strstr(line, "kill -9")) high++;
        }
        free(data);
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(sem, call->schema_id)),
            semantic_field("operation", semantic_symbol(sem, "assess")),
            semantic_field("engine", semantic_symbol(sem, "substring-heuristic")),
            semantic_field("source", semantic_path(sem, path)),
            semantic_field("available", jdv_value_bool(exists)),
            semantic_field("history_lines", exists ? jdv_value_u64(total) : jdv_value_nothing()),
            semantic_field("high_risk_commands", exists ? jdv_value_u64(high) : jdv_value_nothing()),
            semantic_field("profile", !exists ? jdv_value_nothing() : semantic_symbol(sem, high > 10 ? "excessive" : high > 5 ? "moderate" : "normal"))
        };
        return semantic_finish(call, semantic_record(sem, fields, 8), 0);
    }
    if (argc && !strcmp(argv[0], "--")) { argc--; argv++; }
    if (argc != 1 || !*argv[0] || strlen(argv[0]) > 4095u) return semantic_error(call, "expected_command_up_to_4095_bytes", 2);
    const char* command = argv[0];
    uint64_t fixed = (uint64_t)risk_fixed_score(command), avoid = 0, learned = 0, mistakes = 0, files = 0;
    int avoid_exists, mistake_exists;
    char path[4096];
    if (semantic_home_path(path, sizeof(path), "risk_patterns.yaml") ||
        predict_pattern_score(sem, path, command, 0, &avoid, &avoid_exists)) return semantic_error(call, "invalid_avoid_store", 1);
    char directory[4096];
    if (semantic_home_path(directory, sizeof(directory), "patterns")) return semantic_error(call, "store_path_limit", 1);
    DIR* dir = opendir(directory);
    if (!dir && errno != ENOENT) return semantic_error(call, "pattern_directory_read_failed", 1);
    int failed = 0;
    while (dir) {
        errno = 0; struct dirent* entry = readdir(dir);
        if (!entry) { if (errno) failed = 1; break; }
        if (!predict_yaml_file(entry->d_name)) continue;
        if (++files > SEMANTIC_STORE_ROWS) { failed = 1; break; }
        int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name), exists;
        if (n < 0 || (size_t)n >= sizeof(path) || predict_pattern_score(sem, path, command, 1, &learned, &exists) || !exists) { failed = 1; break; }
    }
    if (dir && closedir(dir)) failed = 1;
    if (failed) return semantic_error(call, "invalid_or_oversized_pattern_store", 1);
    char* data = NULL; size_t bytes = 0;
    if (semantic_home_path(path, sizeof(path), "mistakes.log") ||
        semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, &mistake_exists)) return semantic_error(call, "mistake_store_read_failed", 1);
    if (bytes && memchr(data, 0, bytes)) { free(data); return semantic_error(call, "invalid_mistake_store", 1); }
    size_t offset = 0;
    while (offset < bytes) {
        size_t start = offset; while (offset < bytes && data[offset] != '\n') offset++;
        if (offset < bytes) data[offset++] = 0;
        const char* line = data + start;
        if (*line && (strstr(line, command) || strstr(command, line))) mistakes++;
    }
    free(data);
    uint64_t score = fixed + avoid * 25u + learned * 10u + mistakes * 15u;
    if (score > 100) score = 100;
    jdv_record_input_t features[] = {
        semantic_field("fixed_pattern_score", jdv_value_u64(fixed)),
        semantic_field("avoid_matches", jdv_value_u64(avoid)), semantic_field("learned_file_matches", jdv_value_u64(learned)),
        semantic_field("mistake_line_matches", jdv_value_u64(mistakes)), semantic_field("pattern_files_observed", jdv_value_u64(files)),
        semantic_field("avoid_store_exists", jdv_value_bool(avoid_exists)), semantic_field("mistake_store_exists", jdv_value_bool(mistake_exists))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, "predict")),
        semantic_field("command", semantic_text(sem, command)),
        semantic_field("engine", semantic_symbol(sem, "substring-heuristic")),
        semantic_field("risk_score", jdv_value_u64(score)), semantic_field("score_scale", jdv_value_u64(100)),
        semantic_field("level", semantic_symbol(sem, risk_label((int)score))),
        semantic_field("failure_probability", jdv_value_nothing()),
        semantic_field("executed", jdv_value_bool(0)), semantic_field("features", semantic_record(sem, features, 7))
    };
    return semantic_finish(call, semantic_record(sem, fields, 10), 0);
}
