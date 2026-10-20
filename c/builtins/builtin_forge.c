// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_forge - Contextual Code Generation (ADR-0099)
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/argv.h>
#include <core/agent_json.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "semantic_builtin.h"

static int forge_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    if (argc && !strcmp(argv[0], "--")) { argc--; argv++; }
    if (argc != 1 || strlen(argv[0]) > 511) return semantic_error(call, "expected_description_up_to_511_bytes", 2);
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("template", semantic_symbol(sem, "test")),
        semantic_field("description", semantic_text(sem, argv[0])),
        semantic_field("status", semantic_symbol(sem, "scaffold")),
        semantic_field("validated", jdv_value_bool(0)),
        semantic_field("implemented", jdv_value_bool(0)),
        semantic_field("files_written", jdv_value_u64(0))
    };
    return semantic_finish(call, semantic_record(sem, fields, 7), 0);
}

static int forge_scaffold(jash_writer_t w, void* ctx, int argc, char** argv) {
    if (!argc) {
        jash_writes(w, ctx,
            "usage: forge [--json] [--] DESCRIPTION\n"
            "Emits an unfinished C test scaffold, not implemented or validated code.\n"
        );
        return 0;
    }
    int json_out = 0;
    int i = 0;
    if (!strcmp(argv[i], "--json")) { json_out = 1; i++; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (i < argc && argv[i][0] == '-') {
        jash_writes(jash_stderr_writer, NULL, "forge: unsupported option; only --json is implemented\n");
        return 2;
    }
    if (argc - i != 1 || strlen(argv[i]) > 511) {
        jash_writes(jash_stderr_writer, NULL, "forge: expected one description (up to 511 bytes)\n");
        return 2;
    }
    const char* description = argv[i];
    if (json_out) {
        char escaped[3073];
        jash_json_esc(description, escaped, sizeof(escaped));
        jash_writef(w, ctx, "{\"template\":\"test\",\"description\":\"%s\",\"status\":\"scaffold\"}\n", escaped);
        return 0;
    }
    jash_writes(w, ctx, "// Unfinished C test scaffold from jash forge\n");
    /* Do not let untrusted descriptions escape a comment into generated code. */
    jash_writes(w, ctx, "// Description: ");
    for (const unsigned char* p = (const unsigned char*)description; *p; p++) {
        if (*p < 32 || *p == 127 || *p == '\\') jash_writef(w, ctx, "<%02x>", *p);
        else w(ctx, (const char*)p, 1);
    }
    jash_writes(w, ctx, "\n#error \"forge scaffold: implement the test before use\"\n");
    jash_writes(w, ctx, "#include <stdio.h>\n");
    jash_writes(w, ctx, "#include <assert.h>\n\n");
    jash_writes(w, ctx, "int test_main(void) {\n");
    jash_writes(w, ctx, "    /* TODO: exercise behavior and assert an observable result. */\n");
    jash_writes(w, ctx, "    return 0;\n");
    jash_writes(w, ctx, "}\n");
    
    return 0;
}

int jash_b_forge(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    int selected = semantic_dispatch(s, w, ctx, "forge", "jash.forge.v1", args, forge_semantic);
    if (selected >= 0) return selected;
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0) return 1;
    int rc = forge_scaffold(w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}

int jash_b_forge_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    if (!s || !w) return 1;
    int selected = semantic_dispatch_argv(s, w, ctx, "forge", "jash.forge.v1", argc, argv, forge_semantic);
    if (selected >= 0) return selected;
    return forge_scaffold(w, ctx, argc, argv);
}
