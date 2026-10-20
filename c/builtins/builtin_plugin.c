/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <plugin.h>
#include <limits.h>
#include <string.h>

static int plugin_index(const char* name) {
    for (int i = 0; i < jash_plugin_count(); i++)
        if (!strcmp(jash_plugin_info_at(i)->name, name)) return i;
    return -1;
}

static int plugin_row(jash_discovery_rows_t* rows, const char* operation, const char* requested,
                       const jash_plugin_info_t* info, const char* path, int before, int after,
                       int changed, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("requested"), jash_discovery_string(rows, requested)},
        {JDV_LITERAL("name"), jash_discovery_string(rows, info ? info->name : NULL)},
        {JDV_LITERAL("version"), jash_discovery_string(rows, info ? info->version : NULL)},
        {JDV_LITERAL("description"), jash_discovery_string(rows, info ? info->description : NULL)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("loaded_before"), before < 0 ? jdv_value_nothing() : jdv_value_bool(before)},
        {JDV_LITERAL("loaded"), after < 0 ? jdv_value_nothing() : jdv_value_bool(after)},
        {JDV_LITERAL("changed"), jdv_value_bool(changed)},
        {JDV_LITERAL("ok"), jdv_value_bool(!error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

int jash_b_plugin_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "plugin", mode, "conflicting_formats", 2);
    const char* operands[2]; int count = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if ((!ended && argv[i][0] == '-') || count == 2)
            return jash_discovery_error(w, ctx, "plugin", mode, "invalid_options", 2);
        operands[count++] = argv[i];
    }
    const char* operation = count ? operands[0] : (mode == JASH_DISCOVERY_TEXT ? "help" : "list");
    const char* target = count > 1 ? operands[1] : NULL;
    int list = !strcmp(operation, "list"), help = !strcmp(operation, "help"),
        load = !strcmp(operation, "load"), unload = !strcmp(operation, "unload");
    if ((!list && !help && !load && !unload) || ((list || help) && target) ||
        ((load || unload) && (!target || !target[0])) || (target && strlen(target) >= PATH_MAX))
        return jash_discovery_error(w, ctx, "plugin", mode, "invalid_options", 2);
    if (mode == JASH_DISCOVERY_TEXT && help) {
        return jash_writes(w, ctx, "Usage: plugin load <path> | plugin unload <name> | plugin list\n") < 0 ? 125 : 0;
    }
    if (mode == JASH_DISCOVERY_TEXT && list) return jash_plugin_list(w, ctx);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "plugin", mode, "allocation_failed", 125);
    if (load || unload) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "plugin", mode, "jash.plugin.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0;
    if (list) {
        for (int i = 0; i < jash_plugin_count(); i++)
            if (plugin_row(&rows, "list", NULL, jash_plugin_info_at(i), jash_plugin_path_at(i), 1, 1, 0, NULL)) break;
    } else if (help) {
        jash_plugin_info_t info = {NULL, NULL, "load <path> | unload <name> | list | help"};
        plugin_row(&rows, "help", NULL, &info, NULL, 0, 0, 0, NULL);
    } else {
        int saved_stdout;
        if (jash_discovery_callback_begin(mode, &saved_stdout)) {
            plugin_row(&rows, operation, target, NULL, NULL, -1, -1, 0, "callback_output_unavailable");
            status = 125;
        } else if (load) {
            int prior_index = -1;
            for (int i = 0; i < jash_plugin_count(); i++)
                if (!strcmp(jash_plugin_path_at(i), target)) { prior_index = i; break; }
            int before = jash_plugin_count(), result = jash_plugin_load(target);
            const char* error = result ? "load_failed" : NULL;
            if (jash_discovery_callback_end(saved_stdout)) error = "callback_output_failed";
            int changed = jash_plugin_count() > before;
            const jash_plugin_info_t* info = changed ? jash_plugin_info_at(jash_plugin_count() - 1) : jash_plugin_info_at(prior_index);
            plugin_row(&rows, operation, target, info, target, changed ? 0 : prior_index >= 0 ? 1 : -1,
                        changed || prior_index >= 0 ? 1 : -1, changed, error);
            if (error) status = 1;
        } else {
            int index = plugin_index(target), before = index >= 0;
            char name[64], version[256], description[4096], path[PATH_MAX];
            jash_plugin_info_t snapshot = {NULL, NULL, NULL};
            if (before) {
                const jash_plugin_info_t* info = jash_plugin_info_at(index);
                memcpy(name, info->name, strlen(info->name) + 1u); snapshot.name = name;
                if (info->version) { memcpy(version, info->version, strlen(info->version) + 1u); snapshot.version = version; }
                if (info->description) { memcpy(description, info->description, strlen(info->description) + 1u); snapshot.description = description; }
                const char* source = jash_plugin_path_at(index); memcpy(path, source, strlen(source) + 1u);
            }
            int result = jash_plugin_unload(target);
            const char* error = result ? (before ? "unload_failed" : "not_loaded") : NULL;
            if (jash_discovery_callback_end(saved_stdout)) error = "callback_output_failed";
            int after = plugin_index(target) >= 0;
            plugin_row(&rows, operation, target, before ? &snapshot : NULL, before ? path : NULL,
                        before, after, before != after, error);
            if (error) status = 1;
        }
    }
    int rc;
    if (mode == JASH_DISCOVERY_TEXT && !rows.error) {
        rc = status;
        if (jash_writef(status ? jash_stderr_writer : w, status ? NULL : ctx,
                        "plugin: %s %s: %s\n", operation, target ? target : "", status ? "failed" : "ok") < 0) rc = 125;
    } else rc = jash_discovery_emit_schema(w, ctx, "plugin", mode, &rows, status, "jash.plugin.v1");
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_plugin(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_plugin_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
