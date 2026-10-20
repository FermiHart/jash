// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Explicit persistence reports reflect the real publication/restore result.
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <core/persist.h>
#include <limits.h>
#include <string.h>

int jash_b_jash_state_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "jash_state", mode, "conflicting_formats", 2);
    const char* operation = NULL; int ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (operation || (!ended && argv[i][0] == '-')) return jash_discovery_error(w, ctx, "jash_state", mode, "invalid_options", 2);
        operation = argv[i];
    }
    if (!operation || (strcmp(operation, "save") && strcmp(operation, "restore") && strcmp(operation, "clear")))
        return jash_discovery_error(w, ctx, "jash_state", mode, "invalid_options", 2);
    char path[PATH_MAX];
    if (jash_persist_path(path, sizeof path)) return jash_discovery_error(w, ctx, "jash_state", mode, "path_too_long", 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "jash_state", mode, "allocation_failed", 125);
    int admitted = jash_discovery_effects_begin(&rows, w, ctx, "jash_state", mode, "jash.persist.v1");
    if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    jash_persist_result_t outcome;
    int restore = !strcmp(operation, "restore");
    int result = restore ? jash_persist_load_checked(s, &outcome) :
                 !strcmp(operation, "save") ? jash_persist_save_checked(s, &outcome) : jash_persist_clear_checked(s, &outcome);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("operation"), jash_discovery_string(&rows, operation)},
        {JDV_LITERAL("path"), jash_discovery_path(&rows, path)},
        {JDV_LITERAL("applied"), jdv_value_bool(outcome.applied)},
        {JDV_LITERAL("durable"), restore ? jdv_value_nothing() : jdv_value_bool(outcome.durable)},
        {JDV_LITERAL("cwd_applied"), jdv_value_bool(outcome.cwd_applied)},
        {JDV_LITERAL("cwd_policy"), jash_discovery_string(&rows, restore ? "interactive-only" : NULL)},
        {JDV_LITERAL("env_entries"), jdv_value_u64(outcome.env_entries)},
        {JDV_LITERAL("alias_entries"), jdv_value_u64(outcome.alias_entries)},
        {JDV_LITERAL("ok"), jdv_value_bool(result == 0)},
        {JDV_LITERAL("error"), jash_discovery_string(&rows, outcome.error)},
        {JDV_LITERAL("errno"), result ? jdv_value_u64((unsigned)outcome.error_number) : jdv_value_nothing()},
    };
    jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
    int status = result ? 1 : 0, rc;
    if (mode == JASH_DISCOVERY_TEXT && !rows.error) {
        if (status) rc = jash_writef(jash_stderr_writer, NULL, "jash_state: %s: %s\n", operation, outcome.error) < 0 ? 125 : status;
        else rc = jash_writef(w, ctx, "state %s\n", restore ? "restored" : !strcmp(operation, "save") ? "saved" : "cleared") < 0 ? 125 : 0;
    } else rc = jash_discovery_emit_schema(w, ctx, "jash_state", mode, &rows, status, "jash.persist.v1");
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_jash_state(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_jash_state_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
