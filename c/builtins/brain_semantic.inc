// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Actual liquid-model state and outputs. Model predictions remain estimates;
 * intent is a keyword classifier. Prediction advances the recurrent state. */
static int brain_integer(const char* text, int* value) {
    if (!text || !*text) return -1;
    char* end = NULL; errno = 0; long number = strtol(text, &end, 10);
    if (errno || *end || number < INT_MIN || number > INT_MAX) return -1;
    *value = (int)number; return 0;
}

static jdv_value_t brain_stats_value(semantic_value_t* sem, jash_liquid_stats_t stats) {
    jdv_record_input_t fields[] = {
        semantic_field("version", jdv_value_u64(stats.version)),
        semantic_field("input_dimensions", jdv_value_u64(JL_INPUT_DIM)),
        semantic_field("hidden_size", jdv_value_u64(stats.hidden_size)),
        semantic_field("command_classes", jdv_value_u64(stats.cmd_classes)),
        semantic_field("intent_classes", jdv_value_u64(stats.intent_classes)),
        semantic_field("examples_seen", jdv_value_u64(stats.examples_seen)),
        semantic_field("last_saved_ns", jdv_value_u64(stats.last_saved_ns)),
        semantic_field("clock", semantic_symbol(sem, "monotonic")),
        semantic_field("average_loss", semantic_binary64(sem, stats.avg_loss)),
        semantic_field("accuracy_estimated", semantic_binary64(sem, stats.accuracy))
    };
    return semantic_record(sem, fields, 10);
}

