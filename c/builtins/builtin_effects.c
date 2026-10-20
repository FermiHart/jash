// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_effects — o EFEITO declarado de cada builtin.
//
// JINV-047. Um agente que precisa decidir "posso rodar isto sem perguntar?"
// hoje adivinha por regex sobre o NOME do comando. Quem reportou este pedido
// foi mordido duas vezes pelo mesmo erro no mesmo dia: um alias `Bash` furou o
// guard porque a comparação era sensível a caixa, e a allow-list de leitura
// deixou passar `grep` minúsculo. A raiz é sempre a mesma — segurança inferida
// de um nome.
//
// A correção não é uma regex melhor: é o shell DECLARAR o efeito. Quem sabe o
// que `rm` faz é quem o implementa.
//
// ── A regra que governa esta tabela ────────────────────────────────────────
//
// `unknown` é o PADRÃO, e é uma resposta legítima — não um buraco a preencher
// às pressas. Um builtin só sai de `unknown` depois que alguém leu o que ele
// faz. Um `effect` errado é pior que ausente: um guard que confia num `read`
// falso executa a coisa perigosa com confiança.
//
// Por isso a classificação abaixo é EXPLÍCITA, nome a nome. Derivá-la de
// heurística sobre o nome reproduziria exatamente o bug que ela existe para
// matar.
//
// ── Os quatro valores ──────────────────────────────────────────────────────
//
//   read    — só observa. Não escreve arquivo, não muda estado do shell, não
//             executa outro programa. Seguro para um agente rodar sozinho.
//   mutate  — escreve arquivo, muda estado do shell/sessão, ou grava histórico.
//             Reversível ou não, mas deixa marca.
//   exec    — pode executar código arbitrário (subprocesso, eval, plugin).
//             O efeito real é o do que ele roda, então é sempre o mais alto.
//   unknown — ninguém leu ainda. O consumidor deve tratar como perigoso.
//
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include "analysis_output.h"
#include <string.h>

typedef struct { const char* name; const char* effect; } eff_t;

