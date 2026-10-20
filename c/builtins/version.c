// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — version builtin

#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <theme/banner.h>
#include <theme/colors.h>
#include <module/jash_crypto.h>
#include <osif/osinfo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef JASH_BUILD_TIMESTAMP
#define JASH_BUILD_TIMESTAMP "unknown"
#endif
#ifndef JASH_BUILD_DATE
#define JASH_BUILD_DATE "unknown"
#endif
#ifndef JASH_GITHASH
#define JASH_GITHASH "nogit"
#endif

static int binary_path(char* out, size_t cap) {
    out[0] = '\0';
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &size) == 0) return 0;
    return -1;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0 && (size_t)n < cap - 1) { out[n] = '\0'; return 0; }
    return -1;
#endif
}

static int binary_hash(const char* path, char out[JASH_SHA256_HEX_LEN]) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        jash_sha256_update(&ctx, buf, n);
    }
    int failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed) return -1;
    uint8_t digest[JASH_SHA256_DIGEST_LEN];
    jash_sha256_final(&ctx, digest);
    jash_sha256_hex(digest, out);
    return 0;
}

static int version_text(jash_state_t*, jash_writer_t, void*, const char*);

int jash_b_version_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(version_text, s, w, ctx, argc, argv);
    if (selected < 0) return jash_discovery_error(w, ctx, "version", mode, "conflicting_formats", 2);
    int ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        return jash_discovery_error(w, ctx, "version", mode, "invalid_options", 2);
    }
    char path[4096], hash[JASH_SHA256_HEX_LEN];
    const char* error = NULL;
    int have_path = binary_path(path, sizeof(path)) == 0;
    int have_hash = have_path && binary_hash(path, hash) == 0;
    if (!have_path) error = "binary_path_unavailable";
    else if (!have_hash) error = "binary_hash_unavailable";
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "version", mode, "allocation_failed", 125);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("version"), jash_discovery_string(&rows, JASH_VERSION)},
        {JDV_LITERAL("codename"), jash_discovery_string(&rows, JASH_CODENAME)},
        {JDV_LITERAL("build_date"), jash_discovery_string(&rows, JASH_BUILD_DATE)},
        {JDV_LITERAL("build_timestamp"), jash_discovery_string(&rows, JASH_BUILD_TIMESTAMP)},
        {JDV_LITERAL("git"), jash_discovery_string(&rows, JASH_GITHASH)},
        {JDV_LITERAL("target"), jash_discovery_string(&rows, bear_osinfo_target())},
        {JDV_LITERAL("binary_path"), jash_discovery_path(&rows, have_path ? path : NULL)},
        {JDV_LITERAL("sha256"), jash_discovery_string(&rows, have_hash ? hash : NULL)},
        {JDV_LITERAL("error"), jash_discovery_string(&rows, error)},
    };
    jash_discovery_add(&rows, fields, sizeof(fields) / sizeof(fields[0]));
    int rc = mode == JASH_DISCOVERY_DATA
        ? jash_discovery_emit_data_outcome(w, ctx, "version", &rows, error ? 1 : 0,
              error ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE, error)
        : jash_discovery_emit(w, ctx, "version", mode, &rows, error ? 1 : 0);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_version(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_version_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}

static int version_text(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s; (void)args;
    jash_writef(w, ctx,
        "%sjash%s %s%s%s %s%s%s\n",
        J_ROLE_BANNER, J_RESET,
        J_BOLD, JASH_VERSION, J_RESET,
        J_ROLE_ACCENT, JASH_CODENAME, J_RESET);
    jash_writef(w, ctx, "  build:      %s%s%s\n",
        J_ROLE_OK, JASH_BUILD_DATE, J_RESET);
    jash_writef(w, ctx, "  git:        %s%s%s\n",
        J_ROLE_OK, JASH_GITHASH, J_RESET);

    char path[1024];
    if (binary_path(path, sizeof(path)) == 0) {
        jash_writef(w, ctx, "  binary:     %s%s%s\n",
            J_ROLE_OK, path, J_RESET);
        char hash[JASH_SHA256_HEX_LEN];
        if (binary_hash(path, hash) == 0) {
            jash_writef(w, ctx, "  sha256:     %s%.16s%s...\n",
                J_ROLE_OK, hash, J_RESET);
        }
    }
    return 0;
}