static int brain_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "status";
    int intent = !strcmp(operation, "intent"), suggest = !strcmp(operation, "suggest");
    int impact = !strcmp(operation, "impact"), learn = !strcmp(operation, "learn");
    int reward = !strcmp(operation, "reward"), guard = !strcmp(operation, "guard");
    int reset = !strcmp(operation, "reset"), status = !strcmp(operation, "status");
    if (!intent && !suggest && !impact && !learn && !reward && !guard && !reset && !status)
        return semantic_error(call, "invalid_operation", 2);
    if (((intent || impact || reward) && argc != 2) || ((reset || status) && argc > 1) ||
        (guard && (argc > 2 || (argc == 2 && strcmp(argv[1], "status") && strcmp(argv[1], "on") && strcmp(argv[1], "off")))) ||
        (learn && (argc < 2 || argc > 6))) return semantic_error(call, "invalid_arguments", 2);
    for (int i = 1; i < argc; i++) if (strlen(argv[i]) >= 512) return semantic_error(call, "argument_limit", 2);
    const char* input = argc > 1 ? argv[1] : "";
    const char* history = NULL; const char* next = NULL;
    int execution_rc = 0, intent_class = -1, reward_number = 0, have_reward = 0;
    if (suggest) {
        if (argc > 1 && !strcmp(argv[1], "--after")) {
            if (argc < 3 || argc > 4) return semantic_error(call, "invalid_arguments", 2);
            history = argv[2]; input = argc == 4 ? argv[3] : "";
        } else if (argc > 2) return semantic_error(call, "invalid_arguments", 2);
    }
    if (reward && brain_integer(input, &reward_number)) return semantic_error(call, "invalid_reward", 2);
    if (learn) {
        unsigned seen = 0;
        for (int i = 2; i < argc; i++) {
            const char* value = strchr(argv[i], '='); unsigned bit = 0;
            if (!value) return semantic_error(call, "invalid_learning_option", 2);
            value++;
            if (!strncmp(argv[i], "rc=", 3)) {
                bit = 1; if (brain_integer(value, &execution_rc)) return semantic_error(call, "invalid_rc", 2);
            } else if (!strncmp(argv[i], "intent=", 7)) {
                bit = 2;
                if (brain_integer(value, &intent_class)) intent_class = jash_liquid_intent_class(value);
                if (intent_class < 0 || intent_class >= JL_INTENT_CLASSES) return semantic_error(call, "invalid_intent_class", 2);
            } else if (!strncmp(argv[i], "reward=", 7)) {
                bit = 4; have_reward = 1;
                if (brain_integer(value, &reward_number)) return semantic_error(call, "invalid_reward", 2);
            } else if (!strncmp(argv[i], "next=", 5)) { bit = 8; next = value; }
            else return semantic_error(call, "invalid_learning_option", 2);
            if (seen & bit) return semantic_error(call, "duplicate_learning_option", 2);
            seen |= bit;
        }
        if (!have_reward) reward_number = execution_rc ? -1 : 1;
    }
    if (reward_number > 1) reward_number = 1;
    if (reward_number < -1) reward_number = -1;
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[20]; size_t count = 0;
    fields[count++] = semantic_field("schema", semantic_symbol(sem, call->schema_id));
    fields[count++] = semantic_field("operation", semantic_symbol(sem, operation));
    fields[count++] = semantic_field("engine", semantic_symbol(sem, intent ? "keyword-classifier" : "local-liquid-model"));
    int result = 0, persisted = 0, effect = learn || reward || reset || suggest || impact || (guard && argc == 2 && strcmp(argv[1], "status"));
    /* Model initialization may create the store. It is admitted even for a
     * status query, independently of the command's explicit state mutation. */
    if (!intent && !(guard && !effect)) {
        if (semantic_admit(call)) return 125;
        if (jash_liquid_init(NULL)) return semantic_error(call, "model_initialization_failed", 1);
    }
    if (intent) {
        int id = jash_liquid_intent_class(input);
        fields[count++] = semantic_field("input", semantic_text(sem, input));
        fields[count++] = semantic_field("intent_class", jdv_value_i64(id));
        fields[count++] = semantic_field("intent", semantic_symbol(sem, JASH_LIQUID_INTENT_NAMES[id]));
    } else if (guard) {
        if (effect) { result = guard_write(!strcmp(argv[1], "on")) ? 1 : 0; persisted = !result; }
        fields[count++] = semantic_field("enabled", jdv_value_bool(guard_read()));
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) return semantic_error(call, "cwd_unavailable", 1);
        if (suggest || impact) {
            if (history) {
                char copy[512]; memcpy(copy, history, strlen(history) + 1); char* cursor = copy; char* item;
                while ((item = strsep(&cursor, ";"))) if (*item) jash_liquid_prime(item, 0, 0, 0, cwd);
            }
            float features[JL_INPUT_DIM]; jash_liquid_prediction_t prediction;
            jash_liquid_features(input, 0, 0, impact ? 1 : 0, cwd, features);
            jash_liquid_predict(features, &prediction);
            if (prediction.top_intent_class < 0 || prediction.top_intent_class >= JL_INTENT_CLASSES)
                return semantic_error(call, "invalid_model_prediction", 1);
            jdv_value_t top[3];
            for (size_t i = 0; i < 3; i++) {
                jdv_record_input_t item[] = {
                    semantic_field("class", jdv_value_i64(prediction.top_k_classes[i])),
                    semantic_field("label", semantic_text(sem, jash_liquid_class_label(prediction.top_k_classes[i]))),
                    semantic_field("score_estimated", semantic_binary64(sem, prediction.top_k_scores[i]))
                };
                top[i] = semantic_record(sem, item, 3);
            }
            fields[count++] = semantic_field("input", impact ? semantic_path(sem, input) : semantic_text(sem, input));
            fields[count++] = semantic_field("intent", semantic_symbol(sem, JASH_LIQUID_INTENT_NAMES[prediction.top_intent_class]));
            fields[count++] = semantic_field("intent_confidence_estimated", semantic_binary64(sem, prediction.intent_confidence));
            fields[count++] = semantic_field("risk_estimated", semantic_binary64(sem, prediction.risk));
            fields[count++] = semantic_field("confidence_estimated", semantic_binary64(sem, prediction.confidence));
            fields[count++] = semantic_field("next_commands", semantic_list(sem, top, 3));
            fields[count++] = semantic_field("basis", semantic_symbol(sem, impact ? "synthetic-one-mutation-feature-vector" : "recurrent-state-and-command-features"));
            fields[count++] = semantic_field("verified_blast_radius", jdv_value_bool(0));
        } else if (learn) {
            jash_liquid_example_t example = {0};
            jash_liquid_features(input, execution_rc, 0, 0, cwd, example.features);
            example.cmd_class = jash_liquid_cmd_class(input);
            example.next_cmd_class = next ? jash_liquid_cmd_class(next) : -1;
            example.intent_class = intent_class < 0 ? jash_liquid_intent_class(input) : intent_class;
            example.reward = (float)reward_number;
            jash_liquid_learn(&example); jash_liquid_update_label(example.cmd_class, input);
            if (next) jash_liquid_update_label(example.next_cmd_class, next);
            result = jash_liquid_save() ? 1 : 0; persisted = !result;
            fields[count++] = semantic_field("input", semantic_text(sem, input));
            fields[count++] = semantic_field("observed_status", jdv_value_i64(execution_rc));
            fields[count++] = semantic_field("reward", jdv_value_i64(reward_number));
        } else if (reward || reset) {
            if (reward) jash_liquid_reward((float)reward_number); else jash_liquid_reset();
            result = jash_liquid_save() ? 1 : 0; persisted = !result;
        }
        fields[count++] = semantic_field("model", brain_stats_value(sem, jash_liquid_stats()));
    }
    fields[count++] = semantic_field("ok", jdv_value_bool(!result));
    fields[count++] = semantic_field("state_advanced", jdv_value_bool(effect));
    fields[count++] = semantic_field("persisted", effect && !suggest && !impact ? jdv_value_bool(persisted) : jdv_value_nothing());
    fields[count++] = semantic_field("error", semantic_symbol(sem, result ? "model_store_write_failed" : NULL));
    return semantic_finish(call, semantic_record(sem, fields, count), result);
}
