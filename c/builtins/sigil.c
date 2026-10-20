// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ADR-0003 M3: `sigil` builtin — control the execution verification level

#include "builtin.h"
#include "semantic_builtin.h"
#include "discovery_output.h"
#include <module/sigil.h>
#include <module/sigil_native.h>
#include <module/loader.h>
#include <limits.h>
#include <string.h>
#ifndef JASH_TINALINUX
#include <dlfcn.h>
#endif

static const char* const lvl_names[] = { "off", "log", "sign", "seal" };
static const char* const lvl_desc[] = {
    "no recording — zero overhead",
    "log cmd + exit code  →  ~/.jash/commands.log",
    "LOG + BLAKE3/Ed25519 signing  (S2)",
    "SIGN + anti-bypass enforcement  (S5)",
};
static const char* const lvl_glyphs[] = { "", " \xe2\x97\x89 LOG ", " \xe2\x9c\x8d SIGN ", " \xe2\x8a\x97 SEAL " };

static int sigil_legacy(jash_state_t* s, jash_writer_t w, void* ctx, const char* args)
{
    (void)s;
    if (!args) args = "";
    while (*args == ' ') args++;

    /* No args → show current level */
    if (!*args) {
        int lvl = jash_sigil_get();
        jash_writef(w, ctx, "sigil: %d (%s)%s — %s\n",
                    lvl, lvl_names[lvl],
                    lvl > 0 ? lvl_glyphs[lvl] : "",
                    lvl_desc[lvl]);
        return 0;
    }

    int new_level = -1;
    if (strcmp(args, "off")  == 0 || strcmp(args, "0") == 0) new_level = SIGIL_OFF;
    else if (strcmp(args, "log")  == 0 || strcmp(args, "1") == 0) new_level = SIGIL_LOG;
    else if (strcmp(args, "sign") == 0 || strcmp(args, "2") == 0) new_level = SIGIL_SIGN;
    else if (strcmp(args, "seal") == 0 || strcmp(args, "3") == 0) new_level = SIGIL_SEAL;

    if (new_level < 0) {
        /* Unknown subcommand — delegate to sigil.modjash if loaded */
#ifndef JASH_TINALINUX
        if (jmod_find("sigil") == NULL) {
            char spath[PATH_MAX];
            if (jmod_resolve("sigil", spath, sizeof(spath)) == 1)
                jmod_load(spath, 0xFFFFFFFF);
        }
#endif
        if (g_modules.count > 0) {
            jmod_call_t mc = { s, (jmod_writer_t)w, ctx, "sigil", args };
            int mrc = jmod_dispatch("sigil", &mc);
            if (mrc >= 0) return mrc;
        }
        jash_writes(w, ctx,
            "usage: sigil [off|0|log|1|sign|2|seal|3]\n"
            "  off  (0) — no recording\n"
            "  log  (1) — log commands to ~/.jash/commands.log\n"
            "  sign (2) — log + BLAKE3/Ed25519  [S2]\n"
            "  seal (3) — sign + anti-bypass    [S5]\n"
            "  chain         — show chain log  (needs sigil.modjash)\n"
            "  chain verify  — verify chain integrity\n");
        return 1;
    }

    int old = jash_sigil_get();
    jash_sigil_set(new_level);
    if (old != new_level)
        jash_writef(w, ctx, "sigil: %d → %d (%s)\n", old, new_level, lvl_names[new_level]);
    return 0;
}

static int sigil_level(const char* text) {
    for (int i = 0; i < 4; i++) {
        if (!strcmp(text, lvl_names[i]) || (text[0] == '0' + i && !text[1])) return i;
    }
    return -1;
}

static int sigil_native(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    int level = argc ? sigil_level(argv[0]) : -1;
    int changing = argc && strcmp(argv[0], "status");
    if (changing && level < 0) {
        static const char* const operations[] = {"list", "chain", "verify", "proof", "quarantine",
            "ratchet", "history", "blame", "diff", "bisect", "caps", "export", "push", "restore", "replay", "record"};
        int known = 0;
        for (size_t i = 0; i < sizeof operations / sizeof operations[0]; i++)
            if (!strcmp(argv[0], operations[i])) { known = 1; break; }
        if (!known) return semantic_error(call, "invalid_operation", 2);
        /* Loading a native module may initialize keys and store directories.
         * Admit before loading as well as before the delegated operation. */
        int admission = semantic_admit_rows(call, 256); if (admission) return admission;
#ifndef JASH_TINALINUX
        jmod_entry_t* module = jmod_find("sigil");
        if (!module) {
            char path[PATH_MAX];
            if (jmod_resolve_installed("sigil", path, sizeof path) != 1)
                return semantic_error(call, "sigil_module_unavailable", 1);
            if (jmod_load_expected(path, UINT32_MAX, "sigil"))
                return semantic_error(call, "sigil_module_load_failed", 1);
            module = jmod_find("sigil");
        }
        if (!module) return semantic_error(call, "sigil_module_unavailable", 1);
        void* symbol = dlsym(module->dlhandle, JASH_SIGIL_NATIVE_SYMBOL);
        jash_sigil_native_fn provider = NULL;
        _Static_assert(sizeof provider == sizeof symbol, "native module function pointer ABI");
        memcpy(&provider, &symbol, sizeof provider);
        if (!provider) return semantic_error(call, "sigil_native_provider_unavailable", 1);
        return provider(call, state, argc, argv);
#else
        (void)state;
        return semantic_error(call, "sigil_module_unavailable", 1);
#endif
    }
    if (argc > 1) return semantic_error(call, "invalid_arguments", 2);
    int before = jash_sigil_get();
    if (changing) {
        int admission = semantic_admit(call); if (admission) return admission;
        jash_sigil_set(level);
    }
    int current = jash_sigil_get();
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.sigil.level.v1")),
        semantic_field("scope", semantic_symbol(values, "shell-process")),
        semantic_field("level", jdv_value_i64(current)),
        semantic_field("name", semantic_symbol(values, lvl_names[current])),
        semantic_field("previous_level", jdv_value_i64(before)),
        semantic_field("changed", jdv_value_bool(current != before)),
        semantic_field("module_loaded", jdv_value_bool(jmod_find("sigil") != NULL)),
        semantic_field("verification_performed", jdv_value_bool(0))
    };
    return semantic_finish(call, semantic_record(values, fields, sizeof fields / sizeof fields[0]), 0);
}

int jash_b_sigil_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    /* New uniform prefix modes use the native module extension. Historical
     * module-specific JSON invocations retain their compatibility grammar. */
    int mode = argc && (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2"));
    if (mode) return semantic_dispatch_argv(state, writer, context, "sigil", "jash.sigil.v1", argc, argv, sigil_native);
    char* arguments = semantic_legacy_join(argc, argv);
    if (!arguments) return 125;
    int result = sigil_legacy(state, writer, context, arguments);
    free(arguments); return result;
}

int jash_b_sigil(jash_state_t* state, jash_writer_t writer, void* context, const char* arguments) {
    jash_argv_t parsed = {0};
    if (jash_argv_build(arguments ? arguments : "", &parsed) < 0) return 2;
    int result = jash_b_sigil_argv(state, writer, context, parsed.argc, parsed.argv);
    jash_argv_free(&parsed); return result;
}
