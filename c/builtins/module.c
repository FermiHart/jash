// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native module registry observations and load/unload receipts.
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <module/loader.h>
#include <string.h>
#include <stdio.h>

#define REQUESTED_CAPS UINT32_MAX

static int module_row(jash_discovery_rows_t* rows, const char* operation, const char* requested,
                       const jmod_entry_t* entry, const char* resolved, int before, int after,
                       int changed, int unloaded, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("scope"), jash_discovery_string(rows, "module-registry")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("requested"), jash_discovery_string(rows, requested)},
        {JDV_LITERAL("name"), jash_discovery_string(rows, entry ? entry->name : NULL)},
        {JDV_LITERAL("version"), jash_discovery_string(rows, entry ? entry->version : NULL)},
        {JDV_LITERAL("description"), jash_discovery_string(rows, entry ? entry->description : NULL)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, entry ? entry->path : resolved)},
        {JDV_LITERAL("caps_declared"), entry ? jdv_value_u64(entry->caps_declared) : jdv_value_nothing()},
        {JDV_LITERAL("caps_used"), entry ? jdv_value_u64(entry->caps_used) : jdv_value_nothing()},
        {JDV_LITERAL("loaded_before"), before < 0 ? jdv_value_nothing() : jdv_value_bool(before)},
        {JDV_LITERAL("loaded"), after < 0 ? jdv_value_nothing() : jdv_value_bool(after)},
        {JDV_LITERAL("changed"), jdv_value_bool(changed)},
        {JDV_LITERAL("unloaded"), jdv_value_bool(unloaded)},
        {JDV_LITERAL("ok"), jdv_value_bool(!error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

static int module_path(const char* target, char* path, size_t capacity) {
    if (strchr(target, '/')) {
        size_t length = strlen(target);
        if (length >= capacity) return 0;
        memcpy(path, target, length + 1u); return 1;
    }
    return jmod_resolve(target, path, capacity);
}

int jash_b_module_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "module", mode, "conflicting_formats", 2);
    const char* operands[2]; int count = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if ((!ended && argv[i][0] == '-') || count == 2)
            return jash_discovery_error(w, ctx, "module", mode, "invalid_options", 2);
        operands[count++] = argv[i];
    }
    const char* operation = count ? operands[0] : (mode == JASH_DISCOVERY_TEXT ? "help" : "list");
    const char* target = count > 1 ? operands[1] : NULL;
    int list = !strcmp(operation, "list"), help = !strcmp(operation, "help"), info = !strcmp(operation, "info"),
        load = !strcmp(operation, "load"), unload = !strcmp(operation, "unload"), reload = !strcmp(operation, "reload");
    if ((!list && !help && !info && !load && !unload && !reload) || ((list || help) && target) ||
        ((load || unload || reload || info) && (!target || !target[0])) || (target && strlen(target) >= PATH_MAX))
        return jash_discovery_error(w, ctx, "module", mode, "invalid_options", 2);
    if (mode == JASH_DISCOVERY_TEXT && help)
        return jash_writes(w, ctx, "usage: module load <name|path> | unload <name> | reload <name> | list | info <name>\n") < 0 ? 125 : 0;
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "module", mode, "allocation_failed", 125);
    if (load || unload || reload) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "module", mode, "jash.module.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0;
    if (list) {
        for (int i = 0; i < g_modules.count; i++) if (g_modules.entries[i].active &&
                module_row(&rows, "list", NULL, &g_modules.entries[i], NULL, 1, 1, 0, 0, NULL)) break;
    } else if (help) {
        jmod_entry_t descriptor = {0};
        snprintf(descriptor.description, sizeof descriptor.description, "load <name|path> | unload <name> | reload <name> | list | info <name> | help");
        module_row(&rows, "help", NULL, &descriptor, NULL, 0, 0, 0, 0, NULL);
    } else {
        jmod_entry_t* existing = jmod_find(target);
        if (load && !existing) for (int i = 0; i < g_modules.count; i++)
            if (g_modules.entries[i].active && !strcmp(g_modules.entries[i].path, target)) { existing = &g_modules.entries[i]; break; }
        int before = existing != NULL, changed = 0, unloaded = 0;
        jmod_entry_t snapshot = {0};
        if (existing) snapshot = *existing;
        const char* error = NULL;
        char path[PATH_MAX] = {0};
        const jmod_entry_t* result = before ? &snapshot : NULL;
        if (info) {
            if (!before) error = "not_loaded";
        } else {
            if ((load || reload) && !(reload && before)) {
                if (!module_path(target, path, sizeof path)) error = "not_found";
            } else if (reload && before) memcpy(path, snapshot.path, strlen(snapshot.path) + 1u);
            int saved_stdout = -1;
            if (!error && jash_discovery_callback_begin(mode, &saved_stdout)) error = "callback_output_unavailable";
            if (!error && (unload || (reload && before))) {
                if (jmod_unload(target)) error = before ? "unload_failed" : "not_loaded";
                unloaded = before && !jmod_find(target); changed = unloaded;
            }
            if (!error && (load || reload)) {
                int count_before = g_modules.count;
                if (jmod_load(path, REQUESTED_CAPS)) error = "load_failed";
                else if (g_modules.count > count_before) {
                    result = &g_modules.entries[g_modules.count - 1]; changed = 1;
                } else error = "load_not_registered";
            }
            if (jash_discovery_callback_end(saved_stdout)) error = "callback_output_failed";
        }
        int after = info ? before : (result && jmod_find(result->name) != NULL);
        module_row(&rows, operation, target, result, path[0] ? path : NULL,
                    load && !before && error ? -1 : before, load && !result && error ? -1 : after, changed, unloaded, error);
        if (error) status = 1;
    }
    int rc;
    if (mode != JASH_DISCOVERY_TEXT || rows.error) rc = jash_discovery_emit_schema(w, ctx, "module", mode, &rows, status, "jash.module.v1");
    else {
        rc = status;
        if (list && !rows.count) jash_writes(w, ctx, "no modules loaded\n");
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* name = jash_discovery_field(&rows.items[i], "name");
            const jdv_value_t* version = jash_discovery_field(&rows.items[i], "version");
            const jdv_value_t* description = jash_discovery_field(&rows.items[i], "description");
            const jdv_value_t* error = jash_discovery_field(&rows.items[i], "error");
            if (error->kind != JDV_NOTHING) jash_writef(jash_stderr_writer, NULL, "module: %s: %.*s\n", target ? target : "",
                    (int)error->as.slice.len, error->as.slice.data);
            else if (list || load || info || reload) {
                if (jash_writef(w, ctx, "%.*s %.*s %.*s\n", (int)name->as.slice.len, name->as.slice.data,
                        (int)version->as.slice.len, version->as.slice.data,
                        (int)description->as.slice.len, description->as.slice.data) < 0) rc = 125;
                if (info) {
                    const jdv_value_t* path_value = jash_discovery_field(&rows.items[i], "path");
                    const jdv_value_t* caps = jash_discovery_field(&rows.items[i], "caps_declared");
                    if (jash_writef(w, ctx, "path: %.*s\ncaps: %llu\n", (int)path_value->as.slice.len,
                                    path_value->as.slice.data, (unsigned long long)caps->as.u64) < 0) rc = 125;
                }
            }
        }
    }
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_module(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_module_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
