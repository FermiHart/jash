// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 The author makes music. Hear F E R M I ∞ H A R T wherever you stream.
//    No obligation. Just a wish.
//
// Module: core/state.h — central mutable state of one jash instance
//
// (FHCL-1.0 — F E R M I ∞ H A R T Creativity License — pending draft;
//  Unlicense placeholder until then.)

#ifndef JASH_CORE_STATE_H
#define JASH_CORE_STATE_H

#include <stdint.h>
#include <core/limits.h>
#include <stddef.h>
#include <core/frecency.h>
#include <core/predict.h>

/* Forward declarations */
struct jash_ss_engine;

/* Caps shared across modules. Keep small; jash is single-user single-instance.
 * Env cap grew from 128 to 1024 so sourcing real profiles (zshrc + conda init
 * + nvm + asdf + 40+ inherited vars) doesn't overflow. The 2048 slots also
 * leave room for 384 positional parameters without sharing a silent ceiling.
 * Per-value ceiling dropped from 128KB to 8KB, while values under 128 bytes
 * stay inline and larger values spill to heap. */
#define JASH_MAX_ENV        2048
#define JASH_ENV_KEY_LEN      64
#define JASH_ENV_VAL_LEN     JASH_LINE_CAP_ENVVAL  /* JINV-030: deriva de core/limits.h — nao um numero solto */
/* BUG-095: positional parameters $1..$N. Was hardcoded 9 everywhere (single-
 * char env keys), making ${10}+ unrepresentable and capping $# at 9. Keys are
 * decimal strings ("1".."384") — identical bytes for 1-9, so storage is
 * backward compatible. shift already handled multi-digit keys. */
#define JASH_POS_MAX         384
#define JASH_HIST_RING       256
#define JASH_HIST_LINE_LEN    512
#define JASH_CWD_MAX         512
#define JASH_YANK_MAX       1024
#define JASH_ALIAS_MAX        64
#define JASH_ALIAS_KEY_LEN    32
#define JASH_ALIAS_VAL_LEN   256

#define JASH_ABBR_GLOBAL_MAX  64
#define JASH_ABBR_LOCAL_MAX   32
#define JASH_ABBR_KEY_LEN     64
#define JASH_ABBR_VAL_LEN    256

/* PATH cache for fast command lookups */
#define JASH_PATH_CACHE_SIZE 128
#define JASH_PATH_KEY_LEN     64
#define JASH_PATH_VAL_LEN    256

typedef struct {
    char cmd[JASH_PATH_KEY_LEN];
    char path[JASH_PATH_VAL_LEN];
    uint32_t hits;
    int neg;           /* 1 = negative cache entry (command not found) */
    uint32_t neg_time; /* unix timestamp when negative entry was cached */
    uint32_t path_h;   /* fnv32 of $PATH the entry was resolved under —
                        * an entry is only valid while $PATH is unchanged
                         * (stale hits execute the wrong binary; stale
                         * negatives hide newly-reachable commands). */
    uint16_t disk_slot;  /* fixed record in path_cache.dat */
    uint8_t persisted;   /* ordinary negative entries stay memory-only */
} jash_path_cache_entry_t;

/* Full PATH executable list for did_you_mean (lazy-loaded) */
#define JASH_PATH_LIST_MAX  256
#define JASH_PATH_LIST_NAME_LEN 64

typedef struct {
    char name[JASH_PATH_LIST_NAME_LEN];
} jash_path_list_entry_t;

/* Inline buffer size for env values. Values that fit (95%+ of real env vars)
 * use zero-alloc inline storage; longer values (e.g. PATH with conda+nvm)
 * spill to a heap buffer. JASH_ENV_VAL_LEN is kept for local stack buffers
 * elsewhere in the parser that need to hold a full-size value temporarily. */
#define JASH_ENV_VAL_INLINE 128

/* One environment entry. Short values (< 128 bytes) live in inline_val with
 * heap_val == NULL. Long values malloc into heap_val; inline_val is unused.
 * Saves ~4.1 MB vs the prior char val[8192] × 512 layout. */
