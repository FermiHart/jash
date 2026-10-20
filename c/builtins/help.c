// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/help — curated, modern help system
// F E R M I ∞ H A R T

#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <theme/colors.h>
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>
#endif
#include <string.h>
#include <stdio.h>

#define DIM  "\033[2m"
#define CYAN "\033[36m"
#define GRN  "\033[32m"
#define YLW  "\033[33m"
#define MAG  "\033[35m"
#define BLU  "\033[34m"
#define WHT  "\033[37m"
#define RST  "\033[0m"
#define PRP  "\033[96m"  /* prompt cyan */
#define BLD  "\033[1m"

typedef struct { const char* name; const char* color; const char* cmds[32]; } cat_t;

static const cat_t g_cats[] = {
    { "Navigation", CYAN, {
        "cd", "pwd", "ls", "pushd", "popd", "dirs", NULL }},

    { "Files", GRN, {
        "touch", "rm", "trash", "cp", "mv", "du", "df", "grep", NULL }},

    { "Shell State", YLW, {
        "export", "unset", "alias", "unalias", "env",
        "history", "fc", "source", ".", NULL }},

    { "Undo & Safety", MAG, {
        "undo", "timeline", "tx", "sigil", "module", "clear", NULL }},

    { "Jobs & Process", BLU, {
        "jobs", "fg", "bg", "wait", "disown", "kill", "ps", NULL }},

    { "Scripting", PRP, {
        "echo", "printf", "read", "test", "[", "eval", "exec",
        "trap", "return", "if/while/for", "set", NULL }},

    { "Customization", WHT, {
        "theme", "statusbar", "spinner", "juxconfig", "abbr", "calc", NULL }},

    { "Help & AI", BLD, {
        "help", "?", "type", "version", "uname", "osif", NULL }},

    { NULL, NULL, { NULL }}
};

/* ── detail pages ──────────────────────────────────────────────────── */

