// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_store.h"
#include <core/env.h>

typedef struct { char name[256], value[4096]; int ready, status; } preference_application_t;

static jdv_value_t preference_apply_value(semantic_builtin_t* call,
        const semantic_memory_row_t* rows, const preference_application_t* applications,
        jdv_value_t* values, size_t count, size_t applied, int failed) {
    semantic_value_t* sem = &call->values;
    for (size_t i = 0; i < count; i++) {
        jdv_record_input_t item[] = {
            semantic_field("preference", rows[i].content),
            semantic_field("name", semantic_text(sem, applications[i].ready ? applications[i].name : NULL)),
            semantic_field("value", semantic_text(sem, applications[i].ready ? applications[i].value : NULL)),
            semantic_field("status", semantic_symbol(sem, !applications[i].ready ? "not-applicable" :
                applications[i].status == 2 ? "not-attempted" : applications[i].status == 1 ? "failed" : "applied"))
        };
        values[i] = semantic_record(sem, item, 4);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, "apply")),
        semantic_field("scope", semantic_symbol(sem, "shell-process-environment")),
        semantic_field("ok", jdv_value_bool(!failed)), semantic_field("applied", jdv_value_u64(applied)),
        semantic_field("partial", jdv_value_bool(failed && applied)),
        semantic_field("effects", semantic_list(sem, values, count))
    };
    return semantic_record(sem, fields, 7);
}

static int preference_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = argc ? argv[0] : "list";
    int learn = !strcmp(operation, "learn"), apply = !strcmp(operation, "apply");
    int recall = !strcmp(operation, "recall"), forget = !strcmp(operation, "forget"), list = !strcmp(operation, "list");
    if ((!learn && !apply && !recall && !forget && !list) ||
        (learn && argc != 2 && (argc != 4 || strcmp(argv[2], "--category"))) ||
        (recall && argc != 2) || ((apply || forget || list) && argc > 1))
        return semantic_error(call, "invalid_arguments", 2);
    char path[4096];
    if (semantic_home_path(path, sizeof(path), "preferences.yaml")) return semantic_error(call, "store_path_limit", 1);
    if (learn) return semantic_memory_append(call, operation, path, "", argv[1], argc == 4 ? argv[3] : "general");
    if (forget) {
        jdv_value_t detail = semantic_symbol(&call->values, "whole-preference-store");
        int preflight = semantic_store_preflight(call, operation, path, detail);
        if (preflight) return semantic_error(call, "receipt_limit_before_store", preflight);
        if (semantic_admit(call)) return 125;
        int failed = unlink(path);
        return semantic_store_receipt(call, operation, path, detail, failed ? 1 : 0);
    }
    semantic_value_t* sem = &call->values;
    if (!apply) {
        jdv_value_t records; int exists;
        if (semantic_memory_collect(sem, path, 1, recall ? argv[1] : NULL, &records, &exists))
            return semantic_error(call, "invalid_or_oversized_memory_store", 1);
        jdv_record_input_t fields[] = {
            semantic_field("schema", semantic_symbol(sem, call->schema_id)),
            semantic_field("operation", semantic_symbol(sem, operation)),
            semantic_field("source", semantic_path(sem, path)),
            semantic_field("exists", jdv_value_bool(exists)),
            semantic_field("preferences", records)
        };
        return semantic_finish(call, semantic_record(sem, fields, 5), 0);
    }
    char* data = NULL; size_t bytes = 0, count = 0; int exists;
    if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, &exists)) return semantic_error(call, "store_read_failed", 1);
    semantic_memory_row_t* rows = calloc(SEMANTIC_STORE_ROWS, sizeof(*rows));
    preference_application_t* applications = calloc(SEMANTIC_STORE_ROWS, sizeof(*applications));
    jdv_value_t* values = calloc(SEMANTIC_STORE_ROWS, sizeof(*values));
    if (!rows || !applications || !values) {
        free(data); free(rows); free(applications); free(values); return semantic_error(call, "allocation_failed", 125);
    }
    int failed = semantic_memory_parse(sem, data, bytes, 1, rows, &count);
    free(data);
    for (size_t i = 0; i < count && !failed; i++) {
        jdv_slice_t text = rows[i].content.as.slice;
        if (text.len > 4095u || memchr(text.data, 0, text.len)) { failed = 1; break; }
        char content[4096]; memcpy(content, text.data, text.len); content[text.len] = 0;
        char* colon = strchr(content, ':');
        if (!colon) continue;
        *colon = 0; char* value = colon + 1; while (*value == ' ') value++;
        const char* name = NULL;
        if (!strcmp(content, "output") || !strcmp(content, "output-format")) name = "JASH_OUTPUT_FORMAT";
        else if (!strcmp(content, "color")) {
            name = "JASH_COLOR";
            if (!strcmp(value, "always") || !strcmp(value, "on")) value = "1";
            else if (!strcmp(value, "never") || !strcmp(value, "off")) value = "0";
            else { failed = 1; break; }
        } else if (!strcmp(content, "verbose")) name = "JASH_VERBOSE";
        else if (!strcmp(content, "safe") || !strcmp(content, "trash")) {
            name = !strcmp(content, "safe") ? "JASH_SAFE" : "JASH_TRASH";
            value = (!strcmp(value, "on") || !strcmp(value, "true")) ? "1" : "0";
        } else {
            if (!semantic_filename(content, 240)) { failed = 1; break; }
            snprintf(applications[i].name, sizeof(applications[i].name), "JASH_PREF_%s", content);
            for (char* p = applications[i].name; *p; p++) if (*p == '-') *p = '_';
        }
        if (name) snprintf(applications[i].name, sizeof(applications[i].name), "%s", name);
        if (strlen(applications[i].name) >= JASH_ENV_KEY_LEN || jash_readonly_is_set(applications[i].name)) { failed = 1; break; }
        memcpy(applications[i].value, value, strlen(value) + 1); applications[i].ready = 1;
        applications[i].status = 2;
    }
    if (failed) { free(rows); free(applications); free(values); return semantic_error(call, "invalid_preference_store_or_application", 2); }
    size_t arena_mark = sem->arena.used;
    jdv_value_t candidate = preference_apply_value(call, rows, applications, values, count, count, 1);
    int preflight = semantic_preflight(call, &candidate);
    sem->arena.used = arena_mark;
    if (preflight) { free(rows); free(applications); free(values); return semantic_error(call, "receipt_limit_before_application", preflight); }
    if (semantic_admit(call)) { free(rows); free(applications); free(values); return 125; }
    size_t applied = 0;
    for (size_t i = 0; i < count; i++) if (applications[i].ready) {
        if (failed) applications[i].status = 2;
        else {
            const char* key = applications[i].name; const char* value = applications[i].value;
            if (jash_env_apply_batch(state, &key, &value, 1)) { applications[i].status = 1; failed = 1; }
            else { applications[i].status = 0; applied++; }
        }
    }
    jdv_value_t result = preference_apply_value(call, rows, applications, values, count, applied, failed);
    free(rows); free(applications); free(values);
    return semantic_finish(call, result, failed ? 1 : 0);
}
