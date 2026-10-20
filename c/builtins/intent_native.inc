// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_store.h"
#include "memory_receipt.h"

static int intent_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = "get"; const char* text = NULL;
    if (argc) {
        if (!strcmp(argv[0], "get") || !strcmp(argv[0], "show")) {
            if (argc != 1) return semantic_error(call, "invalid_arguments", 2);
        } else if (!strcmp(argv[0], "clear")) {
            if (argc != 1) return semantic_error(call, "invalid_arguments", 2);
            operation = "clear";
        } else if (!strcmp(argv[0], "set")) {
            if (argc != 2) return semantic_error(call, "invalid_arguments", 2);
            operation = "set"; text = argv[1];
        } else if (argc == 1 && argv[0][0] != '-') { operation = "set"; text = argv[0]; }
        else return semantic_error(call, "invalid_arguments", 2);
    }
    if (text && (strlen(text) > 511 || strchr(text, '\n') || strchr(text, '\r')))
        return semantic_error(call, "invalid_arguments", 2);
    const char* prior_environment = jash_env_get(state, "JASH_INTENT");
    size_t prior_bytes = prior_environment ? strlen(prior_environment) : 0;
    if (prior_bytes > 65536u) return semantic_error(call, "environment_value_limit", 75);
    int mutating = strcmp(operation, "get") != 0;
    if (mutating) {
        /* Failed environment updates may retain the previous exact value. */
        int admitted = memory_receipt_admit(call, 8192u + 6u * prior_bytes);
        if (admitted) return admitted;
    }
    memory_store_t store = {-1, -1}; memory_write_t write_result = {0};
    char* bytes = NULL; size_t length = 0; int exists = 0, saved = 0, status = 0, environment_applied = 0;
    const char* error = NULL;
    if (memory_store_open(&store, NULL, mutating) ||
        memory_store_read(&store, "intent", 512, &bytes, &length, &exists)) {
        saved = errno; error = memory_store_error(saved); status = 1;
    } else if (exists) {
        if (length && bytes[length - 1] == '\n') bytes[--length] = 0;
        if (length > 511 || memchr(bytes, 0, length) || memchr(bytes, '\n', length) || memchr(bytes, '\r', length)) {
            saved = EINVAL; error = "invalid_store"; status = 1;
        }
    }
    const char* persisted = exists && !status && length ? bytes : NULL;
    if (!status && mutating) {
        if (text && *text) {
            char file[513]; size_t size = strlen(text);
            memcpy(file, text, size); file[size++] = '\n';
            write_result = memory_store_publish(&store, "intent", file, size);
        } else write_result = memory_store_remove(&store, "intent");
        if (write_result.applied || !write_result.error) {
            persisted = text && *text ? text : NULL;
            if (jash_env_set(state, "JASH_INTENT", text ? text : "")) {
                saved = errno ? errno : ENOMEM; error = "environment_update_failed"; status = 1;
            } else environment_applied = 1;
        }
        if (write_result.error) { saved = write_result.error; error = memory_store_error(saved); status = 1; }
    }
    if (memory_store_close(&store) && !status) { saved = errno; error = "store_close_failed"; status = 1; }
    const char* environment = jash_env_get(state, "JASH_INTENT");
    const char* value = environment && *environment ? environment : persisted;
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.intent.v1")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("value", semantic_text(sem, value)),
        semantic_field("persisted_value", semantic_text(sem, persisted)),
        semantic_field("source", semantic_symbol(sem, environment && *environment ? "environment" : persisted ? "store" : "absent")),
        semantic_field("applied", jdv_value_bool(write_result.applied)),
        semantic_field("durable", jdv_value_bool(write_result.durable)),
        semantic_field("environment_applied", jdv_value_bool(environment_applied)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", saved ? jdv_value_i64(saved) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    free(bytes); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(intent, "intent", "jash.intent.v1", intent_native_provider)
