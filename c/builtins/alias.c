// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native alias queries/mutations; no accepted value is silently truncated.
#include "builtin.h"
#include "alias_report.h"
#include <core/alias.h>
#include <core/argv.h>
#include <string.h>

int jash_alias_result_row(jash_discovery_rows_t* rows, const char* name, const char* operation,
                          const char* before, const char* value, int changed, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("name"), jash_discovery_string(rows, name)},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("before"), jash_discovery_string(rows, before)},
        {JDV_LITERAL("value"), jash_discovery_string(rows, value)},
        {JDV_LITERAL("present"), jdv_value_bool(value != NULL)},
        {JDV_LITERAL("changed"), jdv_value_bool(changed)},
        {JDV_LITERAL("ok"), jdv_value_bool(error == NULL)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof(fields) / sizeof(fields[0]));
}

int jash_alias_report_emit(jash_writer_t w, void* ctx, const char* command, int mode,
                           jash_discovery_rows_t* rows, int status) {
    if (mode != JASH_DISCOVERY_TEXT || rows->error)
        return jash_discovery_emit_schema(w, ctx, command, mode, rows, status, "jash.alias.v1");
    for (size_t i = 0; i < rows->count; i++) {
        const jdv_value_t* name = jash_discovery_field(&rows->items[i], "name");
        const jdv_value_t* value = jash_discovery_field(&rows->items[i], "value");
        const jdv_value_t* operation = jash_discovery_field(&rows->items[i], "operation");
        const jdv_value_t* error = jash_discovery_field(&rows->items[i], "error");
        if (error->kind != JDV_NOTHING) {
            jash_writef(jash_stderr_writer, NULL, "%s: %.*s: %.*s\n", command,
                        (int)name->as.slice.len, name->as.slice.data,
                        (int)error->as.slice.len, error->as.slice.data);
        } else if (value->kind != JDV_NOTHING &&
                   ((operation->as.slice.len == 3 && !memcmp(operation->as.slice.data, "get", 3)) ||
                    (operation->as.slice.len == 4 && !memcmp(operation->as.slice.data, "list", 4)))) {
            if (jash_writef(w, ctx, "alias %.*s='%.*s'\n", (int)name->as.slice.len, name->as.slice.data,
                            (int)value->as.slice.len, value->as.slice.data) < 0) return 125;
        }
    }
    return status;
}

int jash_b_alias_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return jash_discovery_error(w, ctx, "alias", mode, "conflicting_formats", 2);
    int ended = 0, operands = 0, print = 0, mutates = 0;
    /* Complete argument validation precedes the first state change. */
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && !strcmp(argv[i], "-p")) { print = 1; continue; }
        if (!ended && argv[i][0] == '-') return jash_discovery_error(w, ctx, "alias", mode, "invalid_options", 2);
        const char* equal = strchr(argv[i], '=');
        if (equal) mutates = 1;
        size_t key_length = equal ? (size_t)(equal - argv[i]) : strlen(argv[i]);
        if (!key_length || key_length >= JASH_ALIAS_KEY_LEN ||
            (equal && strlen(equal + 1) >= JASH_ALIAS_VAL_LEN))
            return jash_discovery_error(w, ctx, "alias", mode, "alias_limit_exceeded", 2);
        operands++;
    }
    if (print && operands) return jash_discovery_error(w, ctx, "alias", mode, "invalid_options", 2);
    if (operands > 1024) return jash_discovery_error(w, ctx, "alias", mode, "operand_limit_exceeded", 75);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "alias", mode, "allocation_failed", 125);
    if (mutates) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "alias", mode, "jash.alias.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0;
    if (!operands) {
        for (uint32_t i = 0; i < jash_alias_count(s); i++) {
            const char *key, *value;
            if (jash_alias_at(s, i, &key, &value) && jash_alias_result_row(&rows, key, "list", value, value, 0, NULL)) break;
        }
    } else {
        ended = 0;
        for (int i = 0; i < argc; i++) {
            if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
            if (!ended && (jash_discovery_format_flag(argv[i]) || !strcmp(argv[i], "-p"))) continue;
            const char* equal = strchr(argv[i], '=');
            size_t length = equal ? (size_t)(equal - argv[i]) : strlen(argv[i]);
            char key[JASH_ALIAS_KEY_LEN], before[JASH_ALIAS_VAL_LEN];
            memcpy(key, argv[i], length); key[length] = 0;
            const char* previous = jash_alias_get(s, key);
            if (previous) memcpy(before, previous, strlen(previous) + 1);
            const char* error = NULL;
            int changed = 0;
            if (equal) {
                if (jash_alias_set(s, key, equal + 1)) { error = "alias_table_full"; status = 1; }
                else changed = !previous || strcmp(before, equal + 1) != 0;
            } else if (!previous) { error = "not_found"; status = 1; }
            if (jash_alias_result_row(&rows, key, equal ? "set" : "get", previous ? before : NULL,
                                      jash_alias_get(s, key), changed, error)) break;
            if (jash_discovery_effects_flush(&rows)) break;
        }
    }
    int rc = jash_alias_report_emit(w, ctx, "alias", mode, &rows, status);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_alias(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_alias_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
