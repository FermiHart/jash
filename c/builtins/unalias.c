// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "builtin.h"
#include "alias_report.h"
#include <core/alias.h>
#include <core/argv.h>
#include <string.h>

int jash_b_unalias_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (selected < 0) return jash_discovery_error(w, ctx, "unalias", mode, "conflicting_formats", 2);
    int ended = 0, names = 0, all = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && !strcmp(argv[i], "-a")) { all = 1; continue; }
        if ((!ended && argv[i][0] == '-') || !argv[i][0] || strlen(argv[i]) >= JASH_ALIAS_KEY_LEN)
            return jash_discovery_error(w, ctx, "unalias", mode, "invalid_options", 2);
        names++;
    }
    if ((!names && !all) || (names && all)) return jash_discovery_error(w, ctx, "unalias", mode, "invalid_options", 2);
    if (names > 1024) return jash_discovery_error(w, ctx, "unalias", mode, "operand_limit_exceeded", 75);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "unalias", mode, "allocation_failed", 125);
    int admitted = jash_discovery_effects_begin(&rows, w, ctx, "unalias", mode, "jash.alias.v1");
    if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    int status = 0;
    if (all) {
        while (jash_alias_count(s)) {
            const char *key, *value;
            if (!jash_alias_at(s, jash_alias_count(s) - 1u, &key, &value)) break;
            if (jash_alias_result_row(&rows, key, "remove", value, NULL, 1, NULL)) break;
            if (jash_alias_unset(s, key)) { status = 1; break; }
            if (jash_discovery_effects_flush(&rows)) break;
        }
    } else {
        ended = 0;
        for (int i = 0; i < argc; i++) {
            if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
            if (!ended && jash_discovery_format_flag(argv[i])) continue;
            const char* previous = jash_alias_get(s, argv[i]);
            if (jash_alias_result_row(&rows, argv[i], "remove", previous, NULL, previous != NULL, previous ? NULL : "not_found")) break;
            if (!previous || jash_alias_unset(s, argv[i])) status = 1;
            if (jash_discovery_effects_flush(&rows)) break;
        }
    }
    int rc = jash_alias_report_emit(w, ctx, "unalias", mode, &rows, status);
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_unalias(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_unalias_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
