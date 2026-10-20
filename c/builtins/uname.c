/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   builtins/uname.c — universal uname(1) backed by OSIF
 *
 *   Replaces the MINIX-only builtin_uname.c (which called bear_uname
 *   directly). One builtin, every target — exactly the property
 *   ADR-0017 promises. F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include "osif/osinfo.h"

#include <string.h>
#include <stdio.h>

static int uname_text(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    osif_sysid_t sid;
    if (bear_osinfo_sysid(&sid) != 0) {
        jash_writes(w, ctx, "uname: cannot get system info\r\n");
        return 1;
    }

    int f_a = 0, f_s = 0, f_n = 0, f_r = 0, f_v = 0, f_m = 0;
    const char* p = args ? args : "";
    if (*p) {
        while (*p) {
            if (*p == '-') {
                p++;
                while (*p && *p != ' ' && *p != '\t') {
                    switch (*p) {
                        case 'a': f_a = 1; break;
                        case 's': f_s = 1; break;
                        case 'n': f_n = 1; break;
                        case 'r': f_r = 1; break;
                        case 'v': f_v = 1; break;
                        case 'm': f_m = 1; break;
                        default:
                            jash_writef(w, ctx,
                                "uname: invalid option -- '%c'\r\n", *p);
                            return 2;
                    }
                    p++;
                }
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != '-') break;
        }
    }

    /* Default: -s (sysname). -a turns on everything. */
    if (!f_a && !f_s && !f_n && !f_r && !f_v && !f_m) f_s = 1;
    if (f_a) { f_s = f_n = f_r = f_v = f_m = 1; }

    int first = 1;
    #define EMIT(field) do { \
        if (!first) jash_writes(w, ctx, " "); \
        jash_writes(w, ctx, (field)); \
        first = 0; \
    } while (0)

    if (f_s) EMIT(sid.sysname);
    if (f_n) EMIT(sid.nodename);
    if (f_r) EMIT(sid.release);
    if (f_v) EMIT(sid.version);
    if (f_m) EMIT(sid.machine);
    jash_writes(w, ctx, "\r\n");

    #undef EMIT
    return 0;
}

int jash_b_uname_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(uname_text, s, w, ctx, argc, argv);
    if (selected < 0) return jash_discovery_error(w, ctx, "uname", mode, "conflicting_formats", 2);
    unsigned fields = 0;
    int ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (ended || argv[i][0] != '-' || !argv[i][1])
            return jash_discovery_error(w, ctx, "uname", mode, "invalid_options", 2);
        for (const char* p = argv[i] + 1; *p; p++) {
            if (*p == 'a') fields = 31u;
            else if (*p == 's') fields |= 1u;
            else if (*p == 'n') fields |= 2u;
            else if (*p == 'r') fields |= 4u;
            else if (*p == 'v') fields |= 8u;
            else if (*p == 'm') fields |= 16u;
            else return jash_discovery_error(w, ctx, "uname", mode, "invalid_options", 2);
        }
    }
    if (!fields) fields = 31u;
    osif_sysid_t system;
    if (bear_osinfo_sysid(&system)) return jash_discovery_error(w, ctx, "uname", mode, "system_info_unavailable", 1);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "uname", mode, "allocation_failed", 125);
    jdv_record_input_t record[] = {
        {JDV_LITERAL("sysname"), jash_discovery_string(&rows, fields & 1u ? system.sysname : NULL)},
        {JDV_LITERAL("nodename"), jash_discovery_string(&rows, fields & 2u ? system.nodename : NULL)},
        {JDV_LITERAL("release"), jash_discovery_string(&rows, fields & 4u ? system.release : NULL)},
        {JDV_LITERAL("version"), jash_discovery_string(&rows, fields & 8u ? system.version : NULL)},
        {JDV_LITERAL("machine"), jash_discovery_string(&rows, fields & 16u ? system.machine : NULL)},
        {JDV_LITERAL("target"), jash_discovery_string(&rows, bear_osinfo_target())},
    };
    jash_discovery_add(&rows, record, sizeof(record) / sizeof(record[0]));
    int rc = jash_discovery_emit(w, ctx, "uname", mode, &rows, 0);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_uname(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_uname_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