typedef struct {
    char     key[JASH_ENV_KEY_LEN];           /* 64 bytes */
    char     inline_val[JASH_ENV_VAL_INLINE]; /* 128 bytes — fast path */
    char*    heap_val;                         /* NULL = use inline_val */
    uint32_t hash;
    int32_t  array_next;                       /* next member in base-hash chain */
} jash_env_entry_t;

/* Read the value of an env entry regardless of where it lives. */
#define JASH_ENV_VAL(e) ((e)->heap_val ? (e)->heap_val : (e)->inline_val)

/* One native call frame (perf W12). `name` is the pushed function name
 * (owned). sh_* snapshot the table slots a sync may clobber. */
typedef struct {
    char* sh_name;
    int sh_name_present;
    char** sh_idx;
    unsigned sh_mask;
    int sh_n;
} jash_frame_t;

/* Hash table buckets for O(1) env lookup. Index = hash % JASH_ENV_HASH_SIZE.
 * Must be ≥ 2× JASH_MAX_ENV to keep load factor < 0.5 (linear probing). With
 * 64 buckets and 128+ entries the table was silently overflowing — inserts
 * after the 64th key were dropped from the index and looked up as "missing"
 * even though `export -p` saw them in the entry array. */
#define JASH_ENV_HASH_SIZE 4096
/* _Static_assert porque o bear nao avalia `<` entre macros no #if. */
_Static_assert(JASH_ENV_HASH_SIZE >= 2 * JASH_MAX_ENV,
    "JASH_ENV_HASH_SIZE must be at least twice JASH_MAX_ENV");

/* Array elements are flat env keys (`base[subscript]`). This parallel hash
 * links only those entries, so aggregate expansion walks array members rather
 * than every scalar environment variable. */
#define JASH_ENV_ARRAY_HASH_SIZE JASH_ENV_HASH_SIZE

/* History ring buffer. Lines older than HEAD-RING are silently dropped. */
typedef struct {
    char lines[JASH_HIST_RING][JASH_HIST_LINE_LEN];
    uint32_t head;     /* next slot to write */
    uint32_t count;    /* how many slots filled (≤ RING) */
} jash_history_t;

/* Alias table. Lexical scope — refilled when entering a function. */
typedef struct {
    char key[JASH_ALIAS_KEY_LEN];
    char val[JASH_ALIAS_VAL_LEN];
} jash_alias_entry_t;

/* Abbreviation table entry (separate from env for local/global scoping). */
typedef struct {
    char key[JASH_ABBR_KEY_LEN];
    char val[JASH_ABBR_VAL_LEN];
} jash_abbr_entry_t;

/* Structured exit status — replaces classic int $?.
 * code follows POSIX (0=ok, 127=not-found, etc) but the surrounding
 * fields give AI / users richer context. */
typedef struct {
    int      code;
    char     msg[128];
    char     suggestion[128];   /* filled by AI on demand */
    char     cmd[256];          /* the command that produced this status */
    uint64_t time_ns;
} jash_status_t;

/* Shell options/flags — set -e/-u/-x/-v/-o pipefail, etc. */
typedef struct {
    int errexit;    /* -e: exit on error */
    int nounset;    /* -u: error on undefined variables */
    int xtrace;     /* -x: trace commands before execution */
    int verbose;    /* -v: print input lines */
    int pipefail;   /* -o pipefail: pipe fails if any command fails */
    int posix;      /* -o posix: strict POSIX mode */
    int emacs;      /* -o emacs: use emacs-style line editing */
    int vi;         /* -o vi: use vi-style line editing */
    int errtrace;   /* -o errtrace: ERR trap propagates to functions */
    int noglob;     /* -f: disable pathname (glob) expansion */
    int lastpipe;   /* shopt lastpipe: run last pipeline segment in parent */
    int noclobber;  /* -C / -o noclobber: > fails if file exists */
    /* BUG-167: shopt glob options — were stored in shopt.c's table but never
     * read anywhere (every glob site hardcoded "off" in a comment as if it
     * were a constant). Synced from shopt.c the same way lastpipe already is. */
    int nullglob;   /* shopt nullglob: unmatched glob expands to nothing */
    int failglob;   /* shopt failglob: unmatched glob is an expansion error */
    int nocaseglob; /* shopt nocaseglob: case-insensitive glob matching */
    int monitor;    /* -m / -o monitor: job-control monitor mode.
                     * No-op under -c (no interactive job control); stored so
                     * `set +m`/`set -m` from agent harnesses is silently
                     * accepted instead of erroring. bash-compatible. */
    /* JINV-067 — as letras POSIX/bash que faltavam. Antes destas, `jash -a -c
     * 'echo OK'` (e -b -h -k -p -t) caía no `break` que engolia flag curta
     * desconhecida, e o comando inteiro sumia com rc 0. Todas são lidas por
     * `set -o` (print_options em builtin_set.c), então nenhuma é campo morto;
     * o que cada uma faz — ou por que é inerte neste shell — está documentado
     * ao lado da tabela, em builtin_set.c. */
    int allexport;  /* -a: exportar toda variável atribuída */
    int notify;     /* -b: reportar fim de job assíncrono na hora */
    int hashall;    /* -h: lembrar o caminho de comandos já resolvidos */
    int keyword;    /* -k: atribuições em qualquer posição vão para o ambiente */
    int privileged; /* -p: modo privilegiado */
    int onecmd;     /* -t: sair depois de ler e executar um comando */
} jash_shell_opts_t;

