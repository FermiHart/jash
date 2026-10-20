// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
//
// Module: builtins/which — locate a command in PATH (builtin version).
//
// Avoids forking /usr/bin/which by using jash_path_lookup() directly,
// which benefits from the in-memory PATH cache.

#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <parser/exec.h>
#include <string.h>

int jash_b_which_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return jash_discovery_error(w, ctx, "which", mode, "conflicting_formats", 2);
    int ended = 0, names = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && argv[i][0] == '-') return jash_discovery_error(w, ctx, "which", mode, "invalid_options", 2);
        names++;
    }
    if (!names) return jash_discovery_error(w, ctx, "which", mode, "name_required", 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "which", mode, "allocation_failed", 125);
    int status = 0;
    ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        int found = jash_discovery_resolve(s, &rows, argv[i], 0, 1);
        if (found < 0) break;
        if (!found) status = 1;
    }
    int rc = status;
    if (mode == JASH_DISCOVERY_TEXT && !rows.error) {
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* path = jash_discovery_field(&rows.items[i], "path");
            if (path->kind != JDV_NOTHING && jash_writef(w, ctx, "%.*s\n", (int)path->as.slice.len, path->as.slice.data) < 0) { rc = 125; break; }
        }
    } else rc = jash_discovery_emit(w, ctx, "which", mode, &rows, status);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_which(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_which_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