/* Classificação explícita. Ordem alfabética por grupo, para revisão humana. */
static const eff_t EFFECTS[] = {
    /* ── read: só observam em toda a sua superfície de argumentos ─────── */
    { "pwd",        "read" },  { "true",       "read" },
    { "false",      "read" },  { ":",          "read" },
    { "echo",       "read" },  { "help",       "read" },
    { "version",    "read" },  { "uname",      "read" },
    { "test",       "read" },
    { "[",          "read" },  { "times",      "read" },
    { "cat",        "read" },  { "grep",       "read" },
    { "glob",       "read" },
    { "ps",         "read" },  { "du",         "read" },
    { "df",         "read" },  { "tread",      "read" },
    { "timeline",   "read" },  { "blame-file", "read" },
    { "explain",    "read" },  { "journal",    "read" },
    { "describe",   "read" },  { "where",      "read" },
    { "pick",       "read" },  { "sortby",     "read" },
    { "count",      "read" },  { "sum",        "read" },
    { "avg",        "read" },  { "uniqby",     "read" },
    { "calc",       "read" },  { "agent-guide","read" },
    { "compgen",    "read" },  { "lint",       "read" },
    { "dashboard",  "read" },

    /* ── mutate: escrevem arquivo ou estado ────────────────────────────── */
    { "cd",         "mutate" }, { "export",     "mutate" },
    { "unset",      "mutate" }, { "alias",      "mutate" },
    { "unalias",    "mutate" }, { "set",        "mutate" },
    { "shopt",      "mutate" }, { "local",      "mutate" },
    { "readonly",   "mutate" }, { "declare",    "mutate" },
    { "typeset",    "mutate" }, { "ulimit",     "mutate" },
    { "umask",      "mutate" }, { "read",       "mutate" },
    { "shift",      "mutate" }, { "pushd",      "mutate" },
    { "popd",       "mutate" }, { "dirs",       "mutate" },
    { "getopts",    "mutate" }, { "printf",     "mutate" },
    { "history",    "mutate" }, { "hash",       "mutate" },
    { "type",       "mutate" }, { "which",      "mutate" },
    { "jobs",       "mutate" },
    { "abbr",       "mutate" }, { "juxconfig",  "mutate" },
    { "config",     "mutate" }, { "theme",      "mutate" },
    { "touch",      "mutate" }, { "rm",         "mutate" },
    { "mkdir",      "mutate" }, { "rmdir",      "mutate" },
    { "chmod",      "mutate" }, { "chown",      "mutate" },
    { "ln",         "mutate" },
    { "gossip",     "mutate" },
    { "mirror",     "read" },
    { "trash",      "mutate" }, { "cp",         "mutate" },
    { "mv",         "mutate" }, { "sed",        "mutate" },
    { "ls",         "mutate" }, { "recall",     "mutate" },
    { "doctor",     "mutate" }, { "selfcheck",  "mutate" },
    { "statusbar",  "mutate" }, { "spinner",    "mutate" },
    { "checkpoint", "mutate" }, { "rewind",     "mutate" },
    { "undo",       "mutate" }, { "revert",     "exec" },
    { "change",     "mutate" }, { "state",      "mutate" },
    { "brain",      "mutate" }, { "kv",         "mutate" },
    { "goal",       "mutate" }, { "intent",     "mutate" },
    { "stance",     "mutate" }, { "preference", "mutate" },
    { "learn",      "mutate" }, { "mistake",    "mutate" },
    { "llm-invoice","mutate" }, { "digest",     "mutate" },
    { "reflect",    "mutate" }, { "handoff",    "mutate" },
    { "distill",    "mutate" }, { "recipe",     "mutate" },
    { "sigil",      "mutate" }, { "adopt",      "mutate" },
    { "contract",   "mutate" }, { "token",      "mutate" },
    { "trap",       "mutate" }, { "disown",     "mutate" },
    { "array",      "mutate" }, { "mapfile",    "mutate" },
    { "readarray",  "mutate" }, { "clear",      "mutate" },
    { "fc",         "mutate" }, { "jash_state", "mutate" },

    /* ── exec: podem rodar código arbitrário ───────────────────────────── */
    { "exec",       "exec" },  { "eval",        "exec" },
    { "find",       "exec" },  { "impact",      "exec" },
    { "source",     "exec" },  { ".",           "exec" },
    { "command",    "exec" },  { "builtin",     "exec" },
    { "xargs",      "exec" },  { "awk",         "exec" },
    { "proc",       "exec" }, { "breathe",    "exec" },
    { "timeout",    "exec" },  { "plugin",      "exec" },
    { "module",     "exec" },  { "enable",      "exec" },
    { "llm",        "exec" },
    { "mesh",       "exec" },  { "speculate",   "exec" },
    { "weave",      "exec" },
    { "expect",     "exec" },  { "agent-guard", "exec" },
    { "kill",       "exec" },  { "wait",        "exec" },
    { "fg",         "exec" },  { "bg",          "exec" },
    { "suspend",    "exec" },  { "debug",       "exec" },
    { "prompt",     "exec" },  { "verify",      "exec" },
    { "context",    "exec" },  { "osif",        "exec" },
    { "wc",         "exec" },  { "stat",        "exec" },
    { "diff",       "exec" },  { "tdiff",       "exec" },
    { "cost",       "exec" },  { "json",        "exec" },
    { "risk",       "exec" },  { "predict",     "exec" },
    { "exit",       "exec" },  { "logout",      "exec" },
    { "return",     "exec" },  { "break",       "exec" },
    { "continue",   "exec" },  { "let",         "exec" },
    { "env",        "exec" },  { "complete",    "exec" },
};
#define EFFECTS_N ((int)(sizeof(EFFECTS) / sizeof(EFFECTS[0])))

const char* jash_builtin_effect(const char* name) {
    if (!name) return "unknown";
    for (int i = 0; i < EFFECTS_N; i++)
        if (strcmp(EFFECTS[i].name, name) == 0) return EFFECTS[i].effect;
    return "unknown";   /* JINV-047: padrão fail-closed, e resposta legítima */
}

int jash_builtin_effect_classified(void) {
    int classified = 0;
    int n = jash_builtin_count();
    for (int i = 0; i < n; i++) {
        const jash_builtin_t* b = jash_builtin_at((uint32_t)i);
        if (b && strcmp(jash_builtin_effect(b->name), "unknown") != 0)
            classified++;
    }
    return classified;
}