/* The one true state. Passed by pointer to every function that needs
 * mutable state. There are NO globals in jash. */
typedef struct jash_state {
    /* CWD */
    char cwd[JASH_CWD_MAX];

    /* Environment */
    jash_env_entry_t env[JASH_MAX_ENV];
    uint32_t         env_count;
    uint32_t         env_reserved_slots; /* caller positionals reserved across functions */
    int32_t          env_hash[JASH_ENV_HASH_SIZE];  /* bucket → index in env[], -1 = empty */
    int32_t          env_array_heads[JASH_ENV_ARRAY_HASH_SIZE];

    /* Alias table */
    jash_alias_entry_t aliases[JASH_ALIAS_MAX];
    uint32_t           alias_count;

    /* Abbreviation tables (global = ~/.jashrc, local = ./.jashrc) */
    jash_abbr_entry_t  abbr_global[JASH_ABBR_GLOBAL_MAX];
    uint32_t           abbr_global_count;
    jash_abbr_entry_t  abbr_local[JASH_ABBR_LOCAL_MAX];
    uint32_t           abbr_local_count;

    /* Yank buffer (clipboard) */
    char     yank[JASH_YANK_MAX];
    uint32_t yank_len;

    /* Last exit status (structured) */
    jash_status_t last_status;

    /* Locale (i18n) — index into i18n string tables. */
    uint32_t locale;

    /* PID (Josh: real PID via getpid; standalone: host PID). */
    long pid;

    /* TTY index (Josh multi-TTY support; standalone always 0). */
    int tty;

    /* Interactive vs script mode. */
    int interactive;

    /* Set by the `exit` builtin to ask the REPL to break out of its loop.
     * (Distinct from `interactive` — that one is the mode flag, set at startup
     * and never cleared by the user. Recycling it here used to make REPLs
     * "stuck" because the loop ignored interactive-flips mid-run.) */
    int exit_requested;

    /* Agent runtime control plane. The normal JSONL request stream is
     * synchronous, so cancellation arrives on a separately inherited socket. */
    int agent_cancel_fd;
    char agent_active_request_id[48];
    char agent_cancel_buf[256];
    unsigned agent_cancel_len;
    int agent_cancel_discarding;
    int agent_request_cancelled;
    int agent_request_timed_out;
    int agent_request_kill_incomplete;

    /* BUG-012: `return` control flow. The return builtin sets return_requested
     * + return_code; the command-list executor stops the current list and the
     * function executor (jash_eval_function_with_redir) consumes/clears it at
     * the function boundary, mirroring exit_requested. */
    int return_requested;
    int return_code;

    /* BUG-166: `break N` / `continue N` multi-level control flow. The break/
     * continue builtins parse N (default 1) into loop_break_level and return
     * their sentinel exit code (255/254 respectively, unchanged). Each loop
     * site (for_loop.c, while_loop.c) decrements this and re-breaks while
     * >1, so it propagates through as many enclosing loop constructs as
     * requested; the level-1 (target) loop is the one that performs the
     * actual break/continue action. Deliberately NOT reset at function-call
     * boundaries — verified against real bash that break/continue inside a
     * function called from a loop DOES break/continue that enclosing loop
     * (dynamic scope), so `return_requested`'s function-boundary-consumption
     * pattern must NOT be mirrored here. */
    int loop_break_level;

    /* JINV-073 — o CANAL DE CONTROLE, separado do canal de status.
     *
     * O defeito que este campo mata: 255 e 254 eram usados COMO codigo de
     * saida para dizer "break" e "continue". So que 0..255 e a faixa inteira
     * e LEGITIMA de um processo — `ssh` devolve 255 quando a conexao falha,
     * `xargs` devolve 255 quando manda parar, e `exit 255` e valido. Um
     * script do usuario que saisse com 255 era lido pelo shell como um
     * pedido de break: o resto da lista de comandos era ABANDONADO.
     *
     *     jash -c '(exit 255); echo sobrevivi'   ->  nada, rc 255
     *     jash -c 'sh -c "exit 255"; echo x'     ->  nada, rc 255
     *     jash -c 'f(){ return 255; }; f; echo x'->  nada, rc 255
     *
     * Dado (o status de um processo) estava virando comando (uma instrucao de
     * fluxo) — a mesma familia dos JINV-016..019. A separacao correta e a
     * unica que resolve: quem pede break/continue marca AQUI, fora da banda
     * de status; quem consome exige as DUAS coisas (o rc sentinela E a marca).
     * Um `exit 255` de verdade nunca marca, logo nunca e confundido.
     *
     * Ciclo de vida: posto por `break`/`continue` (builtin_break.c,
     * builtin_continue.c); baixado pelo laco-alvo junto com loop_break_level
     * (nivel <= 1), e por seguranca no comeco de cada linha de nivel de topo
     * (eval.c) — ali nao existe laco em voo, entao qualquer marca sobrevivente
     * e lixo de um `break` fora de laco.
     *
     * O rc sentinela CONTINUA sendo devolvido: os consumidores que ainda nao
     * migraram (script.c, if_block.c) seguem funcionando exatamente como
     * antes. A marca so ACRESCENTA precisao — nunca remove capacidade. */
    int loop_ctl;

    /* BUG-013: `command NAME` sets this so the next command resolution skips
     * the function table (calls the builtin/external even if a function shadows
     * it). One-shot: consumed/cleared at the resolution point. */
    int skip_function_lookup;

    /* Native call-frame stack (perf W12): function depth + FUNCNAME live
     * here instead of ~15 env round-trips per call. Table reads of these
     * keys prefer native state while depth>0 (env.c frame hooks);
     * jash_env_count() materializes native state first, so every
     * whole-table reader (`declare`, `env`, export -p, execve envp
     * builders) sees exactly what the table version showed. Positionals
     * and `local` scope stay table-managed. */
#define JASH_FRAME_MAX 32
    int func_depth;
    char func_depth_str[16];
    char* funcname_stack[JASH_FRAME_MAX];
    int frame_dirty;
    /* Per-frame snapshot of the table slots a sync may clobber (taken at
     * enter, restored at exit). Indexed shadow storage is malloc'd only
     * when a shadowed slot actually exists (nearly never). */
    jash_frame_t frame_stack[JASH_FRAME_MAX];

    /* Reload state — set by --resume=<path>; main.c re-hydrates. */
    const char* resume_path;

    /* Job control table (for background jobs) */
    struct jash_job_table* jobs;

    /* Job control runtime state (POSIX terminal/process-group control).
     * Only engaged when interactive AND stdin is a real tty. */
    int  job_control_active;  /* 1 = setpgid/tcsetpgrp machinery is live */
    int  shell_pgid;          /* the shell's own process group id        */
    int  shell_tty_fd;        /* fd of the controlling terminal (stdin)  */

    /* Function table (for user-defined functions) */
    struct jash_function_table* functions;

    /* Coprocess table */
    struct coproc_table_s* coprocs;

    /* Shell options (set -e/-u/-x/-v/-o pipefail, etc) */
    jash_shell_opts_t shell_opts;

    /* Frecency directory table */
    frecency_table_t frecency;

    /* Smart command prediction table */
    predict_table_t predictions;

    /* PATH cache for fast command lookups.
     * Loaded lazily on the first external lookup (see jash_path_lookup):
     * a `-c` run that only uses builtins never touches path_cache.dat. */
    jash_path_cache_entry_t path_cache[JASH_PATH_CACHE_SIZE];
    int path_cache_loaded;
    uint32_t path_cache_count;
    uint32_t path_cache_disk_count;
    uint8_t path_cache_dirty[JASH_PATH_CACHE_SIZE];
    uint8_t path_cache_meta_dirty;
    
    /* Full PATH executable list for did_you_mean (lazy-loaded on first use) */
    jash_path_list_entry_t path_list[JASH_PATH_LIST_MAX];
    uint32_t path_list_count;
    int path_list_loaded;  /* 1 = populated, 0 = not yet */
    
    /* Variable expansion cache for performance optimization */
    void* var_cache;  /* jash_var_cache_t* - forward declared to avoid circular deps */
    /* ADR-0036 W5: bumped by jash_env_set/unset/append. A cache entry is only
     * valid if it was stamped with the generation that was current at store
     * time — any env mutation invalidates the whole cache in one comparison,
     * no per-entry scanning/TTL needed for correctness. */
    uint64_t env_generation;

    /* Smart Suggestions engine for next-command prediction */
    struct jash_ss_engine* ss_engine;
} jash_state_t;