static int show_detail(jash_writer_t w, void* ctx, const char* name) {
    /* `?` and `tx` are not in the builtin registry */
    if (strcmp(name, "?") == 0) goto detail_q;
    if (strcmp(name, "tx") == 0) goto detail_tx;

    int n = jash_builtin_count();
    const jash_builtin_t* found = NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(JASH_BUILTINS[i].name, name) == 0) { found = &JASH_BUILTINS[i]; break; }

    if (!found) {
        /* `?` and `tx` handled above via goto; this is for genuinely unknown names */
#ifndef JASH_AGENT_ONLY
        if (g_modules.count > 0 && jmod_find(name)) {
            jash_writef(w, ctx, "%s%s%s  loaded module verb\n", BLD, name, RST);
            jash_writef(w, ctx, "  Type '%s%s --help%s' for module help.\n", PRP, name, RST);
            return 0;
        }
#endif
        jash_writef(w, ctx, "%s%s%s  not found\n  Type %shelp%s for the command list.\n",
                    name, RST, BLU, RST);
        return 1;
    }

    jash_writef(w, ctx, "\n%s  %s%s\n", BLD, name, RST);
    jash_writef(w, ctx, "  %s%s%s\n\n", DIM, found->help ? found->help : "", RST);

    if (strcmp(name, "undo") == 0) {
        jash_writef(w, ctx,
            "  %sundo%s             revert last destructive op\n"
            "  %sundo 3%s           revert last 3\n"
            "  %stimeline%s         show session mutation log\n"
            "  %srm --undo f%s      move f to trash (recoverable 48h)\n"
            "\n"
            "  %sTx module:%s  %smodule load undo%s  →  %stx begin%s / %stx commit%s / %stx rollback%s\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            DIM,RST, PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "tx") == 0) {
        jash_writef(w, ctx,
            "  %smodule load undo%s     enable tx verb\n"
            "  %stx begin [name]%s      start a transaction\n"
            "  %stx status%s            show tx state + sigil level\n"
            "  %stx preview%s           preview mutations without executing\n"
            "  %stx diff%s              summarise mutation types\n"
            "  %stx commit%s            finalise the transaction\n"
            "  %stx rollback%s          undo all mutations since begin\n"
            "  %stx safe on|off%s       agent safety policies\n"
            "  %stx risk <cmd>%s        score destructive potential\n"
            "  %stx sessions%s          list saved session journals\n"
            "  %stx verify%s            BLAKE3 integrity check\n"
            "\n"
            "  %sOne-shot agent:%s  %sJASH_SAFE=1 jash -c '...'%s\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            DIM,RST, PRP,RST);
    } else if (strcmp(name, "module") == 0) {
        jash_writef(w, ctx,
            "  %smodule load <name>%s    load a .modjash module\n"
            "  %smodule list%s           show loaded modules\n"
            "  %smodule unload <name>%s  unload\n"
            "  %smodule info <name>%s    show caps and version\n"
            "\n"
            "  %sCore modules:%s\n"
            "    %ssigil%s  — cryptographic attestation (BLAKE3)\n"
            "    %sundo%s   — transactional safety (tx begin/commit/rollback)\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            DIM,RST, BLD,RST, BLD,RST);
    } else if (strcmp(name, "sigil") == 0) {
        jash_writef(w, ctx,
            "  %ssigil off%s        no recording (default, zero overhead)\n"
            "  %ssigil log%s         write commands to ~/.jash/commands.log\n"
            "  %ssigil sign%s        LOG + BLAKE3 chain + keyed proof\n"
            "  %ssigil seal%s        SIGN + adversarial restore battery\n"
            "  %ssigil chain%s       show the hash chain\n"
            "  %ssigil chain verify%s  verify chain integrity\n"
            "  %ssigil proof emit%s    create session proof\n"
            "  %ssigil proof verify%s  validate proof\n"
            "  %ssigil export%s        package as OCI artifact\n"
            "\n"
            "  %sAgent setup:%s  %sJASH_SIGIL=2%s  or  %s--agent%s\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST,
            DIM,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "?") == 0) {
        jash_writef(w, ctx,
            "  %s? <question>%s     ask the built-in AI agent\n"
            "  %s? on%s             enable AI mode (persistent)\n"
            "  %s? off%s            disable AI mode\n"
            "  %s? status%s         show AI mode state\n"
            "  %s? theme%s          switch AI theme (dark/amber/forest/...)\n"
            "\n"
            "  %sAI mode changes the prompt color and enables contextual help.%s\n"
            "  %sPrefix any command with ? to route through the AI engine.%s\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST,
            DIM,RST, DIM,RST);
    } else if (strcmp(name, "theme") == 0) {
        jash_writef(w, ctx,
            "  %stheme%s               list available themes\n"
            "  %stheme <name>%s        switch to a theme (amber, forest, ocean, mono...)\n"
            "  %stheme reload%s        reload current theme\n",
            PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "statusbar") == 0) {
        jash_writef(w, ctx,
            "  %sstatusbar on%s        enable intelligent status bar\n"
            "  %sstatusbar off%s       disable status bar\n"
            "  %sstatusbar toggle%s    flip state\n"
            "  %sstatusbar style%s     choose layout (minimal/compact/full)\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "spinner") == 0) {
        jash_writef(w, ctx,
            "  %sspinner on%s          enable idle prompt spinner\n"
            "  %sspinner off%s         disable spinner\n"
            "  %sspinner style <n>%s   choose animation (1-6)\n",
            PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "trash") == 0) {
        jash_writef(w, ctx,
            "  %strash <files>%s       move to trash (48h recoverable)\n"
            "  %strash list%s          list trash contents\n"
            "  %strash restore <f>%s   restore a file\n"
            "  %strash empty%s         permanently delete all trash\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "rm") == 0) {
        jash_writef(w, ctx,
            "  %srm file%s             delete\n"
            "  %srm -r dir%s           recursive\n"
            "  %srm --undo file%s      move to trash (recoverable)\n"
            "  %srm --dry-run *.log%s  preview only\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "ls") == 0) {
        jash_writef(w, ctx,
            "  %sls%s           current directory\n"
            "  %sls -l%s        long format\n"
            "  %sls -la%s       all files\n"
            "  %sls --tree%s    tree view\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    } else if (strcmp(name, "juxconfig") == 0) {
        jash_writef(w, ctx,
            "  %sjuxconfig%s           show current UX settings\n"
            "  %sjuxconfig set <k> <v>%s  configure a setting\n",
            PRP,RST, PRP,RST);
    }

    return 0;

detail_tx:
#ifndef JASH_AGENT_ONLY
    if (g_modules.count > 0 && jmod_find("undo")) {
        jash_writef(w, ctx, "\n%s  tx%s  transactional undo (loaded)\n\n", BLD, RST);
        jash_writef(w, ctx,
            "  %stx begin [name]%s      start a transaction\n"
            "  %stx status%s            show tx state + sigil level\n"
            "  %stx preview%s           preview mutations\n"
            "  %stx diff%s              summarise mutation types\n"
            "  %stx commit%s            finalise\n"
            "  %stx rollback%s          undo all since begin\n"
            "  %stx safe on|off%s       agent safety policies\n"
            "  %stx risk <cmd>%s        score destructive potential\n"
            "  %stx sessions%s          list saved session journals\n"
            "  %stx verify%s            BLAKE3 integrity check\n"
            "\n"
            "  %sOne-shot agent:%s  %sJASH_SAFE=1 jash -c 'tx begin fix; ...; tx commit'%s\n",
            PRP,RST, PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST, PRP,RST,
            DIM,RST, PRP,RST);
    } else {
        jash_writef(w, ctx, "\n%s  tx%s  transactional undo (module verb)\n\n", BLD, RST);
        jash_writef(w, ctx,
            "  %sModule not loaded.%s  Run: %smodule load undo%s\n\n"
            "  %stx begin [name]%s      start a transaction\n"
            "  %stx commit%s            finalise\n"
            "  %stx rollback%s          undo all since begin\n"
            "  %stx preview%s           preview mutations\n"
            "  %stx safe on|off%s       agent safety mode\n",
            DIM,RST, PRP,RST,
            PRP,RST, PRP,RST, PRP,RST, PRP,RST, PRP,RST);
    }
#endif
    return 0;

detail_q:
    jash_writef(w, ctx, "\n%s  ?%s  AI agent interface\n\n", BLD, RST);
    jash_writef(w, ctx,
        "  %s? <question>%s     ask the built-in AI agent\n"
        "  %s? on%s             enable AI mode (persistent, changes prompt color)\n"
        "  %s? off%s            disable AI mode\n"
        "  %s? status%s         show AI mode state\n"
        "  %s? theme%s          switch AI theme\n"
        "\n"
        "  %sAI mode: type ? followed by your question.%s\n"
        "  %sQuick: %smodule load ai%s  enables the full AI module.\n",
        PRP,RST, PRP,RST, PRP,RST, PRP,RST,
        PRP,RST, PRP,RST,
        DIM,RST, DIM, PRP,RST);
    return 0;
}

/* ── Main help page ────────────────────────────────────────────────── */

static int help_text(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;

    if (args) { while (*args == ' ' || *args == '\t') args++; if (*args) return show_detail(w, ctx, args); }

    jash_writef(w, ctx, "\n");
    jash_writef(w, ctx, "  %s%s▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚%s\n", DIM, BLD, RST);
    jash_writef(w, ctx, "  %s  JASH%s — the fastest shell with a %ssafety layer%s\n", BLD, RST, MAG, RST);
    jash_writef(w, ctx, "  %s  verifiable · transactional · agent-ready%s\n", DIM, RST);
    jash_writef(w, ctx, "  %s▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚▚%s\n\n", DIM, RST);

    for (int ci = 0; g_cats[ci].name; ci++) {
        const cat_t* c = &g_cats[ci];
        jash_writef(w, ctx, "  %s%s%s%s\n", c->color, BLD, c->name, RST);
        int col = 0;
        for (int i = 0; c->cmds[i]; i++) {
            const char* cmd = c->cmds[i];
            int len = (int)strlen(cmd);
            if (col > 0 && col + len + 2 > 68) { jash_writef(w, ctx, "\n"); col = 0; }
            if (col == 0) { jash_writef(w, ctx, "    "); col = 4; }
            jash_writef(w, ctx, "%s%s%s  ", c->color, cmd, RST);
            col += len + 2;
        }
        jash_writef(w, ctx, "\n\n");
    }

    jash_writef(w, ctx, "  %s─── Quick start ───%s\n", DIM, RST);
    jash_writef(w, ctx,
        "  %s? on%s              enable AI mode\n"
        "  %smodule load undo%s  enable transactions (%stx begin%s → %stx commit%s)\n"
        "  %smodule load sigil%s enable attestation  (%ssigil sign%s)\n"
        "  %sstatusbar on%s      show the smart status bar\n"
        "  %sspinner on%s        enable idle prompt animation\n"
        "  %stheme amber%s       switch to amber color theme\n"
        "  %shelp <cmd>%s        show detailed help for a command\n"
        "  %sTAB TAB%s           list all available commands\n"
        "\n"
        "  %sAgent one-liner:%s  %sJASH_SAFE=1 jash -c 'tx begin fix; ...; tx commit'%s\n",
        PRP,RST, PRP,RST, PRP,RST, PRP,RST,
        PRP,RST, PRP,RST, PRP,RST,
        PRP,RST, PRP,RST, PRP,RST,
        PRP,RST, PRP,RST, PRP,RST, PRP,RST,
        DIM,RST, PRP,RST);

    return 0;
}

static int help_record(jash_discovery_rows_t* rows, const char* name, const jash_builtin_t* builtin) {
    char id[128];
    if (builtin) snprintf(id, sizeof(id), "builtin:%s", builtin->name);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("name"), jash_discovery_string(rows, name)},
        {JDV_LITERAL("found"), jdv_value_bool(builtin != NULL)},
        {JDV_LITERAL("summary"), jash_discovery_string(rows, builtin ? builtin->help : NULL)},
        {JDV_LITERAL("declared_effect"), jash_discovery_string(rows, builtin ? jash_builtin_effect(name) : NULL)},
        {JDV_LITERAL("argv_native"), builtin ? jdv_value_bool(builtin->argv_fn != NULL) : jdv_value_nothing()},
        {JDV_LITERAL("enabled"), jdv_value_bool(builtin && jash_builtin_lookup(name))},
        {JDV_LITERAL("contract_id"), jash_discovery_string(rows, builtin ? id : NULL)},
    };
    return jash_discovery_add(rows, fields, sizeof(fields) / sizeof(fields[0]));
}

int jash_b_help_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(help_text, s, w, ctx, argc, argv);
    if (selected < 0) return jash_discovery_error(w, ctx, "help", mode, "conflicting_formats", 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "help", mode, "allocation_failed", 125);
    int ended = 0, names = 0, status = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && argv[i][0] == '-') { status = 2; break; }
        names++;
        const jash_builtin_t* builtin = NULL;
        for (int n = 0; n < jash_builtin_count(); n++) {
            const jash_builtin_t* candidate = jash_builtin_at((uint32_t)n);
            if (!strcmp(candidate->name, argv[i])) { builtin = candidate; break; }
        }
        if (!builtin) status = 1;
        if (help_record(&rows, argv[i], builtin)) break;
    }
    if (!names && status != 2) for (int n = 0; n < jash_builtin_count(); n++) {
        const jash_builtin_t* builtin = jash_builtin_at((uint32_t)n);
        if (help_record(&rows, builtin->name, builtin)) break;
    }
    int rc = status == 2 ? jash_discovery_error(w, ctx, "help", mode, "invalid_options", 2)
                         : jash_discovery_emit(w, ctx, "help", mode, &rows, status);
    jash_discovery_rows_free(&rows);
    return rc;
}

int jash_b_help(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_help_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}
