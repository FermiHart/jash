// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/builtin — registry of true shell builtins.
//
// Per manifesto §3, the new jash carries ONLY the truly-shell things
// as builtins. Anything that maps to a /bin/<cmd> is left to PATH
// lookup with a fallback shim (builtins/fallback/) for boot before
// /bin exists.

#ifndef JASH_BUILTINS_BUILTIN_H
#define JASH_BUILTINS_BUILTIN_H

#include <theme/io.h>
#include <core/state.h>

/* Builtin function signature. `args` is the rest-of-line AFTER the
 * command name (may be NULL/empty). Returns the shell exit code. */
typedef int (*jash_builtin_fn)(jash_state_t* s, jash_writer_t w, void* ctx,
                               const char* args);

/* ADR-0036 W2: argv-based builtin signature — `argv` is already tokenized
 * respecting quotes (core/argv.h), so a quoted multi-word argument survives
 * as ONE argv[i] instead of being re-split on whitespace (BUG-038/BUG-044).
 * A builtin implementing this should NOT also re-split its own args. */
typedef int (*jash_builtin_argv_fn)(jash_state_t* s, jash_writer_t w, void* ctx,
                                    int argc, char** argv);

typedef struct {
    const char*           name;
    jash_builtin_fn        fn;        /* legacy string-based entry point */
    const char*            help;
    jash_builtin_argv_fn   argv_fn;   /* if set, jash_builtin_dispatch prefers this */
} jash_builtin_t;


/* JINV-046 — uma flag desconhecida nunca vira DADO.
 *
 * Varios builtins tratavam qualquer palavra como posicional, entao uma flag
 * digitada por engano — ou por sondagem, que e como um agente descobre
 * capacidades — virava nome:
 *
 *   $ jash -c 'checkpoint --json'
 *   checkpoint '--json' @ depth 0      <- criou um checkpoint CHAMADO "--json"
 *   $ jash -c 'rewind --json'
 *   rewind: already at or before '--json'
 *   $ jash -c 'impact --json'
 *   impact: map is COLD for --json
 *
 * O `checkpoint` e o pior: EXECUTOU uma acao com nome errado. Um agente
 * sondando cria lixo de estado sem perceber.
 *
 * Mesma raiz do JINV-017, do JINV-044 e do JINV-045: aceitar-e-ignorar. Aqui e
 * ainda pior que ignorar, porque a entrada errada vira efeito.
 *
 * Devolve 1 (e escreve o erro) quando `args` comeca com um `--flag` que nao
 * esta em `conhecidas` (lista NULL-terminada, pode ser vazia). 0 = siga.
 * Gate: tests/test_jinv046_unknown_flags.sh */
int jash_reject_unknown_flag(const char* builtin_name, const char* args,
                             const char* const* conhecidas,
                             jash_writer_t w, void* ctx);


/* JINV-047 — o EFEITO declarado de um builtin: "read" | "mutate" | "exec" |
 * "unknown". Declarado explicitamente em builtins/builtin_effects.c, nome a
 * nome — derivar de heuristica sobre o nome reproduziria exatamente o bug que
 * isto existe para matar (seguranca inferida por regex sobre o nome).
 *
 * "unknown" e o PADRAO e uma resposta legitima: o consumidor deve trata-lo
 * como perigoso. Um effect errado e pior que ausente. */
const char* jash_builtin_effect(const char* name);
const char* jash_builtin_effect_argv(const char* name, int argc, char* const* argv);
int jash_stat_json_mode(int argc, char* const* argv);
int jash_wc_report_mode(int argc, char* const* argv);
int jash_builtin_effect_classified(void);