/* JINV-073 — o vocabulario do canal de controle.
 *
 * `JASH_RC_BREAK`/`JASH_RC_CONTINUE` continuam sendo os codigos que os
 * builtins devolvem (compatibilidade com os consumidores nao migrados), mas
 * NENHUM consumidor migrado pode olhar so para eles: a pergunta certa e
 * `jash_loopctl_is_break(s, rc)`, que exige o rc E a marca fora de banda.
 *
 * Predicados em vez de comparacoes soltas porque a comparacao solta foi
 * exatamente o defeito: espalhada por sete arquivos, ninguem podia
 * consertar num lugar so. Agora ha uma porta. */
#define JASH_LOOPCTL_NONE      0
#define JASH_LOOPCTL_BREAK     1
#define JASH_LOOPCTL_CONTINUE  2

#define JASH_RC_BREAK        255
#define JASH_RC_CONTINUE     254

/* Um `break` pedido de verdade (rc sentinela + marca). */
static inline int jash_loopctl_is_break(const jash_state_t* s, int rc) {
    return rc == JASH_RC_BREAK && s && s->loop_ctl == JASH_LOOPCTL_BREAK;
}
/* Um `continue` pedido de verdade. */
static inline int jash_loopctl_is_continue(const jash_state_t* s, int rc) {
    return rc == JASH_RC_CONTINUE && s && s->loop_ctl == JASH_LOOPCTL_CONTINUE;
}
/* Qualquer um dos dois — o teste que substitui `rc == 255 || rc == 254`. */
static inline int jash_loopctl_is_ctl(const jash_state_t* s, int rc) {
    return jash_loopctl_is_break(s, rc) || jash_loopctl_is_continue(s, rc);
}
/* Ha um pedido de fluxo em voo? (guarda de cauda: o texto depois de
 * `done`/`esac`/`fi` NAO pode rodar quando um break/continue esta subindo.) */
static inline int jash_loopctl_pending(const jash_state_t* s) {
    return s && s->loop_ctl != JASH_LOOPCTL_NONE;
}
/* Consumido: o laco-alvo agiu, o pedido morreu aqui. */
static inline void jash_loopctl_clear(jash_state_t* s) {
    if (s) s->loop_ctl = JASH_LOOPCTL_NONE;
}

/* ── Function prototypes ───────────────────────────────────────────────────────── */

/* Lifecycle. */
void jash_state_init(jash_state_t* s);
void jash_state_cleanup(jash_state_t* s);
int  jash_state_serialize(const jash_state_t* s, const char* path);
int  jash_state_deserialize(jash_state_t* s, const char* path);

/* Variable cache functions */
void jash_var_cache_init(jash_state_t* s);
const char* jash_var_cache_lookup(jash_state_t* s, const char* original);
void jash_var_cache_store(jash_state_t* s, const char* original, const char* expanded);
void jash_var_cache_invalidate(jash_state_t* s);

#endif /* JASH_CORE_STATE_H */
