// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_builtin.h"

static jdv_value_t guide_semantic_value(semantic_builtin_t* call, jdv_value_t builtins,
        jdv_value_t environment, jdv_value_t document, const char* path, size_t bytes,
        int save, int published, int status) {
    semantic_value_t* sem = &call->values;
    jdv_value_t artifact = jdv_value_nothing();
    if (save) {
        jdv_record_input_t detail[] = {
            semantic_field("path", semantic_path(sem, path)),
            semantic_field("format", semantic_symbol(sem, "markdown")),
            semantic_field("bytes", jdv_value_filesize(bytes)),
            semantic_field("published", jdv_value_bool(published)),
            semantic_field("durable", jdv_value_bool(!status))
        };
        artifact = semantic_record(sem, detail, 5);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, save ? "save" : "query")),
        semantic_field("source", semantic_symbol(sem, "compiled-registry-and-environment-docs")),
        semantic_field("metadata_only", jdv_value_bool(1)),
        semantic_field("qualification_source", semantic_symbol(sem, "--contract-json")),
        semantic_field("builtins", builtins), semantic_field("environment", environment),
        semantic_field("document", document), semantic_field("artifact", artifact),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("error", semantic_symbol(sem, status ? "publication_failed" : NULL)),
        semantic_field("effects_may_have_applied", jdv_value_bool(save))
    };
    return semantic_record(sem, fields, 12);
}

static int guide_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    int save = 0, markdown = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--save") && !save) save = 1;
        else if (!strcmp(argv[i], "--markdown") && !markdown) markdown = 1;
        else return semantic_error(call, "invalid_arguments", 2);
    }
    semantic_value_t* sem = &call->values;
    uint32_t count = jash_builtin_count();
    if (count > 4096u) return semantic_error(call, "registry_limit", 75);
    jdv_value_t* entries = calloc(count ? count : 1, sizeof(*entries));
    if (!entries) return semantic_error(call, "allocation_failed", 125);
    for (uint32_t i = 0; i < count; i++) {
        const jash_builtin_t* builtin = jash_builtin_at(i);
        if (!builtin || !builtin->name) { free(entries); return semantic_error(call, "invalid_registry_entry", 1); }
        jdv_record_input_t fields[] = {
            semantic_field("name", semantic_symbol(sem, builtin->name)),
            semantic_field("help", semantic_text(sem, builtin->help)),
            semantic_field("section", semantic_text(sem, guide_section(builtin->name))),
            semantic_field("why", semantic_text(sem, guide_why(builtin->name))),
            semantic_field("unique", jdv_value_bool(is_jash_unique(builtin->name))),
            semantic_field("argv_handler", jdv_value_bool(builtin->argv_fn != NULL))
        };
        entries[i] = semantic_record(sem, fields, 6);
    }
    jdv_value_t builtins = semantic_list(sem, entries, count); free(entries);
    size_t env_count = sizeof(ENV_DOCS) / sizeof(ENV_DOCS[0]) - 1;
    entries = calloc(env_count ? env_count : 1, sizeof(*entries));
    if (!entries) return semantic_error(call, "allocation_failed", 125);
    for (size_t i = 0; i < env_count; i++) {
        jdv_record_input_t fields[] = {
            semantic_field("name", semantic_symbol(sem, ENV_DOCS[i].var)),
            semantic_field("description", semantic_text(sem, ENV_DOCS[i].doc))
        };
        entries[i] = semantic_record(sem, fields, 2);
    }
    jdv_value_t environment = semantic_list(sem, entries, env_count); free(entries);
    guide_buffer_t guide = {0};
    if (save || markdown) {
        emit_complete_guide(guide_buffer_writer, &guide, 1);
        if (guide.failed || !guide.data) { free(guide.data); return semantic_error(call, "document_render_limit", 75); }
    }
    jdv_value_t document = markdown ? semantic_text_n(sem, guide.data, guide.len) : jdv_value_nothing();
    char path[4096] = {0};
    if (save) {
        const char* home = getenv("HOME");
        if (!home || *home != '/' || snprintf(path, sizeof(path), "%s/.jash/agent_guide.md", home) >= (int)sizeof(path)) {
            free(guide.data); return semantic_error(call, "invalid_home_path", 2);
        }
    }
    size_t mark = sem->arena.used;
    jdv_value_t candidate = guide_semantic_value(call, builtins, environment, document, path, guide.len, save, 1, 1);
    int preflight = semantic_preflight(call, &candidate);
    if (preflight) { free(guide.data); return semantic_error(call, "receipt_limit_before_publication", preflight); }
    sem->arena.used = mark;
    int status = 0, published = 0;
    if (save) {
        if (semantic_admit(call)) { free(guide.data); return 125; }
        status = guide_save(&guide, path, sizeof(path), &published) ? 1 : 0;
    }
    jdv_value_t result = guide_semantic_value(call, builtins, environment, document, path, guide.len, save, published, status);
    free(guide.data);
    return semantic_finish(call, result, status);
}
