// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Builtin enable: the registry remains the source of known builtin names.
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <string.h>

#define ENABLE_MAX_DISABLED 64
static char disabled_names[ENABLE_MAX_DISABLED][64];
static int disabled_count;

int jash_builtin_is_disabled(const char* name) {
    for (int i = 0; i < disabled_count; i++)
        if (!strcmp(disabled_names[i], name)) return 1;
    return 0;
}

static int known_name(const char* name) {
    for (int i = 0; i < jash_builtin_count(); i++)
        if (!strcmp(JASH_BUILTINS[i].name, name)) return 1;
    return 0;
}

static int set_enabled(const char* name, int enabled) {
    for (int i = 0; i < disabled_count; i++) {
        if (strcmp(disabled_names[i], name)) continue;
        if (enabled) {
            disabled_count--;
            if (i != disabled_count) memcpy(disabled_names[i], disabled_names[disabled_count], sizeof disabled_names[i]);
        }
        return 0;
    }
    if (enabled) return 0;
    if (disabled_count == ENABLE_MAX_DISABLED) return -1;
    memcpy(disabled_names[disabled_count++], name, strlen(name) + 1u);
    return 0;
}

static int enable_row(jash_discovery_rows_t* rows, const char* name, const char* operation,
                       int known, int before, int after, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("name"), jash_discovery_string(rows, name)},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("known"), jdv_value_bool(known)},
        {JDV_LITERAL("enabled_before"), known ? jdv_value_bool(before) : jdv_value_nothing()},
        {JDV_LITERAL("enabled"), known ? jdv_value_bool(after) : jdv_value_nothing()},
        {JDV_LITERAL("changed"), jdv_value_bool(known && before != after)},
        {JDV_LITERAL("ok"), jdv_value_bool(!error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

int jash_b_enable_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(w, ctx, "enable", mode, "conflicting_formats", 2);
    int ended = 0, names = 0, disable = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && !strcmp(argv[i], "-n")) { disable = 1; continue; }
        if ((!ended && argv[i][0] == '-') || !argv[i][0] || strlen(argv[i]) >= sizeof disabled_names[0])
            return jash_discovery_error(w, ctx, "enable", mode, "invalid_options", 2);
        names++;
    }
    if (names > 1024) return jash_discovery_error(w, ctx, "enable", mode, "operand_limit_exceeded", 75);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "enable", mode, "allocation_failed", 125);
    if (names) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "enable", mode, "jash.enable.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0;
    if (!names) {
        for (int i = 0; i < jash_builtin_count(); i++) {
            const char* name = JASH_BUILTINS[i].name;
            int enabled = !jash_builtin_is_disabled(name);
            if ((disable || mode == JASH_DISCOVERY_TEXT) && enabled) continue;
            if (enable_row(&rows, name, "list", 1, enabled, enabled, NULL)) break;
        }
    } else {
        ended = 0;
        for (int i = 0; i < argc; i++) {
            if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
            if (!ended && (jash_discovery_format_flag(argv[i]) || !strcmp(argv[i], "-n"))) continue;
            int known = known_name(argv[i]), before = !jash_builtin_is_disabled(argv[i]);
            const char* error = !known ? "not_a_builtin" : NULL;
            if (known && set_enabled(argv[i], !disable)) error = "disabled_table_full";
            if (error) status = 1;
            if (enable_row(&rows, argv[i], disable ? "disable" : "enable", known, before,
                            !jash_builtin_is_disabled(argv[i]), error)) break;
            if (jash_discovery_effects_flush(&rows)) break;
        }
    }
    int rc;
    if (mode != JASH_DISCOVERY_TEXT || rows.error) {
        rc = jash_discovery_emit_schema(w, ctx, "enable", mode, &rows, status, "jash.enable.v1");
    } else {
        rc = status;
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* name = jash_discovery_field(&rows.items[i], "name");
            const jdv_value_t* error = jash_discovery_field(&rows.items[i], "error");
            if (error->kind != JDV_NOTHING) jash_writef(jash_stderr_writer, NULL, "enable: %.*s: %.*s\n",
                    (int)name->as.slice.len, name->as.slice.data, (int)error->as.slice.len, error->as.slice.data);
            else if (!names && jash_writef(w, ctx, "%.*s\n", (int)name->as.slice.len, name->as.slice.data) < 0) rc = 125;
        }
    }
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_enable(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_enable_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