int jash_b_assert(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_assert_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                       int argc, char** argv);
int jash_b_why(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_resume(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Registry — NULL-terminated. Defined in builtins/registry.c. */
extern const jash_builtin_t JASH_BUILTINS[];

/* Lookup by name. Returns the entry or NULL. */
const jash_builtin_t* jash_builtin_lookup(const char* name);

/* Convenience: count entries (excluding terminator). */
int jash_builtin_count(void);

/* Iterate: returns the i-th builtin or NULL when out of range. */
const jash_builtin_t* jash_builtin_at(uint32_t idx);

/* ADR-0036 W2: single dispatch point for calling a builtin. Prefers
 * b->argv_fn (tokenizing `args` quote-aware first) and falls back to the
 * legacy b->fn(args) path when argv_fn is NULL. Every call site that used
 * to invoke b->fn(...) directly should go through this instead — it is the
 * only place that needs to know both calling conventions exist. */
int jash_builtin_dispatch(const jash_builtin_t* b, jash_state_t* s,
                          jash_writer_t w, void* ctx, const char* args);

/* Each builtin lives in its own file. Public signatures below. */

int jash_b_cd       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_pwd      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_pwd_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_exit     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_true     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_false    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_colon    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_echo     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
/* JINV-079: a porta VALIDA do echo — a saida se monta do ARGV, nao da linha
 * crua (a linha crua ja perdeu as fronteiras de palavra). A `fn` de string
 * acima segue registrada como fallback do dispatcher. */
int jash_b_echo_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                     int argc, char** argv);
int jash_b_cd_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_pushd_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_popd_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_dirs_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_array_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_unset_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_shopt_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_umask_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_ulimit_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_times_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_alias_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_unalias_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_enable_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_hash_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_plugin_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_module_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_help     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_help_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_env      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_env_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_ls_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_export   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_unset    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_alias    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_unalias  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_history  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_fc       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_type     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_type_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_which    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_which_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_version  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_version_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_clear    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_uname    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_uname_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_osif     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_osif_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
const char* jash_osif_effect_argv(int argc, char* const* argv);
int jash_b_osif_port(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
char jash_osif_summary(char* out, size_t outsz);

/* sys/ wrappers below are reachable as builtins too — thin shims. */
int jash_b_uptime   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_free     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_kill     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_ls       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_touch    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_mkdir    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_rmdir    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_chmod    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_chown    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_ln       (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_cat      (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_find     (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_glob     (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_sed      (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_wc       (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_stat     (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_xargs    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_diff     (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_awk      (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_rm       (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_trash    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
/* ADR-0042 F8: time-travel read without rewind */
int jash_b_tread    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_tdiff    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
/* ADR-0036 W2: migrated to argv (quote-aware, dispatcher-tokenized). */
int jash_b_grep     (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_timeout  (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
/* ADR-0036 W2: migrated to argv (quote-aware, dispatcher-tokenized). */
int jash_b_cp       (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_mv       (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_ps       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_du       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_du_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_df       (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_df_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_forge_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_gossip_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
/* Durable trash publication, shared by trash and rm (regular-file EXDEV). */
int jash_trash_move_file(const char* src, const char* trash_dir,
                         const char* trash_name, const char* orig_dir);
/* Wave 2: Process snapshots */
int jash_b_proc      (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_proc_snapshot(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_proc_restore(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_proc_tree   (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_proc_diff   (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_recall_chain(jash_writer_t w, void* ctx, const char* args); /* ADR-0023 E1: recall cmds/did/history */
int jash_recall_chain_stats(jash_writer_t w, void* ctx);             /* ADR-0023 E1: recall stats */
int jash_recall_chain_adoption(jash_writer_t w, void* ctx, int json); /* BI: recall adoption */
int jash_b_expect   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args); /* ADR-0023 E3 */
int jash_b_explain  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args); /* ADR-0023 E3b */
int jash_explain_risk(const char* line, const char** reason_out);
int jash_b_mesh     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args); /* ADR-0024 E4 */
int jash_b_gap      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args); /* capability-gap radar */
int jash_b_gap_argv (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv); /* ADR-0036 W2 */
int jash_b_dashboard(jash_state_t* s, jash_writer_t w, void* ctx, const char* args); /* BI: usage dashboard */
int jash_b_reboot   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_halt     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_live_reload (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* A6: test/[ — test expressions (porta de string legada; segue registrada
 * como `fn`, que e o fallback do dispatcher). */
int jash_builtin_test (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_builtin_bracket (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* JINV-066: porta argv (ADR-0036 W2). A semantica do `test` do POSIX e
 * definida pela CONTAGEM de argumentos — o que um reconhecedor de prefixo
 * sobre a linha crua nao consegue expressar. Era exatamente por isso que o
 * `!` de `[ ! -f x ]` virava operando (string nao vazia = verdadeiro) e toda
 * clausula de guarda defensiva falhava ABERTA. */
int jash_builtin_test_argv (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_builtin_bracket_argv (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);

/* ADR-0042 F7: attested authorship per file */
int jash_b_blamefile(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0042 F10: session → executable recipe */
int jash_b_distill(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_recipe (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_distill_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
int jash_b_recipe_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);

/* Job control: jobs, wait, fg, bg, disown, suspend */
int jash_b_jobs (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_wait (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_fg   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_bg   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_disown (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_suspend(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Functions: return */
int jash_b_return (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Loop control: break, continue */
int jash_b_break (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_continue (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Exit status display */
int jash_b_exit_status (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Shell options */
int jash_b_set (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* JINV-067 — UMA porta só para as opções de shell.
 *
 * `set -e` (dentro do script) e `jash -e -c …` (na linha de comando) têm que
 * ser a MESMA tabela. Enquanto foram duas, a linha de comando tinha um
 * conjunto próprio de três letras (`l i s`) e engolia em silêncio tudo o que
 * não estivesse nele — inclusive o `-c` que vinha depois. Ver JINV-067 em
 * c/main.c.
 *
 * `s == NULL` = modo VALIDAÇÃO: responde se a letra/nome existe sem escrever
 * estado nenhum. É como main.c recusa uma flag desconhecida ANTES de alocar
 * ou rodar qualquer coisa, sem manter uma segunda lista de letras válidas.
 *
 * Retorno:
 *   JASH_OPT_OK          (0)  conhecida (e aplicada, se s != NULL)
 *   JASH_OPT_UNKNOWN    (-1)  não existe neste shell → erro alto, rc 2
 *   JASH_OPT_UNSUPPORTED(-2)  existe no bash, jash NÃO a sustenta (`-n`,
 *                             noexec) → erro alto, rc 2, nunca rc 0 calado.
 *                             Ver JINV-069. */
#define JASH_OPT_OK            0
#define JASH_OPT_UNKNOWN     (-1)
#define JASH_OPT_UNSUPPORTED (-2)
int jash_shell_opt_short(jash_state_t* s, char opt, int enable);
int jash_shell_opt_long (jash_state_t* s, const char* name, int enable);
/* JINV-069 — a mensagem única de `-n`/noexec, para main.c e para `set`
 * dizerem exatamente a mesma coisa. Escreve em stderr. */
void jash_shell_opt_noexec_refuse(const char* como_foi_pedido);

int jash_b_source (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_dot    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
/* ADR-0036 W2: migrated to argv (quote-aware, dispatcher-tokenized). */
int jash_b_printf (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);

/* SinguOS port (c/osif/singuos/): kernel-integrated builtins, never host. */
#ifdef JASH_TARGET_SINGUOS
int jash_b_irq     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_genesis (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_ring3_selftest(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_cage    (jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);
#endif
int jash_b_read   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
/* Internal non-interactive redirect path; fd remains owned by the caller. */
int jash_b_read_from_fd(jash_state_t* s, jash_writer_t w, void* ctx,
                        const char* args, int input_fd);
int jash_b_exec   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_eval   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_trap   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_run_pending_traps(jash_state_t* s, jash_writer_t w, void* ctx);
int jash_run_trap_debug(jash_state_t* s, jash_writer_t w, void* ctx);
int jash_run_trap_return(jash_state_t* s, jash_writer_t w, void* ctx);
int jash_run_trap_exit(jash_state_t* s, jash_writer_t w, void* ctx);
int jash_run_trap_err(jash_state_t* s, jash_writer_t w, void* ctx);
int jash_b_let    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_shift  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_local  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_readonly(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_declare(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_typeset(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_readonly_is_set(const char *name);
void jash_readonly_mark(const char *name);
long jash_trash_ttl_for(long size_bytes);
int jash_b_ulimit (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_umask  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_times  (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_command(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_shopt   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_compgen (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_complete(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
const char* jash_complete_get_words(const char* command);
const char* jash_complete_get_action(const char* command);
int jash_b_array(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_mapfile(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_getopts(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_pushd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_popd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_dirs(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_abbr(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_juxconfig(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_calc(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_debug(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);



/* Phase 6.4: builtin, enable, hash, logout */
int jash_b_builtin_cmd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_enable(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_hash(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_logout(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Disabled-builtin check (implemented in enable.c) */
int jash_builtin_is_disabled(const char* name);

/* Phase 8: plugin system */
int jash_b_plugin(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_module(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_sigil (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_sigil_argv(jash_state_t*, jash_writer_t, void*, int, char**);

/* Persistent state management */
int jash_b_jash_state(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Theme system — runtime-switchable color themes */
int jash_b_theme(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Status bar — toggle intelligent status bar */
int jash_b_statusbar(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Idle prompt spinner — on/off/toggle */
int jash_b_spinner(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Configuration management */
int jash_b_jash_config(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* F0: context compiler for AI agents — map/fold/bundle */
int jash_b_context(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_change_argv(jash_state_t*, jash_writer_t, void*, int, char**);
const char* jash_change_effect_argv(int, char* const*);

/* ADR-0028: self-digest — project briefing for agents */
int jash_b_digest(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_digest_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);

/* ADR-0026 Tier 0: live system state mirror for agents */
int jash_b_state(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_state_argv(jash_state_t*, jash_writer_t, void*, int, char**);

/* ADR-0026 Tier 1: structured adapters for legacy CLIs */
int jash_b_json(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_json_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_weave_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_mirror_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_adopt_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_agent_guard_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_llm_invoice_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_stance_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_token_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_brain_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_prompt_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_revert_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_selfcheck_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_goal_argv(jash_state_t*, jash_writer_t, void*, int, char**);
const char* jash_mesh_effect_argv(int, char* const*);
int jash_b_journal_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_blamefile_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_why_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_mesh_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_mistake_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_learn_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_preference_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_reflect_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_predict_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_risk_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_kv_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_intent_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_resume_argv(jash_state_t*, jash_writer_t, void*, int, char**);

/* ADR-0026 Tier 1: speculative execution sandbox */
int jash_b_speculate(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0026 Tier 1: cross-domain undo/revert */
int jash_b_revert(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0026 Tier 2: cost estimator */
int jash_b_cost(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_cost_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_dashboard_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_recall_argv(jash_state_t*, jash_writer_t, void*, int, char**);

/* ADR-0026 Tier 2: output contract assertions */
int jash_b_contract(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0026 Tier 3: agent anti-bypass guard control panel */
int jash_b_agent_guard(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0040 W2: adoption governor control panel */
int jash_b_adopt(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0026 Tier 3: auto-generated agent field guide */
int jash_b_agent_guide(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_agent_guide_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                            int argc, char** argv);

/* ADR-0033: Liquid Reasoning Engine control panel */
int jash_b_brain(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0033: Intention Memory Caching Protocol */
int jash_b_goal(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0033 Wave 3: LLM token economics / invoice */
int jash_b_llm_invoice(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0003: sovereign spec-driven change engine */
int jash_b_change(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0034: Agent Self-Improvement Primitives */
int jash_b_mistake   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_token     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_learn     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_predict   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_risk      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_preference(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_reflect   (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_selfcheck (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_prompt    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_selfcheck_auto(void);

/* F8/F9: agent ergonomics — kv store + environment doctor */
int jash_b_kv    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_doctor(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_recall(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_recall_why(jash_writer_t w, void* ctx, const char* pattern);
unsigned jash_recall_guard(const char* line, unsigned long long* out_seq, unsigned long long* out_ts, int* out_rc);
int jash_b_verify(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Wave 4: Advanced Linting */
int jash_b_lint(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Journal — session timeline with causal auto-tags */
int jash_b_journal(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Stance — operational modes */
int jash_b_stance(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Impact — test impact map learned from the chain (ADR-0038 W5) */
int jash_b_impact(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* Handoff — session handoff compiler (ADR-0038 W8) */
int jash_b_handoff(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_handoff_argv(jash_state_t*, jash_writer_t, void*, int, char**);

/* LLM session intent anchor — ADR-0022 */
int  jash_b_intent    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
void jash_intent_init (jash_state_t* s);

/* Structured pipe filters */
int jash_b_where    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_pick     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_sort     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_count    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_sum      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_avg      (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_uniq     (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_describe (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0099: Shell inteligente contextual */
int jash_b_breathe(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_mirror(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_gossip(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_sense(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_forge(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_weave(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);

/* ADR-0053: living graph — repo knowledge map that learns from attested use */
int jash_b_graph(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_graph_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv); /* ADR-0036 W2 */

/* ADR-0054: mem — session memory distilled from the attested chain,
 * verifiable (receipt) and outcome-aware; zero LLM */
int jash_b_mem(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_mem_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv); /* ADR-0036 W2 */


/* Native management argv boundaries. */
int jash_b_jash_state_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_jash_config_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_doctor_argv(jash_state_t*, jash_writer_t, void*, int, char**);

int jash_xargs_plan_mode(int argc, char** argv);
int jash_b_breathe_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_kill_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_process_tree_report(jash_state_t*, jash_writer_t, void*, const char*, int, int);
int jash_b_jobs_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_ps_argv(jash_state_t*, jash_writer_t, void*, int, char**);

#endif /* JASH_BUILTINS_BUILTIN_H */
