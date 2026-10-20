// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/type — identify how a name resolves.

#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <core/alias.h>
#include <core/env.h>
#include <parser/exec.h>
#include <theme/colors.h>
#include <function_defs.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int resolution_record(jash_discovery_rows_t* rows, const char* name,
                              const char* kind, const char* value, const char* path,
                              const char* summary, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("name"), jash_discovery_string(rows, name)},
        {JDV_LITERAL("kind"), jash_discovery_string(rows, kind)},
        {JDV_LITERAL("found"), jdv_value_bool(error == NULL)},
        {JDV_LITERAL("value"), jash_discovery_string(rows, value)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("summary"), jash_discovery_string(rows, summary)},
        {JDV_LITERAL("executable"), path ? jdv_value_bool(access(path, X_OK) == 0) : jdv_value_nothing()},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof(fields) / sizeof(fields[0]));
}

int jash_discovery_resolve(jash_state_t* s, jash_discovery_rows_t* rows,
                           const char* name, int all, int path_only) {
    int found = 0;
    if (!path_only) {
        const char* alias = jash_alias_get(s, name);
        if (alias) {
            if (resolution_record(rows, name, "alias", alias, NULL, NULL, NULL)) return -1;
            found = 1;
            if (!all) return 1;
        }
        /* Match the evaluator: a shell function shadows a builtin. */
        if (s->functions && jash_function_lookup(s->functions, name)) {
            if (resolution_record(rows, name, "function", NULL, NULL, NULL, NULL)) return -1;
            found = 1;
            if (!all) return 1;
        }
        const jash_builtin_t* builtin = jash_builtin_lookup(name);
        if (builtin) {
            if (resolution_record(rows, name, "builtin", NULL, NULL, builtin->help, NULL)) return -1;
            found = 1;
            if (!all) return 1;
        }
    }
    char resolved[PATH_MAX];
    /* jash_path_lookup's explicit-path copy is bounded; never report a prefix
     * of a caller's long name as a different successful resolution. */
    if (strlen(name) >= sizeof(resolved)) {
        if (resolution_record(rows, name, "missing", NULL, NULL, NULL, "name_too_long")) return -1;
        return found;
    }
    if (jash_path_lookup(s, name, resolved, sizeof(resolved))) {
        struct stat st;
        if (!stat(resolved, &st) && S_ISREG(st.st_mode)) {
            if (resolution_record(rows, name, "file", NULL, resolved, NULL, NULL)) return -1;
            found = 1;
        }
    }
    if (!found && resolution_record(rows, name, "missing", NULL, NULL, NULL, "not_found")) return -1;
    return found;
}

int jash_b_type_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return jash_discovery_error(w, ctx, "type", mode, "conflicting_formats", 2);
    int ended = 0, names = 0, all = 0, type_only = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && argv[i][0] == '-') {
            if (!argv[i][1]) return jash_discovery_error(w, ctx, "type", mode, "invalid_options", 2);
            for (const char* p = argv[i] + 1; *p; p++) {
                if (*p == 'a') all = 1;
                else if (*p == 't') type_only = 1;
                else return jash_discovery_error(w, ctx, "type", mode, "invalid_options", 2);
            }
        } else names++;
    }
    if (!names) return jash_discovery_error(w, ctx, "type", mode, "name_required", 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "type", mode, "allocation_failed", 125);
    int status = 0;
    ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && (jash_discovery_format_flag(argv[i]) || argv[i][0] == '-')) continue;
        int found = jash_discovery_resolve(s, &rows, argv[i], all, 0);
        if (found < 0) break;
        if (!found) status = 1;
    }
    int rc = status;
    if (mode == JASH_DISCOVERY_TEXT && !rows.error) {
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* row = &rows.items[i];
            const jdv_value_t* name = jash_discovery_field(row, "name");
            const jdv_value_t* kind = jash_discovery_field(row, "kind");
            const jdv_value_t* error = jash_discovery_field(row, "error");
            const jdv_value_t* value = jash_discovery_field(row, "value");
            const jdv_value_t* path = jash_discovery_field(row, "path");
            long written;
            if (error->kind != JDV_NOTHING) {
                jash_writef(jash_stderr_writer, NULL, "%.*s: not found\n", (int)name->as.slice.len, name->as.slice.data);
                continue;
            }
            if (type_only) written = jash_writef(w, ctx, "%.*s\n", (int)kind->as.slice.len, kind->as.slice.data);
            else if (value->kind != JDV_NOTHING)
                written = jash_writef(w, ctx, "%.*s is aliased to `%.*s'\n", (int)name->as.slice.len, name->as.slice.data, (int)value->as.slice.len, value->as.slice.data);
            else if (path->kind != JDV_NOTHING)
                written = jash_writef(w, ctx, "%.*s is %.*s\n", (int)name->as.slice.len, name->as.slice.data, (int)path->as.slice.len, path->as.slice.data);
            else written = jash_writef(w, ctx, "%.*s is a shell %.*s\n", (int)name->as.slice.len, name->as.slice.data, (int)kind->as.slice.len, kind->as.slice.data);
            if (written < 0) { rc = 125; break; }
        }
    } else rc = jash_discovery_emit(w, ctx, "type", mode, &rows, status);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_type(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_type_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
