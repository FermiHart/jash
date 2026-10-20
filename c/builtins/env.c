// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Checked environment JSON, with literal argv delegation for POSIX env.
#include "builtin.h"
#include "shell_output.h"
#include <core/env.h>
#include <core/argv.h>
#include <core/structured_output.h>
#include <core/output_budget.h>
#include <parser/exec.h>
#include <stdlib.h>
#include <string.h>

int jash_b_env_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                    int argc, char** argv) {
    int json = argc && !strcmp(argv[0], "--json");
    int data = argc && (!strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2"));
    if (data) {
        if (argc != 1) return jash_discovery_error(w, ctx, "env", JASH_DISCOVERY_DATA, "invalid_options", 2);
        return jash_shell_env_data(s, w, ctx);
    }
    if (json && argc != 1)
        return jash_structured_error(w, ctx, "env", "invalid_options", 2);
    if (argc && !json) {
        char* args = jash_argv_join(argc, argv);
        if (!args) return 125;
        int rc = jash_exec_external(s, w, ctx, "env", args);
        free(args);
        return rc;
    }
    uint32_t count = jash_env_count(s);
    if (json) {
        /* The legacy map requires UTF-8 keys. Refuse an unrepresentable key
         * before emitting bytes; values have a reversible byte representation. */
        for (uint32_t i = 0; i < count; i++) {
            const char *key = NULL, *value = NULL;
            if (jash_env_at(s, i, &key, &value) && !jash_json_utf8_valid(key))
                return jash_structured_error(w, ctx, "env", "non_utf8_environment_key", 1);
        }
        jash_structured_buffer_t buffer;
        jash_structured_buffer_init(&buffer, 32u * 1024u * 1024u);
        jash_structured_output_t out = {0};
        jash_jsonw_init(&out.json, jash_structured_buffer_sink, &buffer);
        jash_jsonw_raw(&out.json, "{");
        int first = 1;
        for (uint32_t i = 0; i < count && !out.json.error; i++) {
            const char *key = NULL, *value = NULL;
            if (!jash_env_at(s, i, &key, &value)) continue;
            if (!first) jash_jsonw_raw(&out.json, ",");
            first = 0;
            jash_jsonw_string(&out.json, key);
            jash_jsonw_raw(&out.json, ":");
            jash_jsonw_string_or_bytes(&out.json, value ? value : "");
        }
        jash_jsonw_raw(&out.json, "}\n");
        int result = jash_jsonw_finish(&out.json) || buffer.error
            ? jash_structured_error(w, ctx, "env", "encoding_failed", 125)
            : jash_structured_document(w, ctx, "env", buffer.data, buffer.used, buffer.required, jash_budget_limit(), 0);
        jash_structured_buffer_free(&buffer);
        return result;
    }
    for (uint32_t i = 0; i < count; i++) {
        const char *key = NULL, *value = NULL;
        if (jash_env_at(s, i, &key, &value))
            if (jash_writef(w, ctx, "%s=%s\n", key, value) < 0) return 125;
    }
    return 0;
}

int jash_b_env(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_env_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