const char* jash_builtin_effect_argv(const char* name, int argc, char* const* argv) {
    if (name && !strcmp(name, "osif")) return jash_osif_effect_argv(argc, argv);
    if (name && !strcmp(name, "mesh")) return jash_mesh_effect_argv(argc, argv);
    if (name && !strcmp(name, "change")) return jash_change_effect_argv(argc, argv);
    if (name && !strcmp(name, "expect")) return jash_expect_effect_argv(argc, argv);
    if (name && !strcmp(name, "assert")) return jash_assert_effect_argv(argc, argv);
    if (name && !strcmp(name, "ls") && argv) {
        int format = 0, delta = 0;
        for (int i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "--")) break;
            if (!strcmp(argv[i], "--semantic") || !strcmp(argv[i], "--relate") ||
                !strcmp(argv[i], "--monitor") || !strcmp(argv[i], "--pretty-limit")) { i++; continue; }
            if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) format = 1;
            if (!strcmp(argv[i], "--delta")) delta = 1;
        }
        if (format && !delta) return "read";
    }
    int structured = argc > 0 && argv && argv[0] &&
        (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2"));
    if (name && structured) {
        const char* operation = argc > 1 ? argv[1] : "";
        if (!strcmp(name, "state")) return argc == 2 && !strcmp(operation, "--no-git") ? "read" : "exec";
        if (!strcmp(name, "context")) return !*operation || !strcmp(operation, "bundle") ? "exec" : "read";
        if (!strcmp(name, "impact")) {
            for (int i = 1; i < argc && strcmp(argv[i], "--"); i++)
                if (!strcmp(argv[i], "--run")) return "exec";
            return "read";
        }
        if (!strcmp(name, "graph")) return !strcmp(operation, "build") ||
            !strcmp(operation, "push") || !strcmp(operation, "sync") ? "mutate" : "read";
        if (!strcmp(name, "debug")) return !strcmp(operation, "--parse") ||
            !strcmp(operation, "--locals") ? "read" : "exec";
        if (!strcmp(name, "distill")) return "mutate";
        if (!strcmp(name, "recipe")) {
            if (!strcmp(operation, "run")) return "exec";
            if (!strcmp(operation, "forget")) return "mutate";
            return "read";
        }
        if (!strcmp(name, "mem")) return !strcmp(operation, "distill") ? "mutate" : "read";
        if (!strcmp(name, "digest")) {
            int read = 0, session = 0, no_git = 0, output = 0;
            for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "--")) break;
                if (!strcmp(argv[i], "--output")) { output = 1; i++; continue; }
                if (!strcmp(argv[i], "--session")) session = 1;
                if (!strcmp(argv[i], "--no-git")) no_git = 1;
                if (!strcmp(argv[i], "--session") || !strcmp(argv[i], "--no-cache") ||
                    !strcmp(argv[i], "--fingerprint") || !strcmp(argv[i], "--manifest") ||
                    !strcmp(argv[i], "--diff-base") || !strcmp(argv[i], "--ledger") ||
                    !strcmp(argv[i], "--cache-status") || !strcmp(argv[i], "--fetch") ||
                    !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) read = 1;
                if (!strcmp(argv[i], "--diff-base") || !strcmp(argv[i], "--fetch") ||
                    !strcmp(argv[i], "--jobs") || !strcmp(argv[i], "--last")) i++;
            }
            return session && !no_git ? "exec" : output ? "mutate" : read ? "read" : "mutate";
        }
        if (!strcmp(name, "gap")) {
            if (argc == 1 || (argc == 2 && (!strcmp(operation, "list") || !strcmp(operation, "scan"))) ||
                (argc == 3 && !strcmp(operation, "list") && !strcmp(argv[2], "--all"))) return "read";
            if (!strcmp(operation, "verify")) return "exec";
            return "mutate";
        }
        if (!strcmp(name, "token") || !strcmp(name, "prompt") || !strcmp(name, "forge") ||
            !strcmp(name, "why") || !strcmp(name, "blame-file") || !strcmp(name, "journal") ||
            !strcmp(name, "recall") || !strcmp(name, "handoff")) return "read";
        if (!strcmp(name, "cost") && !strcmp(operation, "--dry-run")) return "read";
        if (!strcmp(name, "adopt") || !strcmp(name, "agent-guard") || !strcmp(name, "llm-invoice"))
            return !*operation || !strcmp(operation, "status") ? "read" : "mutate";
        if (!strcmp(name, "stance"))
            return !*operation || !strcmp(operation, "show") || !strcmp(operation, "list") ? "read" : "mutate";
        if (!strcmp(name, "kv"))
            return !*operation || !strcmp(operation, "get") || !strcmp(operation, "list") ? "read" : "mutate";
        if (!strcmp(name, "intent") || !strcmp(name, "resume"))
            return !*operation || !strcmp(operation, "get") || !strcmp(operation, "show") ? "read" : "mutate";
        if (!strcmp(name, "goal"))
            return !*operation || !strcmp(operation, "status") || !strcmp(operation, "recall") ? "read" : "mutate";
        if (!strcmp(name, "brain")) {
            if (!strcmp(operation, "intent") || (!strcmp(operation, "guard") &&
                (argc == 2 || (argc == 3 && !strcmp(argv[2], "status"))))) return "read";
            return "mutate";
        }
        if (!strcmp(name, "learn") || !strcmp(name, "preference"))
            return !*operation || !strcmp(operation, "list") || !strcmp(operation, "recall") ? "read" : "mutate";
        if (!strcmp(name, "mistake"))
            return !*operation || !strcmp(operation, "list") || !strcmp(operation, "avoid") ? "read" : "mutate";
        if (!strcmp(name, "reflect")) return !strcmp(operation, "improve") ? "read" : "mutate";
        if (!strcmp(name, "risk")) return !strcmp(operation, "avoid") ? "mutate" : "read";
        if (!strcmp(name, "predict")) return !strcmp(operation, "risk") && argc > 2 &&
            !strcmp(argv[2], "avoid") ? "mutate" : "read";
    }
    if (name && !strcmp(name, "sigil")) {
        int prefix = argc && (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2"));
        int operation = prefix ? 1 : 0;
        if (prefix && (argc == 1 || (argc == 2 && !strcmp(argv[1], "status")))) return "read";
        if (prefix) {
            if (argc > operation && (!strcmp(argv[operation], "push") ||
                (!strcmp(argv[operation], "replay") && argc > operation + 1 &&
                 !strcmp(argv[operation + 1], "--exec")))) return "exec";
            return "mutate";
        }
        if (argc > operation && (!strcmp(argv[operation], "push") || !strcmp(argv[operation], "restore") ||
            !strcmp(argv[operation], "replay"))) return "exec";
    }
    if (name && !strcmp(name, "selfcheck") && argc >= 1 && argc <= 2 &&
        (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2")) &&
        (argc == 1 || !strcmp(argv[1], "verify"))) return "read";
    if (name && !strcmp(name, "gossip")) {
        int i = 0;
        while (i < argc) {
            if (!strcmp(argv[i], "--json") || !strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) i++;
            else if (!strcmp(argv[i], "--channel") && i + 1 < argc) i += 2;
            else break;
        }
        if (i < argc && !strcmp(argv[i], "receive")) return "read";
    }
    if (name && !strcmp(name, "json") && argv &&
        ((argc == 1 && (!strcmp(argv[0], "--from-data") || !strcmp(argv[0], "--inspect-data") || !strcmp(argv[0], "--to-data"))) ||
         (argc == 2 && !strcmp(argv[0], "--to-data") && !strcmp(argv[1], "--jsonl")))) return "read";
    if (name && !strcmp(name, "xargs") && jash_xargs_plan_mode(argc, (char**)argv)) return "read";
    if (name && (!strcmp(name, "proc") || !strcmp(name, "breathe"))) {
        for (int i = 0; i < argc; i++) {
            const char* arg = argv[i];
            if (!strcmp(arg, "--json") || !strcmp(arg, "--data") || !strcmp(arg, "--data-v2") || !strcmp(arg, "--all")) continue;
            if (!strcmp(arg, "--interval") || !strcmp(arg, "--threshold") || !strcmp(arg, "--output") || !strcmp(arg, "-o")) { i++; continue; }
            if (!strcmp(name, "proc")) {
                if (!strcmp(arg, "tree") || !strcmp(arg, "diff")) return "read";
                if (!strcmp(arg, "snapshot")) return "mutate";
            } else {
                if (!strcmp(arg, "status")) return "read";
                if (!strcmp(arg, "check")) return "mutate";
            }
            break;
        }
    }
    /* Only the exact native environment query avoids external execution. */
    if (name && !strcmp(name, "env") && argc == 1 && argv[0] &&
        (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") ||
         !strcmp(argv[0], "--data-v2"))) return "read";
    /* The guide is read-only by default, but --save publishes a HOME file and
     * therefore requires mutation authority in agent-strict requests. */
    if (name && !strcmp(name, "agent-guide")) {
        for (int i = 0; i < argc; i++)
            if (argv[i] && !strcmp(argv[i], "--save")) return "mutate";
        return "read";
    }
    if (name && !strcmp(name, "wc") && jash_wc_report_mode(argc, argv)) return "read";
    if (name && !strcmp(name, "stat") && jash_stat_json_mode(argc, argv)) return "read";
    /* Leading structured modes select find's native no-fallback collector. */
    if (name && (!strcmp(name, "find") || !strcmp(name, "diff") || !strcmp(name, "tdiff") || !strcmp(name, "awk") || !strcmp(name, "verify")) && argc > 0 && argv[0] &&
        (!strcmp(argv[0], "--json") || !strcmp(argv[0], "--data") ||
         !strcmp(argv[0], "--data-v2"))) return "read";
    if (name && !strcmp(name, "df")) {
        for (int i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "--")) break;
            if (!strcmp(argv[i], "--baseline")) return "mutate";
            if (!strcmp(argv[i], "--delta") || !strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
        }
    }
    return jash_builtin_effect(name);
}
