// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "builtin.h"
#include "shell_output.h"
#include <core/argv.h>
#include <core/structured_output.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

int jash_b_pwd_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                    int argc, char** argv) {
    int json = 0, data = 0, formats = 0, physical = 0, selected = 0, invalid = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && !strcmp(argv[i], "--json")) { json = 1; formats++; }
        else if (!ended && (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2"))) { data = 1; formats++; }
        else if (!ended && !strcmp(argv[i], "-P")) { physical = 1; selected = 1; }
        else if (!ended && !strcmp(argv[i], "-L")) { physical = 0; selected = 1; }
        else invalid = 1;
    }
    if (invalid || formats > 1) {
        if (data) return jash_discovery_error(w, ctx, "pwd", JASH_DISCOVERY_DATA, "invalid_options", 2);
        if (json) return jash_structured_error(w, ctx, "pwd", "invalid_options", 2);
        jash_writes(jash_stderr_writer, NULL, "pwd: expected -L, -P or --json\n");
        return 2;
    }
    /* Existing JSON default is physical; text remains logical by default. */
    if ((json || data) && !selected) physical = 1;
    char* resolved = physical ? getcwd(NULL, 0) : NULL;
    const char* cwd = s->cwd;
    if (physical && !resolved) {
        if (data || json) return jash_discovery_error(w, ctx, "pwd", data ? JASH_DISCOVERY_DATA : JASH_DISCOVERY_JSON, "cwd_unavailable", 1);
        jash_writes(jash_stderr_writer, NULL, "pwd: working directory unavailable\n");
        return 1;
    }
    if (resolved) cwd = resolved;
    if (data) {
        int status = jash_shell_pwd_data(w, ctx, cwd, physical);
        free(resolved); return status;
    }
    if (json) {
        jash_structured_output_t out;
        if (jash_structured_begin(&out, w, ctx)) { free(resolved); return 125; }
        jash_jsonw_raw(&out.json, "{\"cwd\":");
        jash_jsonw_string_or_bytes(&out.json, cwd);
        jash_jsonw_raw(&out.json, "}\n");
        int status = jash_structured_finish(&out, 0);
        free(resolved); return status;
    }
    int status = jash_writef(w, ctx, "%s\n", cwd) < 0 ? 125 : 0;
    free(resolved); return status;
}

int jash_b_pwd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_pwd_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
