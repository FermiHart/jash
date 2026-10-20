// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/eval — dispatch (impl).

#include "eval.h"
#include "exec.h"
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>
#endif
#include <module/sigil.h>
#include <core/timeline.h>
#ifndef JASH_AGENT_ONLY
#include <core/jash_ai.h>
#include <liquid/jash_liquid.h>
#endif
#include "pipeline.h"
#include "script.h"
#include "conditional.h"
#include <core/alias.h>
#include <core/guard.h>
#include <core/memo.h>
#include <core/history.h>
#include <core/nudge.h>
#include <core/hints.h>
#include <core/loopguard.h>
#include <core/limits.h>          /* JINV-030/063: teto canonico de linha */
#include <core/predict.h>
#include <builtins/builtin.h>
#include <theme/colors.h>

extern int jash_readonly_is_set(const char* name);
#include <core/env.h>
#include "param_expand.h"
#include "builtin_redir.h"
#include "arithmetic.h"
#include "cmdsub.h"
#include "expand.h"
#include "quotes.h"
#include "coproc.h"
#include "heredoc.h"
#include <function_defs.h>
#include <core/output_budget.h>   /* ADR-0044: budget-govern builtin output */
#include <unistd.h>               /* isatty */
#include <string.h>
#include <sys/syscalls.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <job_control.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

static void eval_free_positionals(char** values) {
    if (!values) return;
    for (int i = 0; i < JASH_POS_MAX; i++) free(values[i]);
    free(values);
}

static char** eval_snapshot_positionals(jash_state_t* s, uint32_t* out_count) {
    char** saved = (char**)calloc(JASH_POS_MAX, sizeof(char*));
    if (!saved) return NULL;
    *out_count = 0;
    /* Scan once: unset N can leave holes, and only canonical keys 1..N
     * belong to the positional frame (not 0, 01, or out-of-range keys).
     * Field access (not jash_env_count()): only table-managed positional
     * keys matter here, never native frame state. */
    for (uint32_t i = 0; i < s->env_count; i++) {
        const char* key = NULL;
        const char* old = NULL;
        if (!jash_env_at(s, i, &key, &old) || key[0] < '1' || key[0] > '9')
            continue;
        int index = 0;
        const char* p = key;
        for (; *p >= '0' && *p <= '9'; p++) {
            index = index * 10 + (*p - '0');
            if (index > JASH_POS_MAX) break;
        }
        if (*p || index < 1 || index > JASH_POS_MAX) continue;
        saved[index - 1] = strdup(old);
        if (!saved[index - 1]) {
            eval_free_positionals(saved);
            return NULL;
        }
        (*out_count)++;
    }
    return saved;
}

static int eval_restore_positionals(jash_state_t* s, char** saved) {
    if (!saved) return -1;
    int count = 0;
    while (count < JASH_POS_MAX && saved[count]) count++;
    int rc = jash_env_replace_positionals_owned(s, saved, count);
    /* The owned API requires a dense prefix. Restore entries beyond its
     * first gap separately, without creating empty keys for the holes. */
    if (rc == 0) {
        for (int i = count + 1; i < JASH_POS_MAX; i++) {
            if (!saved[i]) continue;
            char key[16]; snprintf(key, sizeof key, "%d", i + 1);
            if (jash_env_set(s, key, saved[i]) != 0) rc = -1;
        }
    }
    if (rc != 0)
        jash_writes(jash_stderr_writer, 0,
                    "jash: unable to restore function positionals\n");
    eval_free_positionals(saved);
    return rc;
}

int jash_parse_function_positionals(const char* args, char** values,
                                    int* out_count) {
    const char* p = args ? args : "";
    int count = 0;
    if (!values || !out_count) return -1;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char* start = p;
        char quote = 0;
        while (*p) {
            char c = *p;
            if (quote) {
                if (c == quote) { quote = 0; p++; continue; }
                if (c == '\\' && quote == '"' && p[1]) { p += 2; continue; }
                p++;
                continue;
            }
            if (c == ' ' || c == '\t') break;
            if (c == '\'' || c == '"') { quote = c; p++; continue; }
            if (c == '\\' && p[1]) { p += 2; continue; }
            p++;
        }

        size_t raw_len = (size_t)(p - start);
        char* raw = (char*)malloc(raw_len + 1);
        char* unquoted = NULL;
        if (!raw) goto oom;
        memcpy(raw, start, raw_len); raw[raw_len] = 0;
        char* store = raw;
        if (jash_has_quotes(raw) || strchr(raw, '\\')) {
            unquoted = (char*)malloc(raw_len + 1);
            if (!unquoted) { free(raw); goto oom; }
            if (jash_unquote(raw, unquoted, raw_len + 1) >= 0) store = unquoted;
        }
        jash_quote_unshield(store);

        const char* field = store;
        for (;;) {
            const char* boundary = strchr(field, '\x1E');
            if (count >= JASH_POS_MAX) {
                free(raw); free(unquoted);
                goto too_many;
            }
            if (!boundary && field == store) {
                /* A single field can own the selected token buffer directly. */
                values[count] = store;
                if (store == raw) raw = NULL;
                else unquoted = NULL;
            } else {
                size_t len = boundary ? (size_t)(boundary - field) : strlen(field);
                values[count] = (char*)malloc(len + 1);
                if (!values[count]) {
                    free(raw); free(unquoted);
                    goto oom;
                }
                memcpy(values[count], field, len); values[count][len] = 0;
            }
            count++;
            if (!boundary) break;
            field = boundary + 1;
        }
        free(raw); free(unquoted);
    }
    *out_count = count;
    return 0;

too_many:
    for (int i = 0; i < count; i++) { free(values[i]); values[i] = NULL; }
    return -2;
oom:
    for (int i = 0; i < count; i++) { free(values[i]); values[i] = NULL; }
    return -1;
}

typedef struct {
    jash_env_entry_t* entries;
    int32_t* hash;
    uint32_t count;
    uint32_t reserved;
    uint64_t generation;
} eval_env_snapshot_t;

static void eval_env_snapshot_free(eval_env_snapshot_t* snap) {
    if (!snap) return;
    for (uint32_t i = 0; i < snap->count; i++) free(snap->entries[i].heap_val);
    free(snap->entries); free(snap->hash);
    memset(snap, 0, sizeof *snap);
}

static int eval_env_snapshot(jash_state_t* s, eval_env_snapshot_t* snap) {
    snap->count = s->env_count;
    snap->reserved = s->env_reserved_slots;
    snap->generation = s->env_generation;
    snap->entries = (jash_env_entry_t*)calloc(snap->count ? snap->count : 1,
                                               sizeof(*snap->entries));
    snap->hash = (int32_t*)malloc(sizeof s->env_hash);
    if (!snap->entries || !snap->hash) goto fail;
    memcpy(snap->hash, s->env_hash, sizeof s->env_hash);
    for (uint32_t i = 0; i < snap->count; i++) {
        snap->entries[i] = s->env[i];
        snap->entries[i].heap_val = NULL;
        if (s->env[i].heap_val) {
            snap->entries[i].heap_val = strdup(s->env[i].heap_val);
            if (!snap->entries[i].heap_val) goto fail;
        }
    }
    return 0;
fail:
    eval_env_snapshot_free(snap);
    return -1;
}

static void eval_array_remove(jash_state_t* s, const char* name) {
    size_t name_len = strlen(name);
    for (;;) {
        int removed = 0;
        uint32_t n = jash_env_count(s);
        for (uint32_t i = 0; i < n; i++) {
            const char* key = NULL;
            if (!jash_env_at(s, i, &key, NULL)) continue;
            if (strlen(key) > name_len &&
                strncmp(key, name, name_len) == 0 && key[name_len] == '[') {
                char copy[JASH_ENV_KEY_LEN];
                snprintf(copy, sizeof copy, "%s", key);
                jash_env_unset(s, copy);
                removed = 1;
                break;
            }
        }
        if (!removed) break;
    }
}

static void eval_env_rollback(jash_state_t* s, eval_env_snapshot_t* snap) {
    uint64_t fresh_generation = s->env_generation + 1;
    for (uint32_t i = 0; i < s->env_count; i++) {
        (void)unsetenv(s->env[i].key);
        free(s->env[i].heap_val);
    }
    memset(s->env, 0, sizeof s->env);
    memcpy(s->env, snap->entries, snap->count * sizeof(*snap->entries));
    memcpy(s->env_hash, snap->hash, sizeof s->env_hash);
    s->env_count = snap->count;
    jash_env_array_index_rebuild(s);
    s->env_reserved_slots = snap->reserved;
    if (fresh_generation <= snap->generation)
        fresh_generation = snap->generation + 1;
    s->env_generation = fresh_generation;
    for (uint32_t i = 0; i < snap->count; i++) {
        snap->entries[i].heap_val = NULL;
        (void)setenv(s->env[i].key, JASH_ENV_VAL(&s->env[i]), 1);
    }
    eval_env_snapshot_free(snap);
}

/* ── v0.7: shell functions ─────────────────────────────────────────────
 * A process-local function table. jash is single-instance per process,
 * so a file-static table is equivalent to hanging it off jash_state_t
 * but avoids touching the state lifecycle / serialization code. */


/* Detect a function definition at the start of `line`:
 *     name() { body }            (single line)
 *     name () { body }
 * Returns 1 and fills name/body if matched. body excludes the outer
 * braces and is trimmed. */
/* JINV-086 — o corpo NÃO pode ser cortado aqui.
 *
 * Este é o PRIMEIRO dos dois tetos em série do caminho de função (o outro é o
 * armazenamento, em `function_defs.h`). O corte morava em
 *
 *     if (blen >= bcap) blen = bcap - 1;
 *
 * e era mudo: a função era registrada decapitada, `rc 0`, stderr vazio, e a
 * chamada seguinte executava um programa que ninguém escreveu.
 *
 * `body`/`bcap` continuam sendo o caminho rápido de pilha (zero `malloc` no
 * caso comum). Acima de `bcap` o corpo vai para `*out_body_heap`, do tamanho
 * EXATO, e é do CHAMADOR a obrigação de liberar. Sem memória devolve -1 —
 * recusa alta, nunca um corpo cortado com cara de sucesso.
 *
 * Devolve: 0 = não é definição de função · 1 = casou · -1 = casou mas faltou
 * memória (o chamador RECUSA).
 *
 * Gate: tests/test_jinv086_silent_caps.sh */
/* JINV/BUG-066: fd de salvamento ALOCADO, nunca base fixa.
 *
 * Era `sys_dup2(tgt, 50 + nsaved)`. Com base fixa, dois niveis de redirect
 * aninhados escolhem os MESMOS fds e o de dentro destroi a copia do de fora.
 * Medido por strace em 2026-08-05, no caso
 *     source arquivo >/dev/null 2>&1      (arquivo contem `cmd 2>/dev/null`)
 *
 *     dup2(1, 50)   externo salva stdout em 50
 *     dup2(3, 1)    stdout -> /dev/null
 *     dup2(2, 50)   INTERNO salva stderr em 50  <- destroi a copia do stdout
 *     dup2(50, 2)   interno restaura
 *     (nunca acontece dup2(50, 1))
 *
 * stdout ficava em /dev/null pelo resto da sessao. E o idioma que TODO harness
 * de agente usa para abrir um comando, entao o efeito era saida VAZIA com rc 0
 * — o comando nunca rodava e o vazio lia como sucesso.
 *
 * F_DUPFD_CLOEXEC devolve o menor fd livre >= 10: dois niveis nunca recebem o
 * mesmo numero, e a colisao fica impossivel por construcao — a mesma disciplina
 * que redir_save_targets() em parser/pipeline.c ja adotara. F E R M I ∞ H A R T */
static int parse_function_def(const char* line, char* name, unsigned ncap,
                              char* body, unsigned bcap, const char** out_rest,
                              char** out_body_heap) {
    const char* p = line;
    if (out_body_heap) *out_body_heap = NULL;   /* JINV-086 */
    while (*p == ' ' || *p == '\t') p++;
    
    /* Check for 'function' keyword */
    int has_function_keyword = 0;
    if (strncmp(p, "function", 8) == 0 && (p[8] == ' ' || p[8] == '\t')) {
        has_function_keyword = 1;
        p += 8;
        while (*p == ' ' || *p == '\t') p++;
    }
    
    /* identifier — bash accepts function names beyond the POSIX identifier:
     * `cmake-build() { …; }`, `npm.test() { …; }` are valid there (the name
     * is a WORD, not an identifier). Without this, rc files with kebab-case
     * helpers exploded at startup with `name(): command not found`. First
     * char stays alpha/underscore; continuation gains `-`, `.`, `+`. */
    const char* nstart = p;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_'))
        return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
           *p == '.' || *p == '+') p++;
    unsigned nlen = (unsigned)(p - nstart);
    if (nlen == 0 || nlen >= ncap) return 0;
    while (*p == ' ' || *p == '\t') p++;
    
    /* If 'function' keyword was used, () is optional. BUG-083: bash allows
     * blanks AND newlines between `()` and `{` (`f()⏎{ … }`), so skip \n/\r
     * here too — the multi-line reader accumulates the whole block. */
    if (!has_function_keyword) {
        if (p[0] != '(' || p[1] != ')') return 0;
        p += 2;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    } else {
        /* With 'function' keyword, () is optional */
        if (p[0] == '(' && p[1] == ')') {
            p += 2;
        }
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    }

    if (*p != '{') return 0;
    p++; /* skip { */
    /* find matching } honoring nesting */
    int depth = 1;
    const char* bstart = p;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) break; }
        p++;
    }
    if (depth != 0) return 0; /* unterminated */
    const char* bend = p;    /* points at closing } */
    p++; /* skip past closing } */
    /* trim body whitespace + leading/trailing ; */
    while (bstart < bend && (*bstart == ' ' || *bstart == '\t' ||
                             *bstart == ';' || *bstart == '\n')) bstart++;
    while (bend > bstart && (bend[-1] == ' ' || bend[-1] == '\t' ||
                             bend[-1] == ';' || bend[-1] == '\n')) bend--;
    unsigned blen = (unsigned)(bend - bstart);
    /* JINV-086: aqui morava `if (blen >= bcap) blen = bcap - 1;` — o corte
     * mudo. Agora o corpo grande migra para o heap, do tamanho exato. */
    char* dst = body;
    if (blen >= bcap) {
        if (!out_body_heap) return -1;
        char* h = (char*)malloc((size_t)blen + 1);
        if (!h) return -1;              /* recusa alta, nunca truncamento */
        *out_body_heap = h;
        dst = h;
    }
    for (unsigned i = 0; i < nlen; i++) name[i] = nstart[i];
    name[nlen] = 0;
    for (unsigned i = 0; i < blen; i++) dst[i] = bstart[i];
    dst[blen] = 0;
    if (out_rest) *out_rest = p;
    return 1;
}

static void copy_to(char* dst, unsigned cap, const char* src, unsigned n) {
    if (cap == 0) return;
    if (n > cap - 1) n = cap - 1;
    for (unsigned i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

/* Skip whitespace at front. */
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Split first token. Returns pointer to args start (or empty string).
 * JINV-019: a extração é quote-aware (parser/quotes.c) — esta função cortava
 * no primeiro espaço, truncando `'/opt/my tool/run'` em `'/opt/my`. */
static const char* split_first(const char* line, char* name, unsigned cap) {
    return jash_first_word(line, name, cap, 0, (int*)0);
}

static int is_directory_path(const char* path) {
    struct stat st;
    if (!path || !*path) return 0;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Check if a token looks like VAR=value (no spaces around =).
 * Returns pointer to '=' if it matches, NULL otherwise. */
/* Helper for assignment parsing — currently unused after eval_assignment
 * removal, but retained for future assignment-refactor (AST parser). */
static const char* find_assignment_eq(const char* token) __attribute__((unused));
static const char* find_assignment_eq(const char* token) {
    if (!token || !*token) return NULL;
    /* Valid variable name chars: alphanumeric + underscore, must start with
     * letter or underscore. */
    const char* p = token;
    if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && *p != '_') {
        return NULL;  /* not a valid var name start */
    }
    while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
        p++;
    }
    if (*p == '=') return p;  /* found the = */
    return NULL;
}




/* Scan for top-level command separator: ;, &, &&, || (skipping inside
 * single/double quotes, $(), `, ${}, $((..)) and parenthesized groups).
 * Returns: pointer to the separator inside `line`, or NULL if none.
 * Sets *out_kind to one of:
 *   0 = ';'   (sequence)
 *   1 = '&&'  (run next only if rc == 0)
 *   2 = '||'  (run next only if rc != 0)
 *   3 = '&'   (run current segment in background, next always runs)
 * *out_skip is how many chars to skip past the separator. */
/* Does `p` begin a redirect operator? (>, >>, <, &>, N>, N<, N>&M, 2>&1, …)
 * Used to route `... done >file` / `done 2>&1` through the pipeline engine,
 * which applies the redirect to the whole compound. */
static int starts_with_redir(const char* p) {
    if (!p) return 0;
    if (*p == '>' || *p == '<') return 1;
    if (*p == '&' && p[1] == '>') return 1;
    if (*p >= '0' && *p <= '9') {
        const char* q = p;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '>' || *q == '<') return 1;
    }
    return 0;
}

/* JINV-083 — o laco inteiro esta canalizado/redirecionado?
 *
 * As tres copias deste teste (for/while/until, logo abaixo) procuravam o
 * fechador com `strstr(line, "done")` — busca por SUBSTRING, sem fronteira de
 * palavra e sem nocao de aspas. Um `done` que era DADO decidia como o
 * enunciado inteiro seria executado:
 *
 *     for i in 1; do echo "all done | x"; done
 *       bash -> all done | x        jash -> (nada)
 *
 * O `done |` citado fazia o laco inteiro ser entregue ao motor de pipeline.
 * Uma leitura so — `jash_find_keyword` (parser/script.h) — e uma copia so
 * deste predicado. */
static int jash_loop_is_piped(const char* line) {
    /* `done` so em POSIÇÃO DE COMANDO (JINV-083): `echo done | wc` no corpo do
     * laco nao pode decidir que o LAÇO INTEIRO e um pipeline. */
    static const jash_kw_t done_kw[] = { { "done", JASH_KW_END_ANY, 1 } };
    const char* after = 0;
    const char* hit = jash_find_keyword_set(line, done_kw, 1, 0, &after);
    while (hit) {
        const char* tail = after;
        while (*tail == ' ' || *tail == '\t') tail++;
        if ((*tail == '|' && tail[1] != '|') || starts_with_redir(tail)) return 1;
        const char* next_after = 0;
        hit = jash_find_keyword_set(after, done_kw, 1, 0, &next_after);
        after = next_after;
    }
    return 0;
}

/* first_nl is the first physical newline in this immutable suffix, or NULL.
 * List walkers can reuse that fact until they advance past the newline. */
static const char* find_top_separator_impl(const char* line, const char* first_nl,
                                           int* out_kind, int* out_skip) {
    if (first_nl) {
        const char* hdop = NULL;
        for (const char* hp = line; hp + 1 < first_nl; hp++) {
            if (hp[0] == '<' && hp[1] == '<' && hp[2] != '<' && hp[2] != '(') {
                hdop = hp;
                break;
            }
        }
        if (hdop) {
            const char* dp = hdop + 2;
            if (*dp == '-') dp++;
            while (*dp == ' ' || *dp == '\t') dp++;
            char delim[128];
            unsigned di = 0;
            while (dp < first_nl && *dp != ' ' && *dp != '\t' && di + 1 < sizeof(delim)) {
                delim[di++] = *dp++;
            }
            delim[di] = '\0';
            if (di > 0) {
                const char* q = first_nl + 1;
                while (*q) {
                    const char* ls = q;
                    while (*q && *q != '\n') q++;
                    if ((unsigned)(q - ls) == di && strncmp(ls, delim, di) == 0) {
                        if (*q == '\n') { *out_kind = 0; *out_skip = 1; return q; }
                        break;
                    }
                    if (*q == '\n') q++;
                }
            }
        }
    }

    int sq = 0, dq = 0;
    /* JINV-020: pilha de aspas-duplas por nivel de parenteses.
     *
     * POSIX 2.2.3: dentro de $( ) a citacao RECOMECA do zero. Sem esta
     * pilha, o ramo `if (dq)` abaixo via a segunda aspa (a que ABRE dentro
     * do $()) como a que FECHA a de fora — paridade invertida — e o ';'
     * seguinte caia FORA de aspas e virava separador de comando:
     *
     *   echo "[$(echo "a;b")]"   ->  jash executava `b)]` como 2o comando
     *
     * Texto virando comando: a mesma classe do JINV-016. Este mesmo padrao
     * de pilha ja existia CORRETO em quatro varreduras do repo
     * (cmdsub.c:702, main.c:850, eval.c:948, eval.c:2283) —
     * find_top_separator foi a instancia que ficou de fora daquela rodada.
     * O debito das 9 copias esta anotado no dossie da auditoria. */
    char dqsave[64] = {0};
    int back_from_dq = 0;   /* JINV-020: crase aberta de dentro de " " */
    int paren_depth = 0;     /* tracks ( ) and $(  */
    int brace_depth = 0;     /* tracks { } and ${ */
    int bracket_depth = 0;   /* tracks [[ ]] */
    int back = 0;            /* backtick `cmd` */
    int block_depth = 0;     /* tracks for/while/if/do/then/done/fi blocks */
    const char* p = line;
    while (*p) {
        char c = *p;
        if (sq) {
            if (c == '\'') sq = 0;
            p++; continue;
        }
        if (dq) {
            if (c == '\\' && p[1]) { p += 2; continue; }
            /* JINV-020: $( e crase REINICIAM a citacao (POSIX 2.2.3) —
             * suspende o dq de fora e guarda-o para o ')' restaurar. */
            if (c == '$' && p[1] == '(' && p[2] != '(') {
                if (paren_depth < (int)sizeof(dqsave)) dqsave[paren_depth] = 1;
                paren_depth++; dq = 0; p += 2; continue;
            }
            if (c == '`') { back = 1; back_from_dq = 1; dq = 0; p++; continue; }
            if (c == '"') dq = 0;
            p++; continue;
        }
        if (back) {
            /* JINV-020: crase aberta de DENTRO de aspas duplas devolve o
             * dq suspenso ao fechar. */
            if (c == '`') {
                back = 0;
                if (back_from_dq) { back_from_dq = 0; dq = 1; }
            }
            p++; continue;
        }
        if (c == '\\' && p[1]) { p += 2; continue; }
        if (c == '\'') { sq = 1; p++; continue; }
        if (c == '"')  { dq = 1; p++; continue; }
        if (c == '`')  { back = 1; p++; continue; }
        /* BUG-121a: a #-comment (at a word boundary, unquoted) runs to the
         * newline — its content is INERT. Without skipping it, a `)` inside
         * `echo hi # note )` closed paren_depth and the next newline became a
         * spurious top-level separator, splitting a multi-line $() statement. */
        if (c == '#' && (p == line || p[-1] == ' ' || p[-1] == '\t' ||
                         p[-1] == '\n' || p[-1] == ';' || p[-1] == '&' ||
                         p[-1] == '|' || p[-1] == '(')) {
            while (*p && *p != '\n') p++;   /* newline handled by next iteration */
            continue;
        }
        if (c == '(' ) {
            /* Check for (( arithmetic */
            if (p[1] == '(') {
                /* Find matching )) */
                const char* q = p + 2;
                int arith_depth = 1;
                while (*q && arith_depth > 0) {
                    if (q[0] == '(' && q[1] == '(') { arith_depth++; q += 2; continue; }
                    if (q[0] == ')' && q[1] == ')') { arith_depth--; if (arith_depth == 0) { p = q + 1; goto next_char; } q += 2; continue; }
                    q++;
                }
                /* Unterminated (( — treat as normal parens */
                if (paren_depth < (int)sizeof(dqsave)) dqsave[paren_depth] = 0;
                paren_depth++; p++; continue;
            }
            /* JINV-020: parentese sem $ nao suspende citacao alguma —
             * zera o slot para a pilha ficar coerente. */
            if (paren_depth < (int)sizeof(dqsave)) dqsave[paren_depth] = 0;
            paren_depth++; p++; continue;
        }
        if (c == ')' ) {
            if (paren_depth > 0) {
                paren_depth--;
                /* JINV-020: fechou um $( aberto de dentro de aspas — devolve
                 * o dq suspenso, senao o resto da linha e lido como se
                 * estivesse fora de aspas. */
                if (paren_depth < (int)sizeof(dqsave) && dqsave[paren_depth]) {
                    dqsave[paren_depth] = 0; dq = 1;
                }
            }
            p++; continue;
        }
        if (c == '{' ) { brace_depth++; p++; continue; }
        if (c == '}' ) { if (brace_depth > 0) brace_depth--; p++; continue; }
        if (c == '[' && p[1] == '[') { bracket_depth++; p += 2; continue; }
        if (c == ']' && p[1] == ']') { if (bracket_depth > 0) bracket_depth--; p += 2; continue; }

        /* Detect block keywords: for/while/if increase depth, done/fi decrease */
        if (paren_depth == 0 && brace_depth == 0 && bracket_depth == 0) {
            int at_word_start = (p == line || isspace(p[-1]) || p[-1] == ';');
            /* JINV-083 — fronteira de palavra NAO basta: a palavra reservada
             * so e reservada em POSIÇÃO DE COMANDO (POSIX 2.9). Sem isto, um
             * `for`/`fi` em posicao de ARGUMENTO mexia na profundidade de
             * bloco e o `;` seguinte deixava de separar comandos, engolindo o
             * resto da linha:
             *
             *   echo for; echo cauda
             *     bash -> `for` + `cauda`   jash -> `for; echo cauda`
             *   if true; then echo fi; fi; echo cauda
             *     bash -> `fi` + `cauda`    jash -> syntax error: missing fi
             *
             * A regra e a mesma de `jash_find_keyword_set`, na forma que olha
             * para tras (parser/script.h) — esta varredura tem estado proprio
             * (aspas, crase, parenteses, chaves, `[[ ]]`) e nao pode adotar o
             * andar do outro arquivo. */
            if (at_word_start && !jash_at_cmd_pos(line, p)) at_word_start = 0;
            if (at_word_start) {
                /* Block-start keywords */
                /* BUG-018: `(` also starts these (C-style `for((`/`while((`),
                 * so they register as block keywords and the `;` before `do`
                 * isn't mistaken for a top-level separator. */
                int is_for = (strncmp(p, "for", 3) == 0 && (isspace(p[3]) || p[3] == ';' || p[3] == '('));
                int is_while = (strncmp(p, "while", 5) == 0 && (isspace(p[5]) || p[5] == ';' || p[5] == '('));
                int is_until = (strncmp(p, "until", 5) == 0 && (isspace(p[5]) || p[5] == ';' || p[5] == '('));
                int is_if = (strncmp(p, "if", 2) == 0 && (isspace(p[2]) || p[2] == ';'));
                int is_case = (strncmp(p, "case", 4) == 0 && (isspace(p[4]) || p[4] == ';'));
                if (is_for || is_while || is_until || is_if || is_case) {
                    block_depth++;
                }
                /* Block-end keywords */
                int is_done = (strncmp(p, "done", 4) == 0 && (isspace(p[4]) || p[4] == ';' || p[4] == '\0'));
                int is_fi = (strncmp(p, "fi", 2) == 0 && (isspace(p[2]) || p[2] == ';' || p[2] == '\0'));
                int is_esac = (strncmp(p, "esac", 4) == 0 && (isspace(p[4]) || p[4] == ';' || p[4] == '\0'));
                if (is_done || is_fi || is_esac) {
                    if (block_depth > 0) block_depth--;
                }
            }
        }

        /* Top-level only when all depths are zero AND not inside a block */
        if (paren_depth == 0 && brace_depth == 0 && bracket_depth == 0 && block_depth == 0) {
            /* Heredoc detection: if we see << at a valid position, skip over
             * the entire heredoc body so newlines within it are NOT treated
             * as command separators. This keeps `cat <<EOF\nbody\nEOF` as
             * a single logical command for the pipeline parser. F E R M I ∞ H A R T */
            if (c == '<' && p[1] == '<' && p[2] != '<' && p[2] != '(' &&
                !(p > line && p[-1] == '<')) {
                const char* hd = p + 2;
                if (*hd == '-') hd++; /* <<- */
                while (*hd == ' ' || *hd == '\t') hd++;
                /* Extract delimiter */
                char hd_delim[256];
                unsigned hdi = 0;
                int hd_quoted = 0;
                (void)hd_quoted;  /* Used for future: quoted delimiters suppress expansion */
                if (*hd == '\'' || *hd == '"') {
                    char hq = *hd++;
                    hd_quoted = 1;
                    while (*hd && *hd != hq && hdi < sizeof(hd_delim) - 1)
                        hd_delim[hdi++] = *hd++;
                    if (*hd == hq) hd++;
                } else {
                    while (*hd && *hd != ' ' && *hd != '\t' && *hd != '\n' && hdi < sizeof(hd_delim) - 1)
                        hd_delim[hdi++] = *hd++;
                }
                hd_delim[hdi] = '\0';
                if (hdi > 0) {
                    /* Skip to end of current line */
                    while (*hd && *hd != '\n') hd++;
                    if (*hd == '\n') hd++;
                    /* Scan body lines until delimiter found */
                    while (*hd) {
                        const char* line_check = hd;
                        /* Skip leading whitespace for <<- (indented) mode */
                        if (p[2] == '-' || (p[2] == '\'' || p[2] == '"')) {
                            /* For <<- the '-' is right after << */
                        }
                        /* Actually check: was it <<- ? */
                        int was_indented = (p[2] == '-');
                        if (was_indented) {
                            while (*line_check == ' ' || *line_check == '\t') line_check++;
                        }
                        if (strncmp(line_check, hd_delim, hdi) == 0 &&
                            (line_check[hdi] == '\n' || line_check[hdi] == '\0')) {
                            /* Found closing delimiter */
                            hd = line_check + hdi;
                            if (*hd == '\n') {
                                *out_kind = 0;
                                *out_skip = 1;
                                return hd;
                            }
                            break;
                        }
                        /* Skip this body line */
                        while (*hd && *hd != '\n') hd++;
                        if (*hd == '\n') hd++;
                    }
                    /* Set p to past the heredoc, then continue scanning */
                    p = hd;
                    continue;
                }
            }
            if (c == ';' || c == '\n') {
                if (c == '\n') {
                    const char* hdop = NULL;
                    for (const char* hp = line; hp < p; hp++) {
                        if (hp[0] == '<' && hp[1] == '<' && hp[2] != '<' && hp[2] != '(') hdop = hp;
                    }
                    if (hdop) {
                        const char* dp = hdop + 2;
                        if (*dp == '-') dp++;
                        while (*dp == ' ' || *dp == '\t') dp++;
                        char delim[128];
                        unsigned di = 0;
                        while (dp < p && *dp != ' ' && *dp != '\t' && *dp != '\n' && di + 1 < sizeof(delim)) {
                            delim[di++] = *dp++;
                        }
                        delim[di] = '\0';
                        if (di > 0) {
                            const char* q = p + 1;
                            while (*q) {
                                const char* ls = q;
                                while (*q && *q != '\n') q++;
                                if ((unsigned)(q - ls) == di && strncmp(ls, delim, di) == 0) {
                                    if (*q == '\n') { *out_kind = 0; *out_skip = 1; return q; }
                                    break;
                                }
                                if (*q == '\n') q++;
                            }
                        }
                    }
                }
                /* Lookahead: if followed by block keyword (then/else/fi/do/done/elif),
                 * this is part of a compound command, not a top-level separator */
                const char* ahead = skip_ws(p + 1);
                int followed_by_kw = 0;
                if (strncmp(ahead, "then", 4) == 0 && (!ahead[4] || isspace(ahead[4]) || ahead[4] == ';')) followed_by_kw = 1;
                if (strncmp(ahead, "else", 4) == 0 && (!ahead[4] || isspace(ahead[4]) || ahead[4] == ';')) followed_by_kw = 1;
                if (strncmp(ahead, "elif", 4) == 0 && (!ahead[4] || isspace(ahead[4]))) followed_by_kw = 1;
                if (strncmp(ahead, "fi", 2) == 0 && (!ahead[2] || isspace(ahead[2]) || ahead[2] == ';')) followed_by_kw = 1;
                if (strncmp(ahead, "do", 2) == 0 && (!ahead[2] || isspace(ahead[2]) || ahead[2] == ';')) followed_by_kw = 1;
                if (strncmp(ahead, "done", 4) == 0 && (!ahead[4] || isspace(ahead[4]) || ahead[4] == ';')) followed_by_kw = 1;
                
                if (!followed_by_kw) {
                    *out_kind = 0; *out_skip = 1; return p;
                }
            }
            if (c == '&' && p[1] == '&') { *out_kind = 1; *out_skip = 2; return p; }
            if (c == '|' && p[1] == '|') { *out_kind = 2; *out_skip = 2; return p; }
            if (c == '&' && p[1] != '>' && p[1] != '&' &&
                !(p > line && (p[-1] == '>' || p[-1] == '<' || p[-1] == '|'))) {
                const char* after = skip_ws(p + 1);
                if (!*after) { p++; continue; }
                *out_kind = 3; *out_skip = 1; return p;
            }
        }
next_char:
        p++;
    }
    return NULL;
}

static const char* find_top_separator(const char* line, int* out_kind, int* out_skip) {
    return find_top_separator_impl(line, strchr(line, '\n'), out_kind, out_skip);
}

/* SECURITY (audit ranks 6 & 7, recursion DoS): jash_eval_line re-enters itself
 * for command substitution, function bodies, sourced files, background lines,
 * etc., with NO depth limit — a recursive shell function (`f(){ f; }; f`) or a
 * self-sourcing file overflows the ring-3 stack and crashes the shell (ASan:
 * stack-overflow in jash_eval_line). One global depth governor caps every
 * recursive entry and errors cleanly instead of smashing the stack. The cap is
 * well above any legitimate nesting. F E R M I ∞ H A R T */
#define JASH_EVAL_MAX_DEPTH 200
/* SECURITY: function-call nesting cap (bash FUNCNEST analogue).
 *
 * Empirically, ONE level of user-function recursion costs ~300 KB of effective
 * stack: the call threads jash_eval_line -> jash_eval_line_inner -> ... -> the
 * function-call block -> jash_eval_line again, and every frame in that cycle
 * carries its own multi-KB on-stack scratch buffers. Measured crash floor:
 * ~19 levels under ASan (inflated red zones), ~28 under -O2 release. The cap is
 * set well BELOW the ASan floor so a runaway `f(){ f; }; f` returns a clean
 * "maximum function recursion depth exceeded" instead of SIGSEGV — which, in a
 * shell that is the OS command brain, would otherwise take the system down.
 *
 * TECH DEBT: 16 is low for legitimately deep recursion (deep tree walks). The
 * real fix is to shrink the per-cycle stack footprint (move the on-stack
 * list_copy[1024]/depth_local_key[192]/etc. scratch off the recursion path)
 * and then raise this cap. Tracked separately; the cap is the safety floor.
 * F E R M I ∞ H A R T */
/* BUG-057: 16 was low enough to break honest workloads — recursive fib(17)
 * silently computed the WRONG number because the depth error printed into
 * the cmdsub capture writer (swallowed) and the empty substitution became 0
 * in $((...)). Empirical crash floor on this tree (2026-07-09, -O2 release,
 * 8MB stack): plain recursion segfaults between depth 80-120, the heavier
 * cmdsub-per-level shape between 50-80. 32 keeps ~35% margin below the
 * worst measured floor. A lower user limit is honored via FUNCNEST (bash
 * parity); the compile-time cap stays the hard safety ceiling. */
#define JASH_FUNC_MAX_DEPTH 32
static int g_eval_depth = 0;
static int g_lineno = 0;
/* Depth governor for nested parameter expansions — must match limits.h */
#ifndef JASH_EVAL_MAX_DEPTH
#define JASH_EVAL_MAX_DEPTH 200
#endif

typedef struct {
    int fd;
    dev_t dev;
    ino_t ino;
} eval_procsub_fd_t;

typedef struct {
    eval_procsub_fd_t* fds;
    size_t count;
    size_t capacity;
} eval_procsub_scope_t;

/* Read ends for <(...) must survive until the command at this evaluator depth
 * has consumed /dev/fd/N, but not until process exit. */
static eval_procsub_scope_t g_procsub_scopes[JASH_EVAL_MAX_DEPTH + 1];

static eval_procsub_scope_t* eval_procsub_scope(void) {
    int idx = g_eval_depth;
    if (idx < 0) idx = 0;
    if (idx > JASH_EVAL_MAX_DEPTH) idx = JASH_EVAL_MAX_DEPTH;
    return &g_procsub_scopes[idx];
}

static int eval_procsub_reserve(void) {
    eval_procsub_scope_t* scope = eval_procsub_scope();
    if (scope->count < scope->capacity) return 1;
    size_t capacity = scope->capacity ? scope->capacity * 2 : 4;
    eval_procsub_fd_t* fds = (eval_procsub_fd_t*)realloc(
        scope->fds, capacity * sizeof(*fds));
    if (!fds) return 0;
    scope->fds = fds;
    scope->capacity = capacity;
    return 1;
}

static int eval_procsub_fstat(int fd, struct stat* st) {
    while (fstat(fd, st) != 0) {
        if (errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int eval_procsub_record(int fd) {
    eval_procsub_scope_t* scope = eval_procsub_scope();
    struct stat st;
    if (!eval_procsub_fstat(fd, &st)) return 0;
    scope->fds[scope->count].fd = fd;
    scope->fds[scope->count].dev = st.st_dev;
    scope->fds[scope->count].ino = st.st_ino;
    scope->count++;
    return 1;
}

static void eval_procsub_cleanup(void) {
    eval_procsub_scope_t* scope = eval_procsub_scope();
    for (size_t i = 0; i < scope->count; i++) {
        struct stat st;
        if (eval_procsub_fstat(scope->fds[i].fd, &st) &&
            st.st_dev == scope->fds[i].dev && st.st_ino == scope->fds[i].ino)
            sys_close(scope->fds[i].fd);
    }
    scope->count = 0;
}

static char* eval_depth_buf(char* slots[JASH_EVAL_MAX_DEPTH], size_t cap) {
    int idx = g_eval_depth;
    if (idx < 0) idx = 0;
    if (idx >= JASH_EVAL_MAX_DEPTH) idx = JASH_EVAL_MAX_DEPTH - 1;
    if (!slots[idx]) slots[idx] = (char*)calloc(1, cap);
    if (slots[idx]) slots[idx][0] = '\0';
    return slots[idx];
}

/* ADR-0036 W4: same depth-indexed lazy-allocation pattern as eval_depth_buf
 * above, sized for jash_expand_ctx_t (~104KB). jash_eval_line_inner recurses
 * up to JASH_EVAL_MAX_DEPTH — a plain stack-local ctx here would cost up to
 * 200 * 104KB (~20MB) of STACK at max recursion, blowing the 8MB default
 * long before the eval-depth guard fires. Heap slots, allocated once per
 * depth actually reached and never freed (process-lifetime reuse), cost the
 * same worst-case total but off the stack, and typically far less since
 * real recursion rarely nears the cap. */
static jash_expand_ctx_t* eval_expand_ctx_slot(void) {
    static jash_expand_ctx_t* slots[JASH_EVAL_MAX_DEPTH];
    int idx = g_eval_depth;
    if (idx < 0) idx = 0;
    if (idx >= JASH_EVAL_MAX_DEPTH) idx = JASH_EVAL_MAX_DEPTH - 1;
    if (!slots[idx]) slots[idx] = (jash_expand_ctx_t*)malloc(sizeof(jash_expand_ctx_t));
    return slots[idx];  /* NULL on OOM — caller must check */
}

/* ═══ ADR-0016 §5.1: eval decomposition — phase helpers ═══════════════════
 * jash_eval_line_inner was a single 13 KB function (the largest code
 * object in the binary). The self-contained phases below were extracted
 * verbatim into static helpers: same semantics, same call graph (LTO is
 * free to re-inline what is hot), 4x smaller review surface. */

/* Parse a (possibly negative) integer from a shell variable. */
static long eval_arith_env_long(jash_state_t* s, const char* name) {
    const char* cur = jash_env_get(s, name);
    long val = 0;
    if (cur) {
        const char* q = cur;
        int neg = 0;
        if (*q == '-') { neg = 1; q++; }
        while (*q && isdigit((unsigned char)*q)) { val = val * 10 + (*q - '0'); q++; }
        if (neg) val = -val;
    }
    return val;
}

/* Persist a long back into a shell variable. */
static void eval_arith_env_store(jash_state_t* s, const char* name, long val) {
    char vb[32];
    snprintf(vb, sizeof(vb), "%ld", val);
    jash_env_set(s, name, vb);
}

/* Phase: arithmetic command ((expr)) with bash semantics and write-back
 * of side effects (var++, --var, var+=N, var=N ...).
 * Returns -1 when the line is NOT a handled arithmetic command (caller
 * falls through to normal dispatch); otherwise returns the final rc. */
static int eval_arith_command(jash_state_t* s, jash_writer_t w, void* ctx,
                              const char* line) {
    if (!(line[0] == '(' && line[1] == '(')) return -1;

    const char* p = line + 2;
    int depth = 1;
    const char* expr_start = p;
    while (*p && depth > 0) {
        if (p[0] == '(' && p[1] == '(') { depth++; p += 2; continue; }
        if (p[0] == ')' && p[1] == ')') {
            depth--;
            if (depth == 0) break;
            p++;
            continue;
        }
        if (p[0] == '(') depth++;
        else if (p[0] == ')') { if (depth > 0) depth--; }
        p++;
    }
    if (depth != 0) return -1;  /* unterminated (( — not ours */

    /* If || or && follows )), the command-list splitter owns the line. */
    const char* after = p + 2;
    while (*after == ' ' || *after == '\t') after++;
    if ((after[0] == '|' && after[1] == '|') ||
        (after[0] == '&' && after[1] == '&'))
        return -1;

    unsigned elen = (unsigned)(p - expr_start);

    /* JINV-101: no silent truncation in $((expr)) */
    char expr_stack[1024];
    char* expr_buf = elen < sizeof(expr_stack) ? expr_stack : NULL;
    int expr_uses_heap = 0;
    if (!expr_buf) {
        expr_buf = malloc(elen + 1);
        if (!expr_buf) {
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: arithmetic sem memoria — recusado%s\n",
                        J_ROLE_ERR, J_RESET);
            return -1;
        }
        expr_uses_heap = 1;
    }
    for (unsigned i = 0; i < elen; i++) expr_buf[i] = expr_start[i];
    expr_buf[elen] = '\0';

    const char* ep = expr_buf;
    while (*ep == ' ' || *ep == '\t') ep++;

    int prefix_op = 0; /* 1=++, 2=-- */
    if (ep[0] == '+' && ep[1] == '+') { prefix_op = 1; ep += 2; while (*ep == ' ' || *ep == '\t') ep++; }
    else if (ep[0] == '-' && ep[1] == '-') { prefix_op = 2; ep += 2; while (*ep == ' ' || *ep == '\t') ep++; }

    char arith_vname[64];
    unsigned avi = 0;
    if ((*ep >= 'a' && *ep <= 'z') || (*ep >= 'A' && *ep <= 'Z') || *ep == '_') {
        while (*ep && avi < sizeof(arith_vname) - 1 &&
               (isalnum((unsigned char)*ep) || *ep == '_'))
            arith_vname[avi++] = *ep++;
        arith_vname[avi] = '\0';
    }

    /* After computing rc: persist $?, then run any ;-chained remainder. */
    #define ARITH_RETURN(rc_val) do { \
        int _rc = (rc_val); \
        s->last_status.code = _rc; \
        const char* _rest = p + 2; \
        while (*_rest == ' ' || *_rest == '\t') _rest++; \
        if (*_rest == ';' || *_rest == '\n') { \
            _rest++; \
            while (*_rest == ' ' || *_rest == '\t') _rest++; \
            if (*_rest) { \
                int _rest_rc = jash_eval_line(s, w, ctx, _rest); \
                s->last_status.code = _rest_rc; \
                if (expr_uses_heap) free(expr_buf); \
                return _rest_rc; \
            } \
        } \
        if (expr_uses_heap) free(expr_buf); \
        return _rc; \
    } while(0)

    if (avi > 0) {
        while (*ep == ' ' || *ep == '\t') ep++;

        if (prefix_op && *ep == '\0') {
            /* ++var / --var */
            long val = eval_arith_env_long(s, arith_vname);
            val = (prefix_op == 1) ? val + 1 : val - 1;
            eval_arith_env_store(s, arith_vname, val);
            ARITH_RETURN((val == 0) ? 1 : 0);
        }
        if (ep[0] == '+' && ep[1] == '+' && (ep[2] == '\0' || ep[2] == ' ')) {
            /* var++ */
            long old_val = eval_arith_env_long(s, arith_vname);
            eval_arith_env_store(s, arith_vname, old_val + 1);
            ARITH_RETURN((old_val == 0) ? 1 : 0);
        }
        if (ep[0] == '-' && ep[1] == '-' && (ep[2] == '\0' || ep[2] == ' ')) {
            /* var-- */
            long old_val = eval_arith_env_long(s, arith_vname);
            eval_arith_env_store(s, arith_vname, old_val - 1);
            ARITH_RETURN((old_val == 0) ? 1 : 0);
        }
        if (ep[0] == '+' && ep[1] == '=') {
            /* var+=expr */
            long val = eval_arith_env_long(s, arith_vname);
            val += jash_eval_arithmetic(ep + 2, s);
            eval_arith_env_store(s, arith_vname, val);
            ARITH_RETURN((val == 0) ? 1 : 0);
        }
        if (ep[0] == '-' && ep[1] == '=') {
            /* var-=expr */
            long val = eval_arith_env_long(s, arith_vname);
            val -= jash_eval_arithmetic(ep + 2, s);
            eval_arith_env_store(s, arith_vname, val);
            ARITH_RETURN((val == 0) ? 1 : 0);
        }
        if (ep[0] == '=' && ep[1] != '=') {
            /* var=expr */
            long val = jash_eval_arithmetic(ep + 1, s);
            eval_arith_env_store(s, arith_vname, val);
            ARITH_RETURN((val == 0) ? 1 : 0);
        }
    }

    /* No side-effect pattern matched — pure evaluation */
    long val = jash_eval_arithmetic(expr_buf, s);
    ARITH_RETURN((val == 0) ? 1 : 0);
    #undef ARITH_RETURN
}

/* ── Loop fast paths (parser/while_loop.c, parser/for_loop.c) ─────────────
 * Loop bodies/conditions are re-evaluated once per iteration through
 * jash_eval_line, which re-runs every text-only pass (history-expand,
 * strip-comments/joins, procsub scan, subshell scan, separator scan) before
 * reaching the dispatch. A `(( expr ))` command whose text those pre-phases
 * would pass through untouched can instead go straight to
 * eval_arith_command — the SAME function the normal dispatch reaches, so
 * side effects (i++, i+=2, ...) and the rc mapping behave identically.
 *
 * jash_arith_cmd_shape certifies exactly that: the whole text is a single
 * `(( ... ))` command (only separators after the closing `))`) and contains
 * nothing a skipped phase could rewrite:
 *   '#'  — strip-comments (also rejects `16#ff` base notation: conservative)
 *   '\'  — line-join
 *   '<(' / '>(' — process substitution
 *   '!'  — history expansion (interactive only; jash_history_expand is a
 *          stub today, but an interactive loop must not bypass a real one)
 * Anything dubious returns 0 and the caller falls back to jash_eval_line. */
int jash_arith_cmd_shape(const char* line, int interactive);
int jash_eval_arith_command(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* line);

int jash_arith_cmd_shape(const char* line, int interactive) {
    if (!line) return 0;
    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (p[0] != '(' || p[1] != '(') return 0;
    for (const char* q = p; *q; q++) {
        if (*q == '#' || *q == '\\') return 0;
        if ((*q == '<' || *q == '>') && q[1] == '(') return 0;
        if (*q == '!' && interactive) return 0;
    }
    /* Find the closing `))` with the same pairing rule as eval_arith_command. */
    const char* r = p + 2;
    int depth = 1;
    while (*r && depth > 0) {
        if (r[0] == '(' && r[1] == '(') { depth++; r += 2; continue; }
        if (r[0] == ')' && r[1] == ')') { depth--; if (depth == 0) break; r++; continue; }
        if (r[0] == '(') depth++;
        else if (r[0] == ')') { if (depth > 0) depth--; }
        r++;
    }
    if (depth != 0) return 0;
    /* Only separators may follow — `&&`/`||` belong to the command-list
     * splitter (eval_arith_command itself declines them). */
    r += 2;
    while (*r == ' ' || *r == '\t' || *r == '\n' || *r == ';') r++;
    return *r == '\0';
}

/* Evaluate a `(( expr ))` command line through the exact phase the normal
 * dispatch uses — one implementation, not a twin. Returns -1 when the line
 * is not an arithmetic command (no side effects in that case; the caller
 * must fall back to jash_eval_line). NOTE: like eval_arith_command, the
 * line must START at `((` — skip leading whitespace before calling. */
int jash_eval_arith_command(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* line) {
    return eval_arith_command(s, w, ctx, line);
}

/* Phase: process substitution <(cmd) / >(cmd) — extracted (ADR-0016 §5.1).
 * ps_buf is caller-owned (stack) so the returned pointer stays valid for
 * the remainder of the caller's evaluation of this line. Returns the
 * original line untouched when no process substitution is present. */
static const char* eval_procsub_phase(jash_state_t* s, jash_writer_t w, void* ctx,
                                      const char* line, char* ps_buf, size_t ps_cap) {
    /* Process substitution pre-processing: <(...) and >(...)
     * TODO: Use real pipes + /dev/fd/N instead of temp files (bash-compatible).
     * Current: mkstemp() + fork + waitpid (works but creates temp files).
     * Ideal: pipe() + fork + child writes to pipe[1], parent reads /dev/fd/N.
     * Requires: /dev/fd support in kernel (Linux has it, Tina needs it).
     * Scan for <( or >( in the line, execute the subcommand,
     * write output to a temp file, replace with < /tmpfile or > /tmpfile. */
    if (!(strstr(line, "<(") || strstr(line, ">("))) return line;
    /* BUG-149: the body of <(...)/>(...) must fork from the shell state AT
     * THE MOMENT its segment runs — `X=42; cat <(echo $X)` must see X, and
     * `cd /tmp; cat <(cat f)` must run in /tmp. This phase used to rewrite
     * the WHOLE line eagerly, so procsub bodies executed BEFORE the earlier
     * segments of the list (empty vars, stale cwd — and `false && cat <(x)`
     * ran x despite short-circuit). Fix: while a top-level ;/&&/|| separator
     * remains, defer — the command-list splitter recurses per segment and we
     * rewrite when that segment actually runs. Exemptions:
     *   - control-structure/{/(-led lines: consumed whole by their phases
     *     (their internal `;` never reaches the splitter);
     *   - `&` (kind 3): background segments re-enter WITH a trailing `&`
     *     appended by the splitter — re-deferring would loop forever. */
    {
        const char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        int exempt = (*t == '{' || *t == '(');
        if (!exempt) {
            static const char* kw[] = { "if", "while", "until", "for", "case", "function", 0 };
            for (int i = 0; kw[i]; i++) {
                size_t n = strlen(kw[i]);
                if (!strncmp(t, kw[i], n) && (t[n] == ' ' || t[n] == '\t')) { exempt = 1; break; }
            }
        }
        if (!exempt) {
            int k = 0, sk = 0;
            if (find_top_separator(line, &k, &sk) != NULL && k != 3) return line;
        }
    }
    {
        unsigned po = 0;
        const char* rp = line;
        /* BUG-050 (same class as BUG-036): the scan below used to check
         * rp[0]=='<'/'>' && rp[1]=='(' with NO quote-state tracking at all,
         * so `echo "<("` (or any argument merely CONTAINING that literal
         * text — even a bare filename) got misdetected as real process
         * substitution and the whole line was rewritten/corrupted. Track
         * quote state and only treat <(/>( as an operator outside quotes. */
        int sq = 0, dq = 0;
        while (*rp && po < ps_cap - 1) {
            if (!dq && *rp == '\'') { sq = !sq; ps_buf[po++] = *rp++; continue; }
            if (!sq && *rp == '"')  { dq = !dq; ps_buf[po++] = *rp++; continue; }
            if (!sq && dq && *rp == '\\' && rp[1]) { ps_buf[po++] = *rp++; if (po < ps_cap - 1) ps_buf[po++] = *rp++; continue; }
            if (!sq && !dq && rp[0] == '<' && rp[1] == '(') {
                /* <(cmd) — run cmd, provide output via /dev/fd/N (bash-compatible).
                 * Uses real pipes instead of temp files for efficiency. */
                rp += 2;
                int depth = 1;
                const char* cmd_start = rp;
                while (*rp && depth > 0) {
                    if (*rp == '(') depth++;
                    else if (*rp == ')') depth--;
                    if (depth > 0) rp++;
                }
                unsigned cmd_len = rp - cmd_start;
                if (depth == 0 && cmd_len > 0) {
                    char cmd[1024];
                    if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
                    for (unsigned i = 0; i < cmd_len; i++) cmd[i] = cmd_start[i];
                    cmd[cmd_len] = '\0';
                    rp++; /* skip ) */

                    /* Create pipe: child writes to pipe[1], parent reads from pipe[0] */
                    int pipefd[2];
                    if (!eval_procsub_reserve()) {
                        jash_writes(jash_stderr_writer, 0,
                                    "jash: process substitution: out of memory\n");
                        return NULL;
                    }
                    if (pipe(pipefd) == 0) {
                        int pid = sys_fork();
                        if (pid == 0) {
                            /* Child: redirect stdout to pipe write end, exec cmd */
                            sys_close(pipefd[0]);  /* close read end */
                            sys_dup2(pipefd[1], 1);
                            sys_close(pipefd[1]);
                            jash_eval_line(s, w, ctx, cmd);
                            sys_exit(0);
                        } else if (pid > 0) {
                            /* Parent: keep read end, close write end */
                            sys_close(pipefd[1]);
                            int read_fd = pipefd[0];
                            if (!eval_procsub_record(read_fd)) {
                                sys_close(read_fd);
                                jash_writes(jash_stderr_writer, 0,
                                            "jash: process substitution: cannot track fd\n");
                                return NULL;
                            }
                            /* Replace <(...) with /dev/fd/N */
                            char devfd[32];
                            snprintf(devfd, sizeof(devfd), "/dev/fd/%d", read_fd);
                            for (unsigned k = 0; devfd[k] && po < ps_cap - 1; k++)
                                 ps_buf[po++] = devfd[k];
                        } else {
                            sys_close(pipefd[0]);
                            sys_close(pipefd[1]);
                            jash_writes(jash_stderr_writer, 0,
                                        "jash: process substitution: fork failed\n");
                            return NULL;
                        }
                    } else {
                        jash_writes(jash_stderr_writer, 0,
                                    "jash: process substitution: pipe failed\n");
                        return NULL;
                    }
                } else {
                    /* Malformed, copy literally */
                    ps_buf[po++] = '<';
                    ps_buf[po++] = '(';
                }
            }
            else if (!sq && !dq && rp[0] == '>' && rp[1] == '(') {
                /* >(cmd) — run cmd, feed it stdin from this point */
                rp += 2;
                int depth = 1;
                const char* cmd_start = rp;
                while (*rp && depth > 0) {
                    if (*rp == '(') depth++;
                    else if (*rp == ')') depth--;
                    if (depth > 0) rp++;
                }
                unsigned cmd_len = rp - cmd_start;
                if (depth == 0 && cmd_len > 0) {
                    char cmd[1024];
                    if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
                    for (unsigned i = 0; i < cmd_len; i++) cmd[i] = cmd_start[i];
                    cmd[cmd_len] = '\0';
                    rp++; /* skip ) */

                    /* Use a temporary file: main command writes to file, then we run cmd with file as stdin */
                    char tmpname[256];
                    snprintf(tmpname, sizeof(tmpname), "%s/jash_ps_XXXXXX", jash_tmpdir());
                    int tmpfd = mkstemp(tmpname);
                    if (tmpfd >= 0) {
                        fcntl(tmpfd, F_SETFD, FD_CLOEXEC);
                        sys_close(tmpfd);
                        /* Replace >(...) with > /tmp/file */
                        const char* prefix = " > ";
                        for (unsigned k = 0; prefix[k] && po < ps_cap - 1; k++)
                            ps_buf[po++] = prefix[k];
                        for (unsigned k = 0; tmpname[k] && po < ps_cap - 2; k++)
                            ps_buf[po++] = tmpname[k];
                        /* TODO: after main command finishes, run: cmd < /tmp/file && rm /tmp/file */
                        /* For now, this is a partial implementation */
                    }
                } else {
                    ps_buf[po++] = '>';
                    ps_buf[po++] = '(';
                }
            }
            else {
                ps_buf[po++] = *rp++;
            }
        }
        /* BUG-039 class-kill: the malformed `<(`/`>(` branches above write TWO bytes
         * while the loop guard only reserved one (po < ps_cap-1), so po can reach
         * ps_cap — clamp the terminator so it never writes ps_buf[ps_cap]. */
        ps_buf[po < ps_cap ? po : ps_cap - 1] = '\0';
        return ps_buf;
    }
    return line;
}

static int jash_eval_line_inner(jash_state_t* s, jash_writer_t w, void* ctx, const char* line);

/* Background spawner: forks `bg_line` (already stripped of the trailing
 * `&`) into its own process group and registers the job. Shared by the
 * trailing-`&` block and the pipeline dispatch (`a | b &` backgrounds
 * the whole pipeline). Returns 0 in the parent; the child never returns. */
static int jash_bg_spawn(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* bg_line);

/* JINV-066 — "isto comeca com o `!` de negacao de pipeline?"
 *
 * Existem DOIS pontos que negam: o divisor de `;`/`&&`/`||` (que ja negava) e
 * o caminho de pipeline unico (que nao negava — `! true` virava busca por um
 * programa chamado `!`, rc 127). Duas portas com a mesma regra escrita duas
 * vezes e como o JINV-063 nasceu, entao a regra mora aqui e os dois a
 * chamam.
 *
 * Exige espaco/tabulacao/fim depois do `!`: `!cmd` colado NAO e negacao (e o
 * override do loopguard, ADR-0042 F3, e para o bash e nome de comando). */
static int jash_bang_negation(const char* p) {
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    return (p[0] == '!' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0'));
}


/* JINV-036 — o que vem DEPOIS de `fi`/`esac` tem que executar.
 *
 * `jash_eval_line` chamava o tratador de bloco e fazia `return rc` na hora,
 * descartando o resto da string. Fora de funcao isso nunca aparecia: o driver
 * de script alimenta uma linha logica por vez, entao `echo DEPOIS` chegava
 * como chamada separada. DENTRO de uma funcao o corpo inteiro vai numa string
 * so — e tudo depois do `fi` sumia, em silencio:
 *
 *   f() {
 *     if [ ! -d "$D" ]; then mkdir "$D"; fi
 *     cp arquivo "$D"        <- NUNCA EXECUTAVA
 *   }
 *
 * Medido contra o bash: `if` e `case` sofriam; `for`, `while` e `until` NAO —
 * eles ja rodavam o resto (for_loop.c:253, o idioma que este conserto copia).
 * Tres tratadores certos e dois errados, sem gate que cobrasse a diferenca.
 *
 * Isto tambem explicava por que TODA recursao de funcao parecia quebrada: a
 * chamada recursiva vem depois do `if` de caso-base, entao nunca acontecia.
 *
 * Devolve o texto util depois da palavra-chave de fechamento, ou NULL quando
 * nao ha nada. Conta aninhamento e respeita aspas — um `fi` dentro de "..."
 * e dado, nao estrutura (a licao dos JINV-020/032/034). */
/* JINV-074 — a mesma varredura, agora dizendo TAMBEM onde o bloco acaba.
 *
 * `head_len` (opcional) recebe o comprimento do BLOCO, isto e, o numero de
 * bytes de `line` ate logo depois da palavra-chave de fechamento — sem o
 * `;`/nova-linha nem a cauda. Esse numero e o que permite ao chamador entregar
 * ao tratador de bloco uma string que TERMINA no `fi`, e com isso ser o dono
 * unico da cauda (veja o comentario grande no sitio do `if`). */
static const char* jash_block_split(const char* line,
                                    const char* open_kw, const char* close_kw,
                                    size_t* head_len)
{
    if (head_len) *head_len = 0;
    if (!line || !open_kw || !close_kw) return 0;
    size_t ol = strlen(open_kw), cl = strlen(close_kw);
    int depth = 0;
    int sq = 0, dq = 0, back = 0;
    const char* p = line;

    /* Fronteira de palavra: inicio, espaco, `;`, nova linha, ou `(`.
     *
     * JINV-083: fronteira de palavra + aspas ainda deixavam passar a palavra
     * reservada em posicao de ARGUMENTO. `jash_at_cmd_pos` (parser/script.h) e
     * a terceira condicao — sem ela, o `fi` de `echo fi` fechava o bloco:
     *
     *   if true; then echo fi; fi; echo cauda
     *     bash -> `fi` + `cauda`
     *     jash -> bloco cortado em `if true; then echo fi` -> "missing fi"  */
    #define JBT_LEFT_OK(q)  (((q) == line || (q)[-1] == ' ' || (q)[-1] == '\t' || \
                             (q)[-1] == '\n' || (q)[-1] == ';' || (q)[-1] == '(') && \
                             jash_at_cmd_pos(line, (q)))
    #define JBT_RIGHT_OK(q) (*(q) == '\0' || *(q) == ' ' || *(q) == '\t' || \
                             *(q) == '\n' || *(q) == ';' || *(q) == '&' || \
                             *(q) == '|' || *(q) == ')')

    while (*p) {
        char c = *p;
        if (sq) { if (c == '\'') sq = 0; p++; continue; }
        if (dq) { if (c == '\\' && p[1]) { p += 2; continue; }
                  if (c == '"') { dq = 0; } p++; continue; }
        if (back) { if (c == '`') back = 0; p++; continue; }
        if (c == '\\' && p[1]) { p += 2; continue; }
        if (c == '\'') { sq = 1; p++; continue; }
        if (c == '"')  { dq = 1; p++; continue; }
        if (c == '`')  { back = 1; p++; continue; }

        if (strncmp(p, open_kw, ol) == 0 && JBT_LEFT_OK(p) && JBT_RIGHT_OK(p + ol)) {
            depth++; p += ol; continue;
        }
        if (strncmp(p, close_kw, cl) == 0 && JBT_LEFT_OK(p) && JBT_RIGHT_OK(p + cl)) {
            depth--;
            p += cl;
            if (depth <= 0) {
                if (head_len) *head_len = (size_t)(p - line);   /* JINV-074 */
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ';' || *p == '\n') { p++; while (*p == ' ' || *p == '\t' || *p == '\n') p++; }
                return *p ? p : 0;
            }
            continue;
        }
        p++;
    }
    #undef JBT_LEFT_OK
    #undef JBT_RIGHT_OK
    return 0;
}

/* Compatibilidade: os chamadores que so querem a cauda continuam com a mesma
 * assinatura de sempre. Uma implementacao, dois usos — o contrario do defeito
 * que o JINV-074 conserta. */
static const char* jash_block_tail(const char* line,
                                   const char* open_kw, const char* close_kw)
{
    return jash_block_split(line, open_kw, close_kw, 0);
}

int jash_eval_line(jash_state_t* s, jash_writer_t w, void* ctx, const char* line) {
    if (!s || !line) return 0;
    /* BUG-030: re-expand any registered heredoc/here-string body referenced by
     * this line against the CURRENT shell state, right before it executes. The
     * for/while executor calls jash_eval_line(body) once per iteration with the
     * loop variable already set, so each render sees the correct per-iteration
     * value. Hot no-op (single counter check) when nothing is registered. */
    jash_heredoc_expand_line(s, line);
    if (g_eval_depth >= JASH_EVAL_MAX_DEPTH) {
        static const char msg[] = "jash: recursion limit exceeded\n";
        if (w) w(ctx, msg, sizeof(msg) - 1);
        return 1;
    }

    /* ADR-0003 M3: pre/post_cmd hooks — only at top-level eval (depth 0).
     * Recursive evals (function calls, eval builtin) do NOT fire hooks to
     * avoid double-counting and infinite loops in hook bodies. */
    int toplevel = (g_eval_depth == 0);
    if (toplevel) {
        /* Expansion refusal belongs to one submitted top-level command. A
         * prior failed line must not poison the next line in a script. */
        jash_cmdsub_clear_error();
        jash_glob_clear_error();
        jash_param_expand_clear_error();
        /* JINV-073: no nivel de topo NAO existe laco em voo. Qualquer marca de
         * break/continue que tenha sobrevivido ate aqui e lixo de um `break`
         * escrito fora de laco (POSIX: no-op), e deixa-la posta envenenaria a
         * proxima linha — um `(exit 255)` legitimo seria lido como break.
         * O canal de controle nasce limpo a cada linha de topo. */
        jash_loopctl_clear(s);
        const char* tl = line;
        while (*tl == ' ' || *tl == '\t') tl++;
        /* ADR-0050 W12: risk-gated auto-checkpoint — the shell scores the
         * line with the SAME engine as `explain --risk` (single source,
         * builtin_explain.c) and, at critical (≥80), drops/refreshes the
         * `auto` checkpoint BEFORE executing. Today only `rm` does this;
         * after W12 any critical line is pre-checkpointed: fork bomb, mkfs,
         * dd to a device, rm -rf on / or ~, curl|sh. The checkpoint never
         * blocks — it arms `rewind auto`, it does not judge. Kill-switch:
         * JASH_RISK_CHECKPOINT=0. Meta-commands (the rewind family itself)
         * never self-trigger. */
        {  static const char* const meta[] = {
               "checkpoint", "rewind", "undo", "redo", "explain", "sense",
               "speculate", "contract", 0 };
           int is_meta = 0;
           for (int i = 0; meta[i]; i++) {
               size_t ml = strlen(meta[i]);
               if (!strncmp(tl, meta[i], ml) &&
                   (tl[ml] == ' ' || tl[ml] == '\t' || tl[ml] == '\0')) {
                   is_meta = 1; break;
               }
           }
           const char* rc_env = getenv("JASH_RISK_CHECKPOINT");
           int rc_on = !(rc_env && rc_env[0] == '0' && rc_env[1] == '\0');
           if (rc_on && !is_meta) {
               extern int jash_explain_risk(const char*, const char**);
               extern int jash_checkpoint_auto(void);
               const char* reason = "";
               int score = jash_explain_risk(tl, &reason);
               if (score >= 80) {
                   if (jash_checkpoint_auto()) {
                       jash_writef(jash_stderr_writer, 0,
                           "\033[90m[jash] critical line (risk %d%% — %s): "
                           "auto-checkpoint armed, `rewind auto` to undo\033[0m\n",
                           score, reason);
                   }
               }
           } }
        /* ADR-0051 W4: ACTIVE memory — this exact command already failed?
         * Warn BEFORE it runs again, with the attested proof (count + last
         * occurrence). One line, only with evidence (≥2 identical failures
         * in the tail window), never a vibe. Meta-commands exempt: the
         * memory subsystem never warns about itself. */
        {  static const char* const rmeta[] = {
               "recall", "expect", "checkpoint", "rewind", "undo", "redo", 0 };
           int is_rmeta = 0;
           for (int i = 0; rmeta[i]; i++) {
               size_t ml = strlen(rmeta[i]);
               if (!strncmp(tl, rmeta[i], ml) &&
                   (tl[ml] == ' ' || tl[ml] == '\t' || tl[ml] == '\0')) {
                   is_rmeta = 1; break;
               }
           }
           if (!is_rmeta) {
               extern unsigned jash_recall_guard(const char*,
                                                 unsigned long long*,
                                                 unsigned long long*, int*);
               unsigned long long gseq = 0, gts = 0; int grc = 0;
               unsigned nf = jash_recall_guard(tl, &gseq, &gts, &grc);
               if (nf >= 2) {
                   char when[16] = "??:??";
                   time_t gt = (time_t)gts;
                   struct tm* gtm = localtime(&gt);
                   if (gtm) snprintf(when, sizeof when, "%02d:%02d",
                                     gtm->tm_hour, gtm->tm_min);
                   jash_writef(jash_stderr_writer, 0,
                       "\033[33m[jash recall]\033[0m this exact command failed "
                       "%u× before (last: rc=%d %s, seq #%llu) — "
                       "`recall why` shows what changed\n",
                       nf, grc, when, gseq);
               }
           } }
#ifndef JASH_AGENT_ONLY
        if (g_modules.count > 0
#ifdef JASH_TINALINUX
            && *tl != '?'
#endif
        ) {
            jmod_call_t _hmc = { s, (jmod_writer_t)w, ctx, NULL, tl };
            jmod_hook_pre_cmd(&_hmc);
        }
#endif
        jash_sigil_pre_cmd(tl);
    }

    g_eval_depth++;
    if (g_eval_depth == 1) g_lineno++;  /* count top-level command lines */

#ifndef JASH_AGENT_ONLY
    struct timespec _brain_t0 = {0}, _brain_t1 = {0};
    int _brain_clock_ok = toplevel && clock_gettime(CLOCK_MONOTONIC, &_brain_t0) == 0;
    int _brain_tl_before = toplevel ? jash_timeline_count() : 0;
#endif

    int jash_eval_rc = jash_eval_line_inner(s, w, ctx, line);
    eval_procsub_cleanup();
    g_eval_depth--;

    if (toplevel) {
        const char* tl = line;
        while (*tl == ' ' || *tl == '\t') tl++;
#ifndef JASH_AGENT_ONLY
        if (g_modules.count > 0
#ifdef JASH_TINALINUX
            && *tl != '?'
#endif
        ) {
            jmod_call_t _hmc = { s, (jmod_writer_t)w, ctx, NULL, tl };
            jmod_hook_post_cmd(&_hmc, jash_eval_rc);
        }
#endif
        jash_sigil_post_cmd(tl, jash_eval_rc);

#ifndef JASH_AGENT_ONLY
        /* ADR-0033 Wave 2: feed the Liquid Reasoning Engine.
         * PERF: gate BEFORE getcwd() — on macOS getcwd() walks and open()s
         * parent directories, and paying that per command when the brain is
         * off was a top-3 profile entry. F E R M I ∞ H A R T */
        if (jash_liquid_should_hook()) {
            uint64_t dur_ms = 0;
            if (_brain_clock_ok && clock_gettime(CLOCK_MONOTONIC, &_brain_t1) == 0) {
                uint64_t start_ns = (uint64_t)_brain_t0.tv_sec * 1000000000ULL +
                                    (uint64_t)_brain_t0.tv_nsec;
                uint64_t end_ns = (uint64_t)_brain_t1.tv_sec * 1000000000ULL +
                                  (uint64_t)_brain_t1.tv_nsec;
                if (end_ns >= start_ns) dur_ms = (end_ns - start_ns) / 1000000ULL;
            }
            int mutations = jash_timeline_count() - _brain_tl_before;
            char cwd[512];
            const char* cwdptr = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
            jash_liquid_post_cmd(tl, jash_eval_rc, dur_ms, mutations > 0 ? (uint32_t)mutations : 0,
                                 cwdptr, w, ctx);
        }
#endif
    }

    /* set -e (errexit): exit the shell when a top-level command fails.
     * The command-list splitter handles `;`/`&&`/`||` internally; we handle
     * the outer case here so that separate lines and compound commands all
     * honour errexit correctly.  We do NOT fire if exit was already requested
     * (e.g. by the `exit` builtin) or if this was triggered from inside a
     * function body or subshell (those are non-toplevel calls). */
    if (toplevel && s->shell_opts.errexit && jash_eval_rc != 0
            && !s->exit_requested) {
        s->exit_requested = 1;
        s->last_status.code = jash_eval_rc;
    }

    /* After every top-level eval, drain any pending signal traps. */
    if (g_eval_depth == 0) {
        jash_run_pending_traps(s, w, ctx);
    }
    return jash_eval_rc;
}

/* Raw-args allowlist, dispatched by leading bytes instead of ~25 strcmp
 * calls per builtin invocation. Every leaf is confirmed by a full strcmp,
 * so the decision is identical to the inline chain this replaces; a
 * transcription slip could only miss the fast path, never misroute.
 * Membership rationale lives with each fix: BUG-127 (let), ADR-0050 W2
 * (metaops), BUG-151 (env), grep/mesh/contract/cost/revert tokenizers,
 * BUG-059 (eval). Add new quote-aware builtins here AND keep the
 * JINV-096 lesson: audit every sibling predicate when adding sentinels. */
static int jash_skip_unquote_builtin(const char* name) {
    /* Index guards: name[1] is always readable (name[0] != 0); deeper
     * indices are read only after proving the prefix is present, so no
     * byte past the NUL is ever touched. */
    if (!name || !name[0]) return 0;
    char c1 = name[1];
    switch (name[0]) {
        case 'b': return strcmp(name, "builtin") == 0;
        case 'c':
            if (!c1) return 0;
            if (c1 == 'o' && name[2]) {
                if (name[2] == 'm') return strcmp(name, "command") == 0;
                if (name[2] == 'n') return strcmp(name, "contract") == 0;
                if (name[2] == 's') return strcmp(name, "cost") == 0;
                return 0;
            }
            if (c1 == 'h' && name[2]) {
                if (name[2] == 'm') return strcmp(name, "chmod") == 0;
                return strcmp(name, "chown") == 0;
            }
            return 0;
        case 'd': return strcmp(name, "declare") == 0;
        case 'e':
            if (!c1) return 0;
            if (c1 == 'n') return strcmp(name, "env") == 0;
            if (c1 == 'v' && name[2]) {
                if (name[2] == 'a') return strcmp(name, "eval") == 0;
                return 0;
            }
            if (c1 == 'x' && name[2] && name[3]) {
                if (name[3] == 'e') return strcmp(name, "expect") == 0;
                if (name[3] == 'o') return strcmp(name, "export") == 0;
                return 0;
            }
            return 0;
        case 'g': return strcmp(name, "grep") == 0;
        case 'l':
            if (!c1) return 0;
            if (c1 == 'e') return strcmp(name, "let") == 0;
            if (c1 == 'o') return strcmp(name, "local") == 0;
            if (c1 == 'n') return strcmp(name, "ln") == 0;
            return 0;
        case 'm':
            if (!c1) return 0;
            if (c1 == 'k') return strcmp(name, "mkdir") == 0;
            if (c1 == 'e') return strcmp(name, "mesh") == 0;
            return 0;
        case 'p': return strcmp(name, "printf") == 0;
        case 'r':
            if (!c1) return 0;
            if (c1 == 'e' && name[2]) {
                if (name[2] == 'a') return strcmp(name, "readonly") == 0;
                if (name[2] == 'v') return strcmp(name, "revert") == 0;
                return 0;
            }
            if (c1 == 'm') return strcmp(name, "rmdir") == 0;
            return 0;
        case 's': return strcmp(name, "set") == 0;
        case 't':
            if (!c1) return 0;
            if (c1 == 'r') return strcmp(name, "trap") == 0;
            if (c1 == 'y') return strcmp(name, "typeset") == 0;
            return 0;
        default: return 0;
    }
}

/* Prove that the source is one literal simple command before entering the
 * compound/expansion parsers. This is deliberately sufficient, not complete:
 * quotes, escapes, expansions, operators, sentinels and non-ASCII fall back.
 * Require a builtin name, and exclude keywords even if a plugin registers one.
 * Only parsing is skipped: the shared resolver below still applies aliases,
 * functions, `command`, enable/disable and every execution hook at runtime. */
static int eval_literal_builtin(const char* line) {
    char name[64];
    size_t n = 0;
    while (line[n] && line[n] != ' ' && line[n] != '\t') {
        unsigned char c = (unsigned char)line[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == ':') ||
            n + 1 >= sizeof(name)) return 0;
        name[n] = (char)c;
        n++;
    }
    if (!n) return 0;
    name[n] = '\0';
    if (!jash_builtin_lookup(name)) return 0;
    if (!strcmp(name, "time") || !strcmp(name, "if") ||
        !strcmp(name, "for") || !strcmp(name, "while") ||
        !strcmp(name, "until") || !strcmp(name, "case") ||
        !strcmp(name, "select") || !strcmp(name, "function") ||
        !strcmp(name, "coproc")) return 0;
    for (; n + 1 < JASH_LINE_CAP_STACK && line[n]; n++) {
        unsigned char c = (unsigned char)line[n];
        if ((c < 0x20 && c != '\t') || c >= 0x7f) return 0;
        switch (c) {
            case '\'': case '"': case '\\': case '$': case '`': case '~':
            case '*': case '?': case '[': case ']': case '{': case '}':
            case '(': case ')': case ';': case '&': case '|': case '<':
            case '>': case '#': case '!':
                return 0;
        }
    }
    /* Preserve the full parser's size refusal and error-state processing. */
    return n + 1 < JASH_LINE_CAP_STACK && !line[n] && !jash_glob_error();
}

static int jash_eval_line_inner(jash_state_t* s, jash_writer_t w, void* ctx, const char* line) {
    // SECURITY: history expansion (bash-compatible !!, !n, !str, !?str?)
    static char* expanded_slots[JASH_EVAL_MAX_DEPTH];
    char* expanded = eval_depth_buf(expanded_slots, 4096);
    if (!expanded) return 1;
    if (jash_history_expand(s, line, expanded, 4096)) {
        /* Visual feedback: show expanded command (bash parity).
         * Only in interactive mode (non-empty ctx or w is stdout writer). */
        if (s->interactive && expanded[0]) {
            jash_writes(w, ctx, expanded);
            jash_writes(w, ctx, "\n");
        }
        line = expanded;
    }
    

    if (!s || !line) return 0;
    jash_run_pending_traps(s, w, ctx);
    /* Run DEBUG trap before each command (if set). */
    jash_run_trap_debug(s, w, ctx);
    line = skip_ws(line);
    if (!*line || *line == '#') return 0; /* empty / comment */

    if (eval_literal_builtin(line)) goto simple_command;

    /* ADR-0003 M2: '?' prefix — intercept BEFORE glob/word expansion so that
     * '?' is not treated as a glob wildcard (would expand to filenames). */
    if (line[0] == '?') {
#ifndef JASH_AGENT_ONLY
#ifdef JASH_TINALINUX
        return jash_ai_route(s, w, ctx, line);
#else
        const char* q_args = line + 1;
        while (*q_args == ' ') q_args++;
        jmod_call_t _mc = { s, (jmod_writer_t)w, ctx, "?", q_args };
        int _mrc = jmod_dispatch("?", &_mc);
        if (_mrc < 0) _mrc = jash_ai_route(s, w, ctx, line) ? 0 : 1;
        s->last_status.code = _mrc;
        return _mrc;
#endif
#else
        jash_writes(jash_stderr_writer, 0,
                    "jash-agentd: dynamic agent routing is unavailable\n");
        return 127;
#endif
    }

    /* Strip trailing `# comment` (bash parity): `#` at the start of a word
     * (preceded by whitespace, ; & | ( ) or at column 0) opens a comment that
     * runs to end-of-line. Inside quotes or as a non-word-start char it's
     * literal. Without this, `echo a #note` printed "a #note". */
    {
        static char* comment_slots[JASH_EVAL_MAX_DEPTH];
        char* cbuf = eval_depth_buf(comment_slots, 4096);
        if (cbuf) {
            /* JINV-070/JINV-071: o analisador de aspas que vivia AQUI (com a
             * pilha por nivel de `$(`/`${` dos BUG-099/BUG-115/BUG-154 e com o
             * rtrim consciente de aspas do JINV-063) foi PROMOVIDO a
             * `jash_strip_comments_and_joins`, em parser/quotes.c. Nao e uma
             * copia: e a MESMA leitura, agora publica, para que o rastreador
             * de bloco use a mesma nocao de comentario e de continuacao que o
             * topo. Eram dois analisadores sobre o mesmo texto e so um estava
             * certo — mesma familia dos 4 casadores de glob e dos 7 rtrim
             * cegos ja mortos.
             *
             * O que muda de comportamento, e por que:
             *  - o comentario acaba na NOVA-LINHA, nao no fim da string. O
             *    corte antigo engolia o `fi`/`done`/`esac` de um bloco de
             *    varias linhas e abortava o ARQUIVO inteiro — 208 dos 2 176
             *    scripts reais medidos, e sozinho zerava os 19 `configure`
             *    (audits/BENCHMARK-scripts-reais.md 4.1);
             *  - `\`+nova-linha passa a ser juncao POSIX tambem aqui, porque
             *    dentro de um bloco ninguem havia juntado antes (759 scripts,
             *    secao 4.2);
             *  - devolveu -1 (nao coube no buffer): a linha ORIGINAL segue
             *    intacta em vez de ser truncada. */
            int nch = jash_strip_comments_and_joins(line, cbuf, 4096);
            if (nch > 0) {
                line = cbuf;
                if (!*line) return 0;
            }
        }
    }

    /* Inline calculator: = EXPRESSION */
    if (line[0] == '=' && (line[1] == ' ' || line[1] == '\t' ||
                           line[1] == '\0')) {
        return jash_b_calc(s, w, ctx, line + 1);
    }

    /* Phase: arithmetic command ((expr)) — extracted (ADR-0016 §5.1). */
    {
        int arc = eval_arith_command(s, w, ctx, line);
        if (arc >= 0) return arc;
    }

    /* Phase: process substitution — extracted (ADR-0016 §5.1). */
    static char* ps_slots[JASH_EVAL_MAX_DEPTH];
    char* ps_buf = eval_depth_buf(ps_slots, 4096);
    if (!ps_buf) return 1;
    line = eval_procsub_phase(s, w, ctx, line, ps_buf, 4096);
    if (!line) return 1;

    /* ── Subshell: (cmd) ──────────────────────────────────────────────
     * Bash-compatible subshell: fork a child, evaluate the body in it,
     * parent waits. Variable assignments, cd, etc. in the child do NOT
     * affect the parent. Detected BEFORE function-def and command-list
     * splitters so the '(' is not mistaken for a function signature or
     * a literal command name.
     *
     * When followed by a pipe (e.g. `(body) | cat`), we fall through
     * to the pipeline engine which has its own subshell-segment support.
     *
     * Use the same evaluator for every body. A second direct-exec parser
     * used to truncate the body, argv and environment independently, and
     * bypassed ordinary command semantics. fork gives the evaluator private
     * memory; a vfork child cannot safely return through a shared wrapper. */
    if (line[0] == '(' && line[1] != '(') {
        /* Skip leading '(' */
        const char* body_start = line + 1;
        /* Find matching ')' respecting nesting and quotes */
        int depth = 1;
        const char* p = body_start;
        char sq = 0, dq = 0;
        while (*p && depth > 0) {
            char c = *p;
            if (sq) { if (c == '\'') sq = 0; p++; continue; }
            if (dq) {
                if (c == '\\' && p[1]) { p += 2; continue; }
                if (c == '"') dq = 0;
                p++; continue;
            }
            if (c == '\\') { sq = 0; /* not in quote, skip next */ p++; if (*p) p++; continue; }
            if (c == '\'') { sq = 1; p++; continue; }
            if (c == '"')  { dq = 1; p++; continue; }
            if (c == '(')  depth++;
            else if (c == ')') { depth--; if (depth == 0) break; }
            p++;
        }
        if (depth == 0) {
            /* p points at closing ')' */
            size_t body_len = (size_t)(p - body_start);
            /* Trim leading/trailing whitespace from body */
            while (body_len > 0 && (*body_start == ' ' || *body_start == '\t' || *body_start == '\n')) {
                body_start++; body_len--;
            }
            while (body_len > 0 && (body_start[body_len-1] == ' ' || body_start[body_len-1] == '\t' || body_start[body_len-1] == '\n' || body_start[body_len-1] == ';')) {
                body_len--;
            }

            /* What comes after the ')'? */
            const char* after = p + 1; /* skip ')' */
            while (*after == ' ' || *after == '\t') after++;

            /* Pipeline fallthrough: if followed by | OR by a redirect
             * operator (>, <, >>, 2>, &>, >|, etc.), let the pipeline
             * engine handle the whole line — it has subshell-segment
             * support, creates proper pipes, AND applies redirects to
             * the subshell child. Without the redirect cases here, the
             * subshell handler below ran the body, then recursively
             * evaluated `>file` as a standalone "command", producing
             * a spurious "/dev/null: command not found". */
            if (*after == '|' && after[1] != '|') {
                goto skip_subshell;
            }
            if (*after == '>' || *after == '<' ||
                ((*after >= '0' && *after <= '9') &&
                 (after[1] == '>' || after[1] == '<'))) {
                goto skip_subshell;
            }
            /* BUG-019: a command-list operator after ')' — `(..) && x`,
             * `(..) || x`, `(..) ; x`, `(..) &` — must thread the subshell's
             * exit status through the list executor. Evaluating `after`
             * standalone (below) lost sub_rc: the operator saw an implicit
             * rc 0, so `( false ) || echo` never ran the echo (and
             * `( true ) && echo` only worked by luck). Defer to the list
             * splitter, which re-enters this handler for the `( .. )` segment
             * (with empty `after`) and applies the operator with the real rc. */
            if ((after[0] == '&' && after[1] == '&') ||
                (after[0] == '|' && after[1] == '|') ||
                (after[0] == ';') ||
                (after[0] == '&')) {
                goto skip_subshell;
            }

            /* Own the complete body and remainder before any child effect.
             * Recursive evaluation reuses depth-local expansion buffers, so
             * neither string may remain a borrowed slice of those buffers. */
            char* sub_body = (char*)malloc(body_len + 1);
            char* after_copy = *after ? strdup(after) : NULL;
            if (!sub_body || (*after && !after_copy)) {
                free(sub_body);
                free(after_copy);
                jash_writes(jash_stderr_writer, NULL,
                            "jash: cannot allocate complete subshell command\n");
                s->last_status.code = 125;
                return 125;
            }
            memcpy(sub_body, body_start, body_len);
            sub_body[body_len] = '\0';

            int sub_rc = 0;
            long sub_pid = sys_fork();
            if (sub_pid == 0) {
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                int erc = jash_eval_line(s, w, ctx, sub_body);
                sys_exit(erc);
                for (;;) { }
            } else if (sub_pid > 0) {
                int status = 0;
                long waited;
                do { waited = sys_waitpid(sub_pid, &status, 0); }
                while (waited < 0 && errno == EINTR);
                if (waited < 0) {
                    jash_writes(jash_stderr_writer, NULL,
                                "jash: wait failed for subshell\n");
                    sub_rc = 125;
                } else if ((status & 0x7F) == 0) sub_rc = (status >> 8) & 0xFF;
                else if ((status & 0x7F) != 0x7F) sub_rc = 128 + (status & 0x7F);
                else sub_rc = 128;
            } else {
                jash_writes(jash_stderr_writer, NULL, "jash: fork failed for subshell\n");
                sub_rc = 125;
            }
            free(sub_body);

            s->last_status.code = sub_rc;

            if (after_copy) {
                int rest_rc = jash_eval_line(s, w, ctx, after_copy);
                free(after_copy);
                s->last_status.code = rest_rc;
                return rest_rc;
            }
            return sub_rc;
        }
        /* Unterminated ( — fall through to normal command handling */
    }
skip_subshell:

    /* v0.7: shell function definition — name() { body }
     * Checked BEFORE the separator splitter so the ';' inside the body
     * are not mistaken for top-level command separators. */
    {
        char fname[64], fbody[JASH_FN_BODY_INLINE];
        char* fbody_heap = NULL;   /* JINV-086 */
        const char* rest = NULL;
        int fdef = parse_function_def(line, fname, sizeof(fname),
                                      fbody, sizeof(fbody), &rest, &fbody_heap);
        if (fdef < 0) {
            /* JINV-086: sem memória para o corpo. RECUSA ALTA — definir uma
             * função com o corpo cortado é registrar um programa diferente do
             * escrito, e é exatamente isso que este invariante existe para
             * impedir. */
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: %s: sem memoria para o corpo da funcao — ela NAO foi definida%s\n",
                        J_ROLE_ERR, fname, J_RESET);
            s->last_status.code = 2;
            if (s && !s->interactive) s->exit_requested = 1;
            return 2;
        }
        if (fdef) {
            /* JINV-086: o retorno era DESCARTADO. Tabela cheia ou falta de
             * memória sumiam sem uma palavra, e a chamada seguinte caía em
             * "command not found" (ou, pior, num externo homônimo). */
            int drc = jash_function_define(s->functions, fname,
                                           fbody_heap ? fbody_heap : fbody);
            if (fbody_heap) { free(fbody_heap); fbody_heap = NULL; }
            if (drc != JASH_FUNC_OK) {
                jash_writef(jash_stderr_writer, 0,
                            "%sjash: %s: %s — a funcao NAO foi definida%s\n",
                            J_ROLE_ERR, fname,
                            (drc == JASH_FUNC_ENOMEM) ? "sem memoria para o corpo"
                                                      : "tabela de funcoes cheia (32)",
                            J_RESET);
                s->last_status.code = 2;
                if (s && !s->interactive) s->exit_requested = 1;
                return 2;
            }
            s->last_status.code = 0;
            /* FIX: Process any remaining commands after the function def.
             * e.g. "greet() { echo hi; }; greet World" must also run "greet World".
             * The rest pointer points to the character after the closing '}'. */
            if (rest) {
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest == ';') rest++;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest) {
                    int rc = jash_eval_line(s, w, ctx, rest);
                    s->last_status.code = rc;
                    return rc;
                }
            }
            return 0;
        }
    }

    /* v0.8 FIX #1: Control structures BEFORE command list splitting
     * Check for/if/while/case FIRST so semicolons inside their bodies
     * are not mistaken for top-level command separators. */

    /* A6: { ... } — brace group (inline block) */
    /* JINV-084: o grupo pode vir INDENTADO. Um `{` de corpo de laço/função
     * chega aqui com os brancos do autor na frente (`  { echo a; }`), e o
     * teste `line[0]=='{'` o largava para o caminho de comando comum — onde
     * `{` vira o nome de um programa que não existe. */
    const char* bg = line;
    while (*bg == ' ' || *bg == '\t') bg++;
    if (bg[0] == '{' && (bg[1] == ' ' || bg[1] == '\t' || bg[1] == '\n')) {
        /* JINV-084 — o `}` que fecha, pela ÚNICA leitura da casa
         * (parser/script.h). A contagem que morava aqui era cega:
         *
         *   `{ echo "}"; }`      o `}` CITADO fechava o grupo cedo demais
         *   `{ echo "${V}"; }`   o `{` de `${` abria um grupo fantasma
         *   `{ echo }; }`        o `}` em posição de ARGUMENTO fechava
         *
         * — as três somem de graça ao usar `jash_find_brace_close`, que é
         * `jash_find_keyword_set` com o par `{`/`}`. */
        const char* p = jash_find_brace_close(bg + 1);
        if (p) {
            /* Check if there's a pipe after the closing brace, possibly
             * separated by whitespace AND redirect tokens (e.g. 2>&1).
             * BUG-055: only whitespace was skipped — `} 2>&1 | cmd`
             * saw '2' and decided "no pipe", running the brace group
             * inline in the parent shell and leaking stdout past the pipe. */
            const char* after = p + 1;
            while (*after == ' ' || *after == '\t') after++;
            const char* redir_start = after;  /* BUG-059: save for redirect parsing */
            /* Skip redirect tokens: [n]>[>]file, [n]<file, n>&m, &>file, etc. */
            while (*after) {
                const char* rs = after;
                while (*rs >= '0' && *rs <= '9') rs++;
                if (*rs == '>' || *rs == '<') {
                    after = rs;
                    if (*after == '&' && after[1] == '>') { after += 2; }
                    else if (*after == '>') {
                        after++; if (*after == '>' || *after == '|') after++;
                        if (*after == '&') { after++; if (*after == '-') { after++; } else while (*after >= '0' && *after <= '9') after++; }
                    } else if (*after == '<') {
                        after++; if (*after == '<' || *after == '>') after++;
                        if (*after == '&') { after++; while (*after >= '0' && *after <= '9') after++; }
                    }
                    /* skip filename / heredoc word */
                    while (*after && *after != ' ' && *after != '\t' &&
                           *after != '\n' && *after != '|' && *after != ';') after++;
                    while (*after == ' ' || *after == '\t') after++;
                    continue;
                }
                break;
            }
            if (*after == '|' && after[1] != '|') {
                /* Delegate to pipeline engine for pipe handling */
                int rc = jash_exec_pipeline(s, w, ctx, line);
                s->last_status.code = rc;
                return rc;
            }

            /* ═══ JINV-084 — a cauda do grupo tinha um dono que só sabia `;` ══
             *
             * O fim deste tratador roda o que vem depois do `}` — mas SÓ
             * quando o separador é `;` ou nova-linha. Com `&&`/`||` a cauda
             * era simplesmente DESCARTADA, em silêncio e com rc do grupo:
             *
             *     { echo a; } && echo b        bash -> a + b     jash -> a
             *     { false; } || echo b         bash -> b         jash -> (nada)
             *     { true; } && echo y || echo n   bash -> y      jash -> (nada)
             *
             * O conserto NÃO é ensinar `&&`/`||` a este tratador — seria a
             * segunda implementação de lista AND-OR da casa, e a empobrecida
             * (a terceira linha acima mostra por quê: uma cadeia `&& … || …` é
             * associativa à esquerda, não um par binário). O separador de topo
             * que já existe em `jash_eval_line` sabe fazer isso e JÁ conta
             * chaves (`brace_depth`, em find_top_separator), então basta NÃO
             * interceptar: o splitter parte em `{ … }` e `echo b`, e o primeiro
             * segmento volta aqui — agora sem cauda nenhuma para disputar.
             *
             * Um dono só, e é o que já existia. Mesma disciplina do JINV-074. */
            if ((after[0] == '&' && after[1] == '&') ||
                (after[0] == '|' && after[1] == '|'))
                goto skip_brace_group;

            /* JINV-084 — o corpo do grupo não pode ser truncado em silêncio.
             *
             * Era `char body[2048]` com clamp mudo (`if (blen >= sizeof(body))
             * blen = sizeof(body) - 1;`): um grupo maior que isso rodava com a
             * CAUDA APAGADA e rc 0 — o programa executado passava a ser outro e
             * ninguém era avisado. É o defeito do JINV-080/081 neste caminho, e
             * ele morde de verdade: os corpos `as_fn_*` do autoconf, que são
             * justamente grupos de chaves, passam folgado de 2 KB.
             *
             * Mesmo idioma dos irmãos: pilha no caso comum, heap acima disso,
             * e falta de memória é recusa ALTA — nunca truncamento. */
            unsigned blen = (unsigned)(p - bg - 1);
            char  body_inline[2048];
            char* body_heap = NULL;
            char* body = body_inline;
            if (blen >= sizeof(body_inline)) {
                body_heap = (char*)malloc((size_t)blen + 1);
                if (!body_heap) {
                    jash_writes(jash_stderr_writer, 0,
                        "jash: sem memoria para o corpo do grupo { } — "
                        "RECUSANDO (nada foi executado)\n");
                    s->last_status.code = 2;
                    return 2;
                }
                body = body_heap;
            }
            for (unsigned i = 0; i < blen; i++) body[i] = bg[i + 1];
            body[blen] = '\0';
            /* BUG-059: apply redirect tokens (e.g. `> file 2>&1`) around the body */
            int rc = 0;
            int saved_fds[8];
            int saved_tgts[8];
            int nsaved = 0;
            if (*redir_start && *redir_start != '|' && *redir_start != ';') {
                /* Parse and apply redirects from redir_start (handle ws correctly) */
                const char* rp = redir_start;
                while (*rp == ' ' || *rp == '\t') rp++;
                while (*rp && *rp != '\n' && *rp != '|' && *rp != ';') {
                    /* ═══ JINV-084 — LAÇO QUE NÃO ANDA = SHELL TRAVADO ═══════
                     *
                     * Nenhum ramo abaixo consome um byte que não seja `>` ou
                     * `<` (nem dígito, nem `&`, nem `\n`). Quando o que segue
                     * o `}` não é redirecionamento, `rp` fica IMÓVEL e a
                     * condição do `while` volta idêntica: laço infinito, shell
                     * pendurado para sempre, sem saída e sem rc.
                     *
                     * Gatilho medido (o mais banal que existe — grupo de
                     * chaves numa linha só, dentro de um laço multi-linha):
                     *
                     *     for i in 1 2; do
                     *     { echo v$i; }
                     *     done
                     *       bash -> v1 + v2      jash -> TRAVA (deadline 124)
                     *
                     * o byte parado é a nova-linha logo depois do `}`. As
                     * mesmas três linhas com `; ` no lugar da nova-linha
                     * sempre funcionaram — por isso ninguém tinha visto.
                     *
                     * A guarda é a do JINV-062 (avanço de ponteiro): lembra
                     * onde a volta começou e SAI se ela não consumiu nada.
                     * Vale para todo byte inesperado, hoje e depois — não só
                     * para a nova-linha que expôs o defeito. */
                    const char* rp_turn_start = rp;
                    while (*rp == ' ' || *rp == '\t') rp++;
                    if (!*rp || *rp == '|' || *rp == ';' || *rp == '&') break;
                    int tgt = 0;
                    const char* d = rp;
                    while (*d >= '0' && *d <= '9') d++;
                    if ((*d == '>' || *d == '<')) {
                        if (d > rp) {
                            for (const char* n = rp; n < d; n++) tgt = tgt * 10 + (*n - '0');
                            rp = d;
                        } else {
                            tgt = (*d == '<') ? 0 : 1;
                        }
                    }
                    int op = *rp;
                    if (op == '&' && rp[1] == '>') { tgt = 1; rp += 2; op = '>'; }
                    if (op == '>') {
                        rp++; int append = (*rp == '>'); if (append) rp++;
                        while (*rp == ' ' || *rp == '\t') rp++;
                        if (*rp == '&') {
                            rp++;
                            if (*rp == '-') { rp++;
                                for (int j = 0; j < nsaved; j++) if (saved_tgts[j] == tgt) goto skip_save_0;
                                { long h = fcntl(tgt, F_DUPFD_CLOEXEC, 10); saved_fds[nsaved] = (h < 0) ? -1 : (int)h; saved_tgts[nsaved] = tgt; nsaved++; }
                                skip_save_0: sys_close(tgt);
                            } else {
                                int sfd = 0; while (*rp >= '0' && *rp <= '9') { sfd = sfd * 10 + (*rp - '0'); rp++; }
                                for (int j = 0; j < nsaved; j++) if (saved_tgts[j] == tgt) goto skip_save_1;
                                { long h = fcntl(tgt, F_DUPFD_CLOEXEC, 10); saved_fds[nsaved] = (h < 0) ? -1 : (int)h; saved_tgts[nsaved] = tgt; nsaved++; }
                                skip_save_1: sys_dup2(sfd, tgt);
                            }
                        } else {
                            const char* fn = rp;
                            while (*rp && *rp != ' ' && *rp != '\t' &&
                                   *rp != '\n' && *rp != '|' && *rp != ';' && *rp != '&') rp++;
                            if (rp > fn) {
                                char fname[512]; int fi = 0;
                                for (const char* fp = fn; fp < rp && fi < 511; fi++) fname[fi] = *fp++;
                                fname[fi] = '\0';
                                for (int j = 0; j < nsaved; j++) if (saved_tgts[j] == tgt) goto skip_save_2;
                                { long h = fcntl(tgt, F_DUPFD_CLOEXEC, 10); saved_fds[nsaved] = (h < 0) ? -1 : (int)h; saved_tgts[nsaved] = tgt; nsaved++; }
                                skip_save_2:;
                                int flags = append ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
                                long fd = sys_open(fname, flags, 0666);
                                if (fd >= 0) { sys_dup2((int)fd, tgt); sys_close((int)fd); }
                            }
                        }
                    } else if (op == '<') {
                        rp++; if (*rp == '<' || *rp == '>') rp++;
                        while (*rp == ' ' || *rp == '\t') rp++;
                        const char* fn = rp;
                        while (*rp && *rp != ' ' && *rp != '\t' &&
                               *rp != '\n' && *rp != '|' && *rp != ';') rp++;
                        if (rp > fn) {
                            char fname[512]; int fi = 0;
                            for (const char* fp = fn; fp < rp && fi < 511; fi++) fname[fi] = *fp++;
                            fname[fi] = '\0';
                            for (int j = 0; j < nsaved; j++) if (saved_tgts[j] == tgt) goto skip_save_3;
                            { long h = fcntl(tgt, F_DUPFD_CLOEXEC, 10); saved_fds[nsaved] = (h < 0) ? -1 : (int)h; saved_tgts[nsaved] = tgt; nsaved++; }
                            skip_save_3:;
                            long fd = sys_open(fname, O_RDONLY, 0);
                            if (fd >= 0) { sys_dup2((int)fd, tgt); sys_close((int)fd); }
                        }
                    }
                    /* JINV-084: a volta não consumiu nada — o que segue o `}`
                     * não é redirecionamento. Sair é a única saída; continuar
                     * é o laço infinito descrito acima. */
                    if (rp == rp_turn_start) break;
                }
            }
            /* Execute body as a command list IN THE CURRENT SHELL (brace group
             * shares scope — assignments persist, unlike a `(..)` subshell). */
            rc = jash_eval_line(s, w, ctx, body);
            free(body_heap);   /* JINV-084 */
            s->last_status.code = rc;
            /* BUG-059: restore saved fds */
            for (int j = nsaved - 1; j >= 0; j--) {
                if (saved_fds[j] >= 0) { sys_dup2(saved_fds[j], saved_tgts[j]); sys_close(saved_fds[j]); }
                else { sys_close(saved_tgts[j]); }
            }
            /* BUG-011: run whatever follows the closing brace — `{ ...; }; cmd`
             * (and `{ ...; } && cmd`, etc.). Previously this text was dropped, so
             * `{ X=2; }; echo $X` never ran the echo. */
            if (s->return_requested || s->exit_requested) return rc;
            /* JINV-084: um break/continue em voo não pode ser atropelado pela
             * cauda — a mesma guarda que o `if` (JINV-075) e o `case` já têm.
             * Sem ela, `for i in 1 2; do { break; }; echo x; done` roda o
             * `echo x` DEPOIS do break e o rc 0 dele APAGA o pedido. */
            if (jash_loopctl_pending(s)) return rc;
            const char* rest = after;  /* points just past `}` (ws + redirs already skipped) */
            if (*rest == ';' || *rest == '\n') {
                rest++;
                while (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == ';') rest++;
                if (*rest) {
                    rc = jash_eval_line(s, w, ctx, rest);
                    s->last_status.code = rc;
                }
            }
            return rc;
        }
    }
skip_brace_group:   /* JINV-084: `{ … } && …` — quem parte a lista AND-OR é o
                     * separador de topo, não este tratador. */

    /* A6: if/elif/else/fi — multi-line block support */
    if (strncmp(line, "if ", 3) == 0 || strncmp(line, "if\t", 3) == 0 ||
        strncmp(line, "if\n", 3) == 0) {
        /* `if ... fi | cmd` — pipeline directly on the compound. Delegate
         * to the pipeline engine, which sets is_compound on segment 0 and
         * dup2s the pipe to stdout BEFORE invoking jash_eval_if_statement
         * in the forked child. Without this, the inline if-evaluator below
         * runs and prints to the parent's stdout, bypassing the pipe (and
         * `cat` on the other side gets EOF immediately → "execve failed"-
         * adjacent symptom: trailing seg with nothing to read). */
        const char* fi_pipe = strstr(line, "fi");
        while (fi_pipe) {
            /* require `fi` to stand as a token (preceded by ws/;/\n) */
            if (fi_pipe != line) {
                char prev = fi_pipe[-1];
                if (prev != ' ' && prev != '\t' && prev != ';' && prev != '\n') {
                    fi_pipe = strstr(fi_pipe + 2, "fi");
                    continue;
                }
            }
            char nxt = fi_pipe[2];
            if (nxt && nxt != ' ' && nxt != '\t' && nxt != ';' &&
                nxt != '\n' && nxt != '|' && nxt != '\0') {
                fi_pipe = strstr(fi_pipe + 2, "fi");
                continue;
            }
            const char* after = fi_pipe + 2;
            while (*after == ' ' || *after == '\t') after++;
            if (*after == '|' && after[1] != '|') {
                int rc = jash_exec_pipeline(s, w, ctx, line);
                s->last_status.code = rc;
                return rc;
            }
            fi_pipe = strstr(fi_pipe + 2, "fi");
        }

        /* ═══ JINV-074 — DOIS DONOS PARA A MESMA CAUDA ═══════════════════════
         *
         * Tudo depois do `fi` era executado DUAS vezes:
         *
         *     jash -c 'if false; then echo yes; fi; echo after'  ->  after
         *                                                            after
         *
         * porque ha dois donos rodando a mesma coisa. O tratador de bloco
         * executa por conta propria o texto depois do `fi` (script.c, no fim
         * de jash_eval_if_statement; if_block.c, no fim de jash_eval_if_block)
         * — e o chamador aqui executava DE NOVO via jash_block_tail (JINV-036).
         *
         * O sintoma nao e so a linha repetida. Como o incremento de um laco
         * tambem e "cauda", `while [ $i -lt 2 ]; do if ...; fi; i=$((i+1)); done`
         * incrementava 2x por volta: `i` pulava 0->2 e o laco saia na PRIMEIRA
         * volta. Foi diagnosticado por muito tempo como laco infinito; e o
         * oposto — laco que termina cedo demais.
         *
         * O precedente correto ja existia ao lado: `case`. O
         * jash_eval_case_stmt NAO roda a cauda; so o chamador roda. Um dono.
         *
         * O conserto aqui aplica essa disciplina ao `if` SEM tocar em
         * script.c/if_block.c: em vez de pedir aos tratadores que parem de
         * rodar a cauda, entrega-se a eles uma string que TERMINA no `fi`.
         * Nao ha cauda para eles rodarem — a propriedade passa a valer por
         * construcao, e nao por acordo entre arquivos. Se um dia o codigo
         * interno deles for removido, nada aqui muda de comportamento: eles ja
         * recebem um bloco sem cauda hoje.
         *
         * Se a copia do bloco nao couber e o malloc falhar, `tail` e zerado e
         * o tratador recebe a linha inteira — exatamente o comportamento
         * antigo, uma execucao pelo lado dele. Degradar para "uma vez" nunca
         * degrada para "duas vezes".
         *
         * Gate: tests/test_jinv074_if_tail_once.sh                       */
        int rc = -1;
        size_t   blk_len  = 0;
        const char* tail  = jash_block_split(line, "if", "fi", &blk_len);
        char  blk_stack[512];
        char* blk_heap    = 0;
        const char* blk   = line;
        if (tail && blk_len > 0) {
            if (blk_len < sizeof(blk_stack)) {
                memcpy(blk_stack, line, blk_len);
                blk_stack[blk_len] = '\0';
                blk = blk_stack;
            } else {
                blk_heap = (char*)malloc(blk_len + 1);
                if (blk_heap) {
                    memcpy(blk_heap, line, blk_len);
                    blk_heap[blk_len] = '\0';
                    blk = blk_heap;
                } else {
                    tail = 0;   /* sem memoria: cai no comportamento antigo */
                }
            }
        }
        /* A escolha de tratador segue sendo feita sobre a LINHA INTEIRA — o
         * mesmo predicado de antes, para que este conserto nao mude qual
         * parser atende cada forma. */
        if (!strstr(line, "elif") && !strchr(line, '\n') && !strstr(line + 2, " if ")) {
            rc = jash_eval_if_statement(s, w, ctx, blk);
            if (rc >= 0) {
                free(blk_heap);
                s->last_status.code = rc;
                /* JINV-036 (a cauda roda) + JINV-075 (mas nao por cima de um
                 * break/continue em voo — a mesma guarda que o irmao
                 * if_block.c ja tinha e os outros dois nao). */
                if (!s->return_requested && !s->exit_requested &&
                    !jash_loopctl_pending(s)) {
                    if (tail) return jash_eval_line(s, w, ctx, tail);
                }
                return rc;
            }
        }
        rc = jash_eval_if_block(s, w, ctx, blk);
        if (rc >= 0) {
            free(blk_heap);
            s->last_status.code = rc;
            /* JINV-036 + JINV-075 */
            if (!s->return_requested && !s->exit_requested &&
                !jash_loopctl_pending(s)) {
                if (tail) return jash_eval_line(s, w, ctx, tail);
            }
            return rc;
        }
        free(blk_heap);
        /* Fall back to single-line if parser (linha INTEIRA: aqui os dois
         * tratadores de bloco recusaram, entao nao ha bloco reconhecido para
         * recortar e o comportamento historico e o unico seguro). */
        rc = jash_eval_if_statement(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            return rc;
        }
    }

    /* A6: for VAR in WORDS; do BODY; done */
    if (strncmp(line, "for ", 4) == 0 || strncmp(line, "for\t", 4) == 0 ||
        strncmp(line, "for(", 4) == 0) {   /* BUG-018: `for((` w/o space (bash parity) */
        if (jash_loop_is_piped(line)) {   /* JINV-083 */
            int rc = jash_exec_pipeline(s, w, ctx, line);
            s->last_status.code = rc;
            return rc;
        }
        /* Check for for (( )) style first */
        const char* fp = line + 3;
        while (*fp == ' ' || *fp == '\t') fp++;
        if (fp[0] == '(' && fp[1] == '(') {
            int rc = jash_eval_for_c_style(s, w, ctx, line);
            if (rc >= 0) {
                s->last_status.code = rc;
                return rc;
            }
        }
        int rc = jash_eval_for_loop(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            return rc;
        }
        /* If parsing fails, fall through to normal command handling. */
    }

    /* A6: while COND; do BODY; done */
    if (strncmp(line, "while ", 6) == 0 || strncmp(line, "while\t", 6) == 0 ||
        strncmp(line, "while(", 6) == 0) {  /* BUG-018: `while((` w/o space */
        if (jash_loop_is_piped(line)) {   /* JINV-083 */
            int rc = jash_exec_pipeline(s, w, ctx, line);
            s->last_status.code = rc;
            return rc;
        }
        int rc = jash_eval_while_loop(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            return rc;
        }
        /* If parsing fails, fall through to normal command handling. */
    }

    /* Phase 6.3: until COND; do BODY; done — inverted while loop */
    if (strncmp(line, "until ", 6) == 0 || strncmp(line, "until\t", 6) == 0) {
        if (jash_loop_is_piped(line)) {   /* JINV-083 */
            int rc = jash_exec_pipeline(s, w, ctx, line);
            s->last_status.code = rc;
            return rc;
        }
        int rc = jash_eval_until_loop(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            return rc;
        }
        /* If parsing fails, fall through to normal command handling. */
    }

    /* A6: case EXPR in PATTERN) CMD ;; esac */
    if (strncmp(line, "case ", 5) == 0 || strncmp(line, "case\t", 5) == 0) {
        int rc = jash_eval_case_stmt(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            /* JINV-036: o que vem depois de `esac` tambem executa.
             *
             * JINV-075: …MENOS quando um break/continue esta subindo. A guarda
             * existia no irmao (if_block.c) e faltava aqui e no for_loop.c —
             * mesma disciplina aplicada num lugar e nao nos outros dois. Sem
             * ela, `for i in 1 2; do case $i in 2) break;; esac; echo x; done`
             * roda o `echo x` depois do `break`, e o rc 0 dele APAGA o pedido:
             * o laco continua como se nada tivesse sido pedido. */
            if (!s->return_requested && !s->exit_requested &&
                !jash_loopctl_pending(s)) {
                const char* tail = jash_block_tail(line, "case", "esac");
                if (tail) return jash_eval_line(s, w, ctx, tail);
            }
            return rc;
        }
        /* If parsing fails, fall through to normal command handling. */
    }

    /* A6: select VAR in WORDS; do BODY; done */
    if (strncmp(line, "select ", 7) == 0 || strncmp(line, "select\t", 7) == 0) {
        int rc = jash_eval_select_loop(s, w, ctx, line);
        if (rc >= 0) {
            s->last_status.code = rc;
            return rc;
        }
        /* If parsing fails, fall through to normal command handling. */
    }

    /* v0.6 FIX #2: command lists with ;, &&, ||
     * Walk left-to-right, splitting on top-level separators.
     * Maintain proper short-circuit semantics where chains like
     *   false && X || Y
     * evaluate Y because the && branch's failure leaves the overall
     * status as failure, which the || then handles. */
    {
        int sep_kind = 0, sep_skip = 0;
        const char* first_nl = strchr(line, '\n');
        const char* sep = find_top_separator_impl(line, first_nl, &sep_kind, &sep_skip);
        if (sep) {
            const char* cursor = line;
            int last_rc = 0;
            int have_run = 0;
            int pending_kind = -1; /* separator type that decides next segment */
            int kind = sep_kind, skip = sep_skip;
            const char* nxt = sep;

            while (1) {
                /* Extract segment [cursor .. nxt) (or to EOL if nxt==NULL) */
                unsigned slen = nxt ? (unsigned)(nxt - cursor) : 0;
                if (!nxt) {
                    /* last segment: rest of line */
                    const char* q = cursor;
                    while (*q) q++;
                    slen = (unsigned)(q - cursor);
                }
                /* trim trailing ws — feito logo apos a copia para `seg`, onde
                 * o texto ja e uma string terminada em NUL (JINV-063). */

                /* JINV-021: este buffer e de PILHA (o avaliador reentra), entao
                 * nao da para simplesmente elevar o teto sem arriscar estouro.
                 * Mas cortar em silencio era pior: executava-se um comando
                 * TRUNCADO — um `rm -rf /caminho/muito/longo/...` cortado no
                 * meio nao e o comando que o usuario escreveu, e um `"` ou `)`
                 * perdido no corte muda o significado do que sobra.
                 *
                 * Agora RECUSA alto e nao executa nada. Perder o comando e
                 * recuperavel; executar outro comando nao. O teto real fica
                 * declarado aqui e medido pelo gate. */
                /* JINV-039 — 16 KB fora da PILHA do ciclo de recursao.
                 *
                 * Uma recursao de funcao custa ~300 KB de pilha por nivel
                 * (medido; ver o comentario em JASH_EVAL_MAX_DEPTH), e o teto
                 * de 32 chamadas e o piso de seguranca calibrado nesse custo.
                 * Cada buffer grande que sai da pilha e teto que pode subir
                 * depois — a divida que o proprio codigo ja anotava.
                 *
                 * Usa o mecanismo que JA existe (eval_depth_buf, acima):
                 * um slot de heap por profundidade, alocado sob demanda e
                 * reusado pela vida do processo. Mesma capacidade, mesmo
                 * comportamento de estouro — so muda ONDE mora. */
                static char* seg_slots[JASH_EVAL_MAX_DEPTH];
                enum { JEVAL_SEG_CAP = 16384 };
                char* seg = eval_depth_buf(seg_slots, JEVAL_SEG_CAP);
                if (!seg) { s->last_status.code = 1; return 1; }
                if (slen >= JEVAL_SEG_CAP) {
                    fprintf(stderr,
                            "jash: comando de %u bytes excede o teto de %d "
                            "deste estagio — recusando (nada foi executado)\n",
                            slen, JEVAL_SEG_CAP - 1);
                    slen = 0;                    /* `if (run && slen > 0)` abaixo */
                }
                for (unsigned i = 0; i < slen; i++) seg[i] = cursor[i];
                seg[slen] = '\0';
                /* JINV-063: o separador do fim do SEGMENTO some, o dado fica.
                 * Era um rtrim cego aqui — `echo x\ ; echo y` perdia o espaco
                 * ESCAPADO e imprimia `x\`. Mesma funcao das outras portas. */
                slen = (unsigned)jash_rtrim_unquoted(seg);
                seg[slen] = '\0';

                if (nxt && kind == 3 && slen > 0 && slen + 2 < JEVAL_SEG_CAP) {
                    seg[slen++] = ' ';
                    seg[slen++] = '&';
                    seg[slen] = '\0';
                }
                /* If slen==0 here, the `&` was leading (left from an upstream
                 * subshell handler that already executed the (cmd) and passed
                 * us "& rest") — don't synthesize " &" as a command. */

                /* Decide whether to run this segment based on pending_kind + last_rc */
                int run = 1;
                if (have_run) {
                    if (pending_kind == 1) run = (last_rc == 0); /* && */
                    else if (pending_kind == 2) run = (last_rc != 0); /* || */
                    /* ; always runs (run stays 1) */
                }

                if (run && slen > 0) {
                    /* POSIX `! cmd` negation: strip leading `!` and invert
                     * this segment's exit status. Idiom `if ! grep -q X`. */
                    int negate = 0;
                    {
                        const char* sp = seg;
                        while (*sp == ' ' || *sp == '\t') sp++;
                        if (sp[0] == '!' && (sp[1] == ' ' || sp[1] == '\t' ||
                                              sp[1] == '\0')) {
                            negate = 1;
                            unsigned shift = (unsigned)((sp - seg) + 1);
                            for (unsigned i = 0; i + shift < slen; i++)
                                seg[i] = seg[i + shift];
                            slen -= shift;
                            seg[slen] = '\0';
                            /* JINV-066: um pipeline aceita UM `!`. O segundo
                             * e erro de sintaxe, nao "nega duas vezes" — a
                             * mesma regra do caminho de pipeline unico,
                             * decidida pelo mesmo predicado. */
                            if (jash_bang_negation(seg)) {
                                jash_writes(jash_stderr_writer, 0,
                                    "jash: syntax error near unexpected token `!'\n");
                                s->last_status.code = 2;
                                return 2;
                            }
                        }
                    }
                    last_rc = (slen > 0)
                              ? jash_eval_line(s, w, ctx, seg)
                              : 0;
                    if (negate) last_rc = last_rc ? 0 : 1;
                    s->last_status.code = last_rc;
                    have_run = 1;
                    /* A command-substitution overflow is a parser-level
                     * refusal, not an ordinary command failure for `||` or
                     * `;` to recover from. Nothing else on the submitted
                     * command line may run after the expansion was refused. */
                    if (jash_cmdsub_error() || jash_glob_error())
                        return last_rc ? last_rc : 1;
                    /* ADR-0033 Wave 2: feed each executed segment to the brain.
                     * PERF: gate BEFORE getcwd() — this splitter runs for every
                     * `;`-segment at every depth (loop bodies!), and the
                     * unconditional getcwd() was ~2/3 of tight-loop runtime
                     * with the brain off. F E R M I ∞ H A R T */
#ifndef JASH_AGENT_ONLY
                    if (slen > 0 && jash_liquid_should_hook()) {
                        char cwd[512];
                        const char* cwdptr = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
                        jash_liquid_post_cmd(seg, last_rc, 0, 0, cwdptr, w, ctx);
                    }
#endif
                    /* `exit N` mid-list: stop the remaining segments. The
                     * exit builtin sets s->exit_requested and last_status;
                     * the outer driver (main -c / REPL / script) checks
                     * exit_requested when we return. Without this, `echo a;
                     * exit 3; echo b` would run echo b and return 0. */
                    if (s->exit_requested) return s->last_status.code;
                    /* BUG-012: `return` inside a function (incl. as the RHS of
                     * `&&`/`||`, which re-enters this same list executor) stops
                     * the list and bubbles up; the function executor clears it. */
                    if (s->return_requested) return s->last_status.code;
                    /* Propagate break/continue codes immediately.
                     *
                     * JINV-073: este `break` era a linha que derrubava o shell.
                     * Ele olhava SO para o rc — e 255/254 sao codigos de saida
                     * legitimos de qualquer processo. `(exit 255); echo x`,
                     * `sh -c "exit 255"; echo x` e `f(){ return 255; }; f; echo x`
                     * abandonavam o resto da lista em silencio e devolviam 255.
                     * `ssh` sai com 255 quando a conexao falha: bastava uma
                     * linha `ssh host cmd; echo depois` para o `echo` sumir.
                     *
                     * Agora exige o rc E a marca fora de banda posta pelos
                     * builtins `break`/`continue` (state.h). */
                    if (jash_loopctl_is_ctl(s, last_rc)) break;
                    /* set -e (errexit): abort the list when a command fails,
                     * UNLESS this segment is the left operand of a following
                     * && / || (kind 1/2) — bash exempts those from errexit
                     * because their status is being tested. F E R M I ∞ H A R T */
                    if (s->shell_opts.errexit && last_rc != 0 &&
                        !(nxt && (kind == 1 || kind == 2))) {
                        return last_rc;
                    }
                } else if (!have_run) {
                    /* first segment empty (e.g. leading ';') — just mark we
                     * "ran" with rc 0 so chaining works */
                    have_run = 1;
                    last_rc = 0;
                }
                /* If run==0 (short-circuit), last_rc stays as-is, which is
                 * correct because the skipped branch's status is the chain's
                 * cached failure/success, allowing subsequent || or && to
                 * fire correctly. */

                if (!nxt) break;
                pending_kind = kind;
                cursor = nxt + skip;
                cursor = skip_ws(cursor);
                if (first_nl && first_nl < cursor) first_nl = strchr(cursor, '\n');
                nxt = find_top_separator_impl(cursor, first_nl, &kind, &skip);
            }
            /* Process any pending traps after command list execution.
             * Critical for: trap "echo caught" INT; kill -INT $$
             * The signal arrives during the last command, but without
             * this check, no subsequent eval_line processes it. */
            jash_run_pending_traps(s, w, ctx);
            return last_rc;
        }
    }

    /* JINV-066 (segundo defeito) — `! cmd` como PREFIXO de pipeline.
     *
     * A negacao de pipeline ja existia, mas SO dentro do divisor de
     * `;`/`&&`/`||` logo acima — e aquele bloco so roda quando ha separador.
     * Uma linha com um comando so nunca passava por la, entao `! true` caia
     * na busca de comando com o nome `!`, que nao existe: rc 127.
     *
     * O 127 e pior que "nao inverteu". `!` e o idioma POSIX de negar
     * (`until ! ping -c1 host`, `if ! grep -q X f`), e 127 e nao-zero — ou
     * seja, um `until ! cond` girava para SEMPRE porque a condicao nunca
     * "ficava verdadeira", e um `if ! cmd` entrava no ramo errado calado.
     *
     * Aqui e o ponto certo: os comandos compostos (if/while/for/case/until/
     * select) e o divisor de listas ja foram tratados acima, e o motor de
     * pipeline vem depois — entao um `!` visto neste ponto nega exatamente
     * UM pipeline, que e o que o POSIX manda (`! a | b` nega `a | b`).
     *
     * Exige espaco/tabulacao depois do `!`: `!cmd` colado e o override do
     * loopguard (ADR-0042 F3), outro operador, e continua intacto.
     * Gate: tests/test_jinv066_test_negation.sh */
    if (line[0] == '!' && (line[1] == ' ' || line[1] == '\t')) {
        const char* negated = skip_ws(line + 1);
        /* Um pipeline aceita UM `!` e so um (POSIX: `pipeline: Bang
         * pipe_sequence`). Aceitar o segundo calado seria dado virando
         * comando de novo, na direcao oposta: `! ! rm -rf x` rodaria o rm
         * com a negacao que o autor achou que tinha escrito. O bash recusa
         * com erro de sintaxe; aqui tambem. `! !cmd` (colado) NAO e isto —
         * ali o `!cmd` e nome de comando, e o bash concorda. */
        if (jash_bang_negation(negated)) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: syntax error near unexpected token `!'\n");
            s->last_status.code = 2;
            return 2;
        }
        if (*negated) {
            int nrc = jash_eval_line(s, w, ctx, negated);
            nrc = nrc ? 0 : 1;
            s->last_status.code = nrc;
            return nrc;
        }
    }

    /* FIRST: Check if this line starts with a variable assignment (VAR=value).
     * If it does, treat the entire remaining line as the value (allowing
     * spaces inside $(...) and quoted strings). After eval, no recursion
     * needed — the top-level separator splitter above already handled
     * command lists like "X=10; echo $X". */
    {
        /* Scan a leading identifier */
        const char* p = line;
        if (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) {
            const char* idstart = p;
            while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                          (*p >= '0' && *p <= '9') || *p == '_')) p++;
            if (*p == '[') {
                const char* bstart = p;
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
                if (*p == '=' || (p[0] == '+' && p[1] == '=')) {
                    int append = (p[0] == '+');
                    
                    /* Stack buffers for array assignment scope */
                    char arr_assign_unq[JASH_ENV_VAL_LEN];
                    char arr_assign_catbuf[JASH_ENV_VAL_LEN];
                    
                    const char* eq_ptr = append ? p + 1 : p;
                    unsigned vname_len = (unsigned)(p - idstart);
                    char vname[128];
                    if (vname_len >= sizeof(vname)) vname_len = sizeof(vname) - 1;
                    for (unsigned i = 0; i < vname_len; i++) vname[i] = idstart[i];
                    vname[vname_len] = '\0';

                    /* BUG-072b: evaluate an arithmetic/variable subscript
                     * (a[i], a[1+1]) to a literal index before storage — the
                     * env keys on name[N] with a numeric N, so a bare `i`
                     * subscript was silently stored under the literal key
                     * "a[i]" and never retrievable via ${a[5]}. */
                    {
                        char* lb = NULL;
                        for (char* q = vname; *q; q++) if (*q == '[') { lb = q; break; }
                        char* rb = NULL;
                        if (lb) for (char* q = lb + 1; *q; q++) if (*q == ']') rb = q;
                        if (lb && rb && rb > lb + 1) {
                            char subx[64];
                            size_t sl = (size_t)(rb - lb - 1);
                            if (sl < sizeof(subx)) {
                                memcpy(subx, lb + 1, sl); subx[sl] = '\0';
                                /* Eval to a numeric index for INDEXED arrays;
                                 * keep literal for ASSOCIATIVE string keys.
                                 * jash tracks no array type, so mirror how
                                 * ${a[..]} retrieval disambiguates: pure number
                                 * → index; has operators → arithmetic expr →
                                 * eval; bare identifier → index iff it is a SET
                                 * variable, else an associative key (literal).
                                 * This fixes a[i]=v / a[1+1]=m without breaking
                                 * declare -A m; m[foo]=1. */
                                int all_digit = subx[0] != '\0', ident = subx[0] != '\0';
                                for (char* q = subx; *q; q++) {
                                    if (*q < '0' || *q > '9') all_digit = 0;
                                    if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||
                                          (*q>='0'&&*q<='9')||*q=='_')) ident = 0;
                                }
                                int do_eval = all_digit ? 0
                                            : !ident      ? 1
                                            : (jash_env_get(s, subx) != NULL);
                                if (do_eval) {
                                    long ix = jash_eval_arithmetic(subx, s);
                                    if (ix < 0) ix = 0;
                                    char nb[sizeof vname + 32];
                                    *lb = '\0';
                                    snprintf(nb, sizeof(nb), "%s[%ld]", vname, ix);
                                    unsigned k = 0;
                                    while (nb[k] && k + 1 < sizeof(vname)) { vname[k] = nb[k]; k++; }
                                    vname[k] = '\0';
                                }
                            }
                        }
                    }

                    const char* raw_val = eq_ptr + 1;
                    /* BUG-072a: the VALUE ends at the first UNQUOTED whitespace
                     * (quote / $() / ${} / backtick aware). Anything after is a
                     * COMMAND — i.e. `a[i]=v cmd` is a PREFIX assignment, not a
                     * standalone one. The old code grabbed the whole rest of the
                     * line, so `a[0]=hi echo test` silently swallowed the
                     * command (and lost its exit status). */
                    const char* vend = raw_val;
                    {
                        char q = 0; int pd = 0, bk = 0, cm = 0; char pv = '=';
                        while (*vend) {
                            char c = *vend;
                            /* BUG-121a: a #-comment inside a multi-line $()
                             * is inert — a `)` in it must not close pd. */
                            if (cm) { if (c == '\n') cm = 0; pv = c; vend++; continue; }
                            if (q)  { if (c == q) q = 0; pv = c; vend++; continue; }
                            if (bk) { if (c == '`') bk = 0; pv = c; vend++; continue; }
                            if (c == '\\' && vend[1]) { pv = vend[1]; vend += 2; continue; }
                            if (c == '\'' || c == '"') { q = c; pv = c; vend++; continue; }
                            if (c == '`') { bk = 1; pv = c; vend++; continue; }
                            if (c == '$' && (vend[1] == '(' || vend[1] == '{')) { pd++; pv = vend[1]; vend += 2; continue; }
                            if (pd > 0) {
                                if (c == '#' && (pv == ' ' || pv == '\t' || pv == '\n' ||
                                                 pv == ';' || pv == '(' || pv == '|' ||
                                                 pv == '&')) { cm = 1; vend++; continue; }
                                if (c == '(' || c == '{') pd++;
                                else if (c == ')' || c == '}') pd--;
                                pv = c; vend++; continue;
                            }
                            if (c == ' ' || c == '\t') break;
                            pv = c; vend++;
                        }
                    }
                    const char* cmd_after = vend;
                    while (*cmd_after == ' ' || *cmd_after == '\t') cmd_after++;
                    int has_trailing_cmd = (*cmd_after != '\0');
                    /* JINV-086: o gêmeo do corte mudo em 1023 bytes, no
                     * elemento de array (`a[i]=…`). Mesmo conserto do sítio
                     * escalar: 1024 B de pilha viram limiar de desempenho,
                     * heap exato acima, porta única de saída
                     * (`arr_assign_done`) e recusa ALTA sem memória. */
                    char  vbuf_inline[1024];
                    char* vbuf = vbuf_inline;
                    char* vbuf_heap = NULL;
                    char* unq_heap  = NULL;
                    int   assign_rc = 0;
                    unsigned vlen = (unsigned)(vend - raw_val);
                    if (vlen >= sizeof(vbuf_inline)) {
                        vbuf_heap = (char*)malloc((size_t)vlen + 1);
                        if (!vbuf_heap) {
                            jash_writef(jash_stderr_writer, 0,
                                        "%sjash: %s: sem memoria para o valor — a atribuicao NAO foi feita%s\n",
                                        J_ROLE_ERR, vname, J_RESET);
                            if (!s->interactive) s->exit_requested = 1;
                            s->last_status.code = 2;
                            return 2;
                        }
                        vbuf = vbuf_heap;
                    }
                    memcpy(vbuf, raw_val, vlen);
                    vbuf[vlen] = '\0';

                    const char* val = vbuf;
                    /* ADR-0036 W4: depth-indexed heap slot, not a stack-local
                     * ctx — this function recurses up to JASH_EVAL_MAX_DEPTH,
                     * and `val` outlives this block, so a per-call stack ctx
                     * here would cost up to ~20MB of stack at max recursion. */
                    { jash_expand_ctx_t* ectx = eval_expand_ctx_slot();
                      if (ectx) { const char* _a = jash_expand_ansi_c(val, ectx); if (_a) val = _a;
                                  /* JINV-041 */
                                  { const char* _t = jash_expand_tilde_value(s, val, ectx); if (_t) val = _t; } } }
                    if (jash_has_cmdsub(val)) {
                        /* BUG-031: assignment RHS is NOT field-split (POSIX), so
                         * `VAR=$(...)` must keep embedded newlines/spaces. */
                        extern int g_jash_cmdsub_no_split;
                        int _ns = g_jash_cmdsub_no_split; g_jash_cmdsub_no_split = 1;
                        const char* x = jash_expand_cmdsub_known(s, val);
                        g_jash_cmdsub_no_split = _ns;
                        if (x) val = x;
                        if (jash_cmdsub_error()) {
                            jash_writes(jash_stderr_writer, 0, jash_cmdsub_error_msg());
                            jash_writes(jash_stderr_writer, 0, "\n");
                            assign_rc = 1;
                            goto arr_assign_done;
                        }
                    }
                    if (jash_has_arithmetic(val)) {
                        jash_arith_clear_error();
                        const char* x = jash_expand_arithmetic_known(val, s);
                        if (x) val = x;
                        if (jash_arith_error()) {   /* BUG-126: div/mod by 0 */
                            jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
                            assign_rc = 1;              /* JINV-086 */
                            goto arr_assign_done;
                        }
                    }
                    if (jash_has_param_expansion(val)) {
                        const char* x = jash_param_expand(val, s);
                        if (x) val = x;
                    }
                    if (jash_param_expand_error()) {
                        jash_writes(jash_stderr_writer, 0, jash_param_expand_error_msg());
                        jash_writes(jash_stderr_writer, 0, "\n");
                        if (!s->interactive) s->exit_requested = 1;
                        assign_rc = 1;                  /* JINV-086 */
                        goto arr_assign_done;
                    }
                    jash_cmdsub_unprotect((char*)val);  /* SECURITY #15 */

                    /* POSIX quote removal (see the scalar-assignment site for
                     * the full rationale — BUG-059). */
                    {
                        extern int jash_unquote(const char* in, char* out, size_t out_size);
                        extern int jash_has_quotes(const char* str);
                        if (jash_has_quotes(val) || strchr(val, '\\')) {
                            /* JINV-086: acima de JASH_ENV_VAL_LEN o unquote
                             * recusava (JINV-027) e o valor ficava COM aspas —
                             * `a[0]="<9000 a's>"` media 9002. */
                            char*  ub = arr_assign_unq;
                            size_t uc = sizeof(arr_assign_unq);
                            size_t need = strlen(val) + 1;
                            if (need > uc) {
                                unq_heap = (char*)malloc(need);
                                if (!unq_heap) {
                                    jash_writef(jash_stderr_writer, 0,
                                                "%sjash: %s: sem memoria para remover aspas — a atribuicao NAO foi feita%s\n",
                                                J_ROLE_ERR, vname, J_RESET);
                                    if (!s->interactive) s->exit_requested = 1;
                                    assign_rc = 2;
                                    goto arr_assign_done;
                                }
                                ub = unq_heap; uc = need;
                            }
                            if (jash_unquote(val, ub, uc) >= 0) val = ub;
                        }
                    }
                    /* BUG-116a: unshield AFTER quote removal (see scalar site). */
                    jash_quote_unshield((char*)val);    /* BUG-096/116a */

                    /* +=: append to current array element value */
                    if (append) {
                        const char* cur = jash_env_get(s, vname);
                        if (cur) {
                            unsigned ci = 0;
                            while (cur[ci] && ci + 1 < sizeof(arr_assign_catbuf)) { arr_assign_catbuf[ci] = cur[ci]; ci++; }
                            const char* v = val;
                            while (*v && ci + 1 < sizeof(arr_assign_catbuf)) { arr_assign_catbuf[ci++] = *v++; }
                            arr_assign_catbuf[ci] = '\0';
                            jash_env_set(s, vname, arr_assign_catbuf);
                        } else {
                            jash_env_set(s, vname, val);
                        }
                    } else {
                        jash_env_set(s, vname, val);
                    }
                    /* BUG-072a: prefix form `a[i]=v cmd` — run the trailing
                     * command; its exit status is the line's result. (bash does
                     * not persist a prefix array assignment; jash keeps it — a
                     * minor, documented divergence in the rare prefix case.)
                     *
                     * JINV-086: o comando de cauda roda DEPOIS de liberar os
                     * buffers — ele reentra em `jash_eval_line` e pode ser um
                     * laço inteiro; segurar o heap durante a recursão o
                     * multiplicaria por nível. */
                    assign_rc = 0;
arr_assign_done:                     /* JINV-086: porta única de saída */
                    if (vbuf_heap) free(vbuf_heap);
                    if (unq_heap)  free(unq_heap);
                    if (assign_rc == 0 && has_trailing_cmd) {
                        s->last_status.code = 0;
                        return jash_eval_line(s, w, ctx, cmd_after);
                    }
                    s->last_status.code = assign_rc;
                    return assign_rc;
                }
                p = bstart;
            }
            /* Compound array assignment: name=(val1 val2 val3 ...)
             * Also handles explicit indices: name=([0]=a [2]=c)
             * Also handles name+=(val...) for append. */
            if ((*p == '=' || (p[0] == '+' && p[1] == '=')) &&
                (p[0] == '+' ? p[2] == '(' : p[1] == '(')) {
                int arr_append = (p[0] == '+');
                const char* paren_open = arr_append ? p + 2 : p + 1;
                /* Find matching closing paren */
                const char* paren_close = NULL;
                int depth = 1;
                const char* scan = paren_open + 1;
                int in_sq = 0, in_dq = 0;
                while (*scan) {
                    /* BUG-116c: \" inside "…" is a literal quote — without
                     * skipping the escape, the quote state desynced and the
                     * closing paren was never found (a=("x\"y") silently
                     * produced an EMPTY array). */
                    if (!in_sq && *scan == '\\' && scan[1]) { scan += 2; continue; }
                    if (*scan == '\'' && !in_dq) in_sq = !in_sq;
                    else if (*scan == '"' && !in_sq) in_dq = !in_dq;
                    else if (!in_sq && !in_dq) {
                        if (*scan == '(') depth++;
                        else if (*scan == ')') { depth--; if (depth == 0) { paren_close = scan; break; } }
                    }
                    scan++;
                }
                if (paren_close) {
                    unsigned vname_len = (unsigned)(p - idstart);
                    char vname[128];
                    if (vname_len >= sizeof(vname)) vname_len = sizeof(vname) - 1;
                    for (unsigned i = 0; i < vname_len; i++) vname[i] = idstart[i];
                    vname[vname_len] = '\0';

                    eval_env_snapshot_t old_env = {0};
                    if (eval_env_snapshot(s, &old_env) != 0) {
                        jash_writes(jash_stderr_writer, 0,
                                    "jash: array: unable to snapshot old value — refusing\n");
                        s->last_status.code = 1;
                        return 1;
                    }
#define ARRAY_SET_OR_ROLLBACK(key_, value_) do { \
    if (jash_env_set(s, (key_), (value_)) != 0) { \
        jash_writes(jash_stderr_writer, 0, \
                    "jash: array element storage exhausted — rolling back\n"); \
        s->last_status.code = 1; \
        eval_env_rollback(s, &old_env); \
        return 1; \
    } \
} while (0)
#define ARRAY_APPEND_OR_ROLLBACK(key_, value_) do { \
    if (jash_env_append(s, (key_), (value_)) != 0) { \
        jash_writes(jash_stderr_writer, 0, \
                    "jash: array append storage exhausted — rolling back\n"); \
        s->last_status.code = 1; \
        eval_env_rollback(s, &old_env); \
        return 1; \
    } \
} while (0)

                    /* Determine starting index (for += append, find current max + 1) */
                    int next_idx = 0;
                    if (arr_append) {
                        /* Find highest existing index */
                        uint32_t n = s ? jash_env_count(s) : 0;
                        for (uint32_t i = 0; i < n; i++) {
                            const char *k = 0, *v2 = 0;
                            if (!jash_env_at(s, i, &k, &v2)) continue;
                            size_t kl = strlen(k);
                            if (kl > vname_len + 2 && strncmp(k, vname, vname_len) == 0 &&
                                k[vname_len] == '[' && k[kl - 1] == ']') {
                                const char* idx_s = k + vname_len + 1;
                                long val2 = 0; int digits = 0;
                                while (idx_s < k + kl - 1 && *idx_s >= '0' && *idx_s <= '9') {
                                    val2 = val2 * 10 + (*idx_s - '0'); idx_s++; digits = 1;
                                }
                                if (digits && idx_s == k + kl - 1 && (int)val2 >= next_idx)
                                    next_idx = (int)val2 + 1;
                            }
                        }
                    } else {
                        /* BUG-105: `a=(new…)` REPLACES the whole array (bash) —
                         * clear every existing element key first, or stale
                         * elements survive: a=(9 8 7); a=(1 2) left a[2]=7. */
                        eval_array_remove(s, vname);
                    }

                    /* BUG-147: brace-expand the array-literal body so
                     * name=({a,b}) yields elements a and b. Quotes are honored
                     * by jash_expand_braces (name=("{a,b}") stays one literal
                     * element), and non-list braces (${x}, [i]=v) are left
                     * alone. Rebuild "(<expanded>)" in a persistent buffer and
                     * repoint paren_open/paren_close so the delicate element
                     * parser below runs unchanged. */
                    char arr_reparen[4096];
                    {
                        size_t clen = (size_t)(paren_close - (paren_open + 1));
                        if (clen > 0 && clen < sizeof(arr_reparen) - 2 &&
                            memchr(paren_open + 1, '{', clen)) {
                            char tmp[4096];
                            memcpy(tmp, paren_open + 1, clen); tmp[clen] = 0;
                            jash_expand_ctx_t* bctx = eval_expand_ctx_slot();
                            if (bctx) {
                                const char* bx = jash_expand_braces(tmp, bctx);
                                size_t bl = bx ? strlen(bx) : 0;
                                if (bx && bl && bl < sizeof(arr_reparen) - 2 &&
                                    strcmp(bx, tmp) != 0) {
                                    arr_reparen[0] = '(';
                                    memcpy(arr_reparen + 1, bx, bl);
                                    arr_reparen[1 + bl] = ')';
                                    arr_reparen[2 + bl] = 0;
                                    paren_open  = arr_reparen;
                                    paren_close = arr_reparen + 1 + bl;
                                }
                            }
                        }
                    }

                    /* Parse elements between parens */
                    const char* ep = paren_open + 1;
                    while (ep < paren_close) {
                        while (ep < paren_close && (*ep == ' ' || *ep == '\t' || *ep == '\n')) ep++;
                        if (ep >= paren_close) break;

                        int explicit_idx = -1;
                        char explicit_key[128]; explicit_key[0] = '\0';
                        /* Check for [sub]=value form. BUG-107: the subscript
                         * is not only digits — [i]=v and [i+1]=v must be
                         * arithmetic-evaluated (bash). Same disambiguation as
                         * BUG-072b/073: digits → index; non-identifier →
                         * arithmetic; bare identifier → arithmetic iff it is
                         * a SET variable, else a literal associative key. */
                        if (*ep == '[') {
                            const char* br_end = ep + 1;
                            while (br_end < paren_close && *br_end != ']') br_end++;
                            if (br_end < paren_close && br_end[1] == '=') {
                                char subx[128];
                                size_t sl = (size_t)(br_end - (ep + 1));
                                if (sl < sizeof(subx)) {
                                    memcpy(subx, ep + 1, sl); subx[sl] = '\0';
                                    int all_digit = subx[0] != '\0', ident = subx[0] != '\0';
                                    const char* ds = subx;
                                    if (*ds == '-') ds++;
                                    for (const char* q = ds; *q; q++)
                                        if (*q < '0' || *q > '9') { all_digit = 0; break; }
                                    for (const char* q = subx; *q; q++)
                                        if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||
                                              (*q>='0'&&*q<='9')||*q=='_')) { ident = 0; break; }
                                    if (all_digit) {
                                        explicit_idx = atoi(subx);
                                        ep = br_end + 2;
                                    } else if (!ident || jash_env_get(s, subx) != NULL) {
                                        long ix = jash_eval_arithmetic(subx, s);
                                        if (ix < 0) ix = 0;
                                        explicit_idx = (int)ix;
                                        ep = br_end + 2;
                                    } else {
                                        /* unset identifier → associative literal key */
                                        snprintf(explicit_key, sizeof(explicit_key), "%s", subx);
                                        ep = br_end + 2;
                                    }
                                }
                            }
                        }

                        /* Extract the element token, PRESERVING quotes so the
                         * expansion pipeline can honor them (BUG-080: $v inside
                         * '...' must stay literal). Boundary is quote- and
                         * paren-aware. `had_quote` records whether any part was
                         * quoted — an unquoted element is field-split. */
                        char elem[1024];
                        size_t el = 0;
                        int eq = 0, edq = 0, had_quote = 0, pdep = 0;
                        while (ep < paren_close) {
                            char c = *ep;
                            if (eq) { if (c == '\'') eq = 0; if (el + 1 < sizeof(elem)) elem[el++] = c; ep++; continue; }
                            if (edq) {
                                if (c == '\\' && ep[1]) { if (el + 2 < sizeof(elem)) { elem[el++] = c; elem[el++] = ep[1]; } ep += 2; continue; }
                                if (c == '"') edq = 0;
                                if (el + 1 < sizeof(elem)) { elem[el++] = c; } ep++; continue;
                            }
                            if (c == '\\' && ep[1]) { if (el + 2 < sizeof(elem)) { elem[el++] = c; elem[el++] = ep[1]; } ep += 2; continue; }
                            /* BUG-109: quotes INSIDE $(...) belong to the inner
                             * command, not to the element — attributing them to
                             * the element (had_quote) suppressed field-splitting
                             * of a=($(echo 'a  b')). Only pdep==0 quotes count.
                             * The eq/edq toggling still runs at any depth so a
                             * quoted `)` doesn't end the paren scan (BUG-075). */
                            if (c == '\'') { eq = 1; if (pdep == 0) had_quote = 1; if (el + 1 < sizeof(elem)) elem[el++] = c; ep++; continue; }
                            if (c == '"')  { edq = 1; if (pdep == 0) had_quote = 1; if (el + 1 < sizeof(elem)) elem[el++] = c; ep++; continue; }
                            if (c == '(') { pdep++; if (el + 1 < sizeof(elem)) elem[el++] = c; ep++; continue; }
                            if (c == ')') { if (pdep > 0) pdep--; if (el + 1 < sizeof(elem)) elem[el++] = c; ep++; continue; }
                            if (pdep == 0 && (c == ' ' || c == '\t' || c == '\n')) break;
                            if (el + 1 < sizeof(elem)) elem[el++] = c;
                            ep++;
                        }
                        elem[el] = '\0';

                        /* Expand: ansi-c -> cmdsub -> arith -> param. An UNQUOTED
                         * element is field-split (BUG-081: a=($(echo 1 2 3)) →
                         * three elements); a quoted element is one word. */
                        const char* eval_ptr = elem;
                        { jash_expand_ctx_t* ectx = eval_expand_ctx_slot();
                          if (ectx) { const char* _a = jash_expand_ansi_c(eval_ptr, ectx); if (_a) eval_ptr = _a; } }
                        if (jash_has_cmdsub(eval_ptr)) {
                            extern int g_jash_cmdsub_no_split;
                            int _ns = g_jash_cmdsub_no_split;
                            g_jash_cmdsub_no_split = had_quote ? 1 : 0;
                            const char* x = jash_expand_cmdsub_known(s, eval_ptr);
                            g_jash_cmdsub_no_split = _ns;
                            if (x) eval_ptr = x;
                            if (jash_cmdsub_error()) {
                                jash_writes(jash_stderr_writer, 0, jash_cmdsub_error_msg());
                                jash_writes(jash_stderr_writer, 0, "\n");
                                s->last_status.code = 1;
                                eval_env_rollback(s, &old_env);
                                return 1;
                            }
                        }
                        if (jash_has_arithmetic(eval_ptr)) {
                            jash_arith_clear_error();
                            const char* x = jash_expand_arithmetic_known(eval_ptr, s);
                            if (x) eval_ptr = x;
                            if (jash_arith_error()) {   /* BUG-126: div/mod by 0 */
                                jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
                                s->last_status.code = 1;
                                eval_env_rollback(s, &old_env);
                                return 1;
                            }
                        }
                        if (jash_has_param_expansion(eval_ptr)) {
                            const char* x = jash_param_expand(eval_ptr, s);
                            if (x) eval_ptr = x;
                            if (jash_param_expand_error()) {
                                jash_writes(jash_stderr_writer, 0,
                                            jash_param_expand_error_msg());
                                jash_writes(jash_stderr_writer, 0, "\n");
                                s->last_status.code = 1;
                                eval_env_rollback(s, &old_env);
                                return 1;
                            }
                        }
                        jash_cmdsub_unprotect((char*)eval_ptr);

                        {
                            extern int jash_unquote(const char* in, char* out, size_t out_size);
                            extern int jash_has_quotes(const char* str);
                            char unq[1024];
                            const char* store = eval_ptr;
                            if (jash_has_quotes(store) || strchr(store, '\\')) {
                                if (jash_unquote(store, unq, sizeof(unq)) >= 0) store = unq;
                            }
                            /* BUG-116a: unshield AFTER quote removal (see the
                             * scalar assignment site). */
                            jash_quote_unshield((char*)store);
                            if (!had_quote && explicit_idx < 0 && !explicit_key[0]) {
                                /* BUG-108: pathname-expand the unquoted element
                                 * (a=(*.txt) must list matching files).
                                 * BUG-167: honor shopt nullglob instead of
                                 * hardcoding "unmatched patterns stay literal". */
                                if (!s->shell_opts.noglob &&
                                    (strchr(store, '*') || strchr(store, '?') ||
                                     strchr(store, '['))) {
                                    jash_expand_ctx_t* gctx = eval_expand_ctx_slot();
                                    if (gctx) {
                                        gctx->nullglob = s->shell_opts.nullglob;
                                        const char* gx = jash_expand_glob(store, gctx);
                                        if (jash_glob_error()) {
                                            jash_writes(jash_stderr_writer, 0,
                                                        jash_glob_error_msg());
                                            jash_writes(jash_stderr_writer, 0, "\n");
                                            s->last_status.code = 1;
                                            eval_env_rollback(s, &old_env);
                                            return 1;
                                        }
                                        if (gx) store = gx;
                                    }
                                }
                                /* Field-split the unquoted expansion on IFS
                                 * whitespace — each field is its own element. */
                                const char* sp = store;
                                while (*sp) {
                                    while (*sp == ' ' || *sp == '\t' || *sp == '\n') sp++;
                                    if (!*sp) break;
                                    const char* ws = sp;
                                    while (*sp && *sp != ' ' && *sp != '\t' && *sp != '\n') sp++;
                                    char field[1024]; size_t fl = (size_t)(sp - ws);
                                    if (fl >= sizeof(field)) fl = sizeof(field) - 1;
                                    memcpy(field, ws, fl); field[fl] = '\0';
                                    char key[256];
                                    snprintf(key, sizeof(key), "%s[%d]", vname, next_idx++);
                                    ARRAY_SET_OR_ROLLBACK(key, field);
                                }
                            } else if (explicit_key[0]) {
                                /* BUG-107: associative literal key.
                                 * BUG-106b: in `name+=([k]=v)` each pair
                                 * string-APPENDS to the existing element. */
                                char key[256];
                                snprintf(key, sizeof(key), "%s[%s]", vname, explicit_key);
                                if (arr_append && jash_env_get(s, key)) {
                                    ARRAY_APPEND_OR_ROLLBACK(key, store);
                                } else {
                                    ARRAY_SET_OR_ROLLBACK(key, store);
                                }
                            } else if (explicit_idx < 0 && strchr(store, '\x1E')) {
                                /* BUG-094: a quoted "${b[@]}" inside the
                                 * literal — each \x1E-bounded element becomes
                                 * its own element (b=("${a[@]}" four)). */
                                const char* sp2 = store;
                                for (;;) {
                                    const char* rs = strchr(sp2, '\x1E');
                                    char key[256];
                                    snprintf(key, sizeof(key), "%s[%d]", vname, next_idx++);
                                    if (rs) {
                                        char field[1024];
                                        size_t fl = (size_t)(rs - sp2);
                                        if (fl >= sizeof(field)) fl = sizeof(field) - 1;
                                        memcpy(field, sp2, fl); field[fl] = 0;
                                        ARRAY_SET_OR_ROLLBACK(key, field);
                                        sp2 = rs + 1;
                                    } else {
                                        ARRAY_SET_OR_ROLLBACK(key, sp2);
                                        break;
                                    }
                                }
                            } else {
                                int use_idx = (explicit_idx >= 0) ? explicit_idx : next_idx;
                                char key[256];
                                snprintf(key, sizeof(key), "%s[%d]", vname, use_idx);
                                /* BUG-106b: `a+=([1]=B)` appends to a[1] (bash),
                                 * it does not replace. Only for EXPLICIT
                                 * subscripts — bare elements append as new
                                 * elements at next_idx. */
                                if (arr_append && explicit_idx >= 0 &&
                                    jash_env_get(s, key)) {
                                    ARRAY_APPEND_OR_ROLLBACK(key, store);
                                } else {
                                    ARRAY_SET_OR_ROLLBACK(key, store);
                                }
                                if (use_idx >= next_idx) next_idx = use_idx + 1;
                            }
                        }
                    }
#undef ARRAY_APPEND_OR_ROLLBACK
#undef ARRAY_SET_OR_ROLLBACK
                    eval_env_snapshot_free(&old_env);
                    s->last_status.code = 0;
                    return 0;
                }
            }
            if (*p == '=' || (p[0] == '+' && p[1] == '=')) {
                /* Assignment(s): VAR=val [VAR2=val2 ...] — multi-assignment.
                 * Each iteration parses one name[+]=value. The value boundary
                 * scan is quote- AND paren-aware so `X="a b"`, `X=$(echo A=1)`
                 * and `MSG='B=2 inside'` are never split mid-value, while
                 * `A=1 B=2 C=3` sets all three variables.
                 *
                 * `FOO=val cmd args` (inline env): the assignments apply
                 * only to the child command. We capture them locally,
                 * snapshot the prior env, set the new values, dispatch
                 * the command, then restore. */
                char assign_unq[JASH_ENV_VAL_LEN];
                const char* cursor = idstart;

                /* ═══ JINV-086 — o VALOR de uma atribuição parava em 1023 B ═══
                 *
                 * `char vbuf[1024]` com `if (vlen >= sizeof(vbuf)) vlen =
                 * sizeof(vbuf) - 1;`. Medido contra o bash:
                 *
                 *     V=<1023 a's>; echo ${#V}   jash 1023 · bash 1023
                 *     V=<1024 a's>; echo ${#V}   jash 1023 · bash 1024
                 *     V=<8000 a's>; echo ${#V}   jash 1023 · bash 8000
                 *
                 * rc 0, stderr vazio, nos três. O valor não é só dado: quando
                 * a cauda cortada carrega um `>` ou um `&&` de um `$(…)` ainda
                 * não expandido, o comando executado deixa de ser o escrito —
                 * a mesma família do JINV-026/028.
                 *
                 * Não aparecia via `V=$(cat arquivo)` (ali o cmdsub devolve o
                 * texto DEPOIS deste buffer) — só no literal, que é a forma
                 * que um agente escreve quando cola um JSON ou um patch.
                 *
                 * Conserto (idioma do JINV-080/082): os 1024 bytes de pilha
                 * viram um LIMIAR DE DESEMPENHO — abaixo dele, zero `malloc`,
                 * exatamente como antes; acima, heap do tamanho exato, reusado
                 * entre as iterações do multi-assign (`A=… B=… C=…` não faz um
                 * `malloc` por variável). Falta de memória é recusa ALTA: a
                 * atribuição NÃO acontece.
                 *
                 * `assign_unq` (a remoção de aspas, JASH_ENV_VAL_LEN = 8 KiB
                 * por `core/limits.h`) tinha o mesmo problema um degrau acima:
                 * `jash_unquote` recusa no estouro (JINV-027, correto) e o
                 * valor ficava COM as aspas — `V="<9000 a's>"` medindo 9002.
                 * Cresce junto, pelo mesmo motivo e com a mesma recusa.
                 *
                 * Porta única de saída (`assign_done`) para que nenhum dos 7
                 * `return` deste bloco vaze os dois buffers.
                 *
                 * Gate: tests/test_jinv086_silent_caps.sh */
                char*  vbuf_heap = NULL;    /* JINV-086 */
                size_t vbuf_cap  = 0;
                char*  unq_heap  = NULL;    /* JINV-086 */
                size_t unq_cap   = 0;
                int    assign_rc = 0;       /* JINV-086: porta única de saída */
                const char* assign_tail = NULL;  /* JINV-086: `VAR=v\ncmd` */
                /* JINV-086: "a atribuição ABORTOU" é diferente de "a
                 * atribuição terminou com rc != 0". Só o primeiro cancela o
                 * comando de cauda: em `A=$(false) echo hi` o rc vem do
                 * `$(false)` e o `echo hi` TEM que rodar assim mesmo (bash).
                 * Colapsar os dois num único `assign_rc` engoliria o comando —
                 * exatamente a classe de erro que este arquivo combate. */
                int    assign_abort = 0;

                /* Track up to 32 inline assignments for restore. */
                struct { char k[128]; char old[1024]; int had_old; } inline_save[32];
                int inline_n = 0;
                const char* command_remainder = NULL;
                int had_cmdsub = 0;  /* BUG-074d: RHS contained a $(...) */

                while (1) {
                    /* Parse identifier at cursor */
                    const char* ids = cursor;
                    const char* q = cursor;
                    while (*q && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                                  (*q >= '0' && *q <= '9') || *q == '_')) q++;
                    int append = 0;
                    const char* eq_ptr;
                    if (*q == '=') { eq_ptr = q; }
                    else if (q[0] == '+' && q[1] == '=') { append = 1; eq_ptr = q + 1; }
                    else break; /* trailing word is not an assignment — done */

                    unsigned vname_len = (unsigned)(q - ids);
                    char vname[128];
                    if (vname_len == 0) break;
                    if (vname_len >= sizeof(vname)) vname_len = sizeof(vname) - 1;
                    for (unsigned i = 0; i < vname_len; i++) vname[i] = ids[i];
                    vname[vname_len] = '\0';
                    if (jash_readonly_is_set(vname)) {
                        assign_rc = 1; assign_abort = 1;   /* JINV-086 */
                        goto assign_done;
                    }

                    const char* raw_val = eq_ptr + 1;
                    /* Find value end: stop at whitespace followed by another
                     * name[+]= token, tracking quotes, $() / (), and `..` so
                     * boundaries inside those constructs are ignored. */
                    const char* end = raw_val;
                    {
                        char qc = 0;
                        int pdepth = 0, btick = 0, cmt = 0;
                        char pvc = '=';
                        /* BUG-153: `$(` inside "..." opens a fresh quoting
                         * context (POSIX 2.2.3). Remember, per paren depth,
                         * whether closing that paren must resume the dq
                         * scan, so `v="$(echo "a b")"` is ONE value. */
                        char dqsave[64] = {0};
                        while (*end) {
                            char c = *end;
                            /* BUG-121a: #-comments inside a multi-line $()
                             * are inert — a `)` in one must not close pdepth. */
                            if (cmt) { if (c == '\n') cmt = 0; pvc = c; end++; continue; }
                            if (qc) {
                                if (c == '$' && qc == '"' && end[1] == '(') {
                                    /* BUG-153: suspend the dq scan for the
                                     * cmdsub extent; resume at its `)`. */
                                    if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 1;
                                    pdepth++; qc = 0; pvc = '('; end += 2; continue;
                                }
                                if (c == qc) qc = 0;
                                else if (c == '\\' && qc == '"' && end[1]) end++;
                                pvc = c; end++; continue;
                            }
                            if (c == '\\' && end[1]) { pvc = end[1]; end += 2; continue; }
                            if (c == '\'' || c == '"') { qc = c; pvc = c; end++; continue; }
                            if (c == '`') { btick = !btick; pvc = c; end++; continue; }
                            if (pdepth > 0 && c == '#' &&
                                (pvc == ' ' || pvc == '\t' || pvc == '\n' ||
                                 pvc == ';' || pvc == '(' || pvc == '|' ||
                                 pvc == '&')) { cmt = 1; end++; continue; }
                            if (c == '(') { pdepth++; pvc = c; end++; continue; }
                            if (c == ')') {
                                if (pdepth > 0) {
                                    pdepth--;
                                    if (pdepth < (int)sizeof(dqsave) && dqsave[pdepth]) {
                                        /* BUG-153: cmdsub closed — resume dq */
                                        dqsave[pdepth] = 0; qc = '"';
                                    }
                                }
                                pvc = c; end++; continue;
                            }
                            pvc = c;
                            if (c == '\n' && pdepth == 0 && !btick) {
                                /* BUG-154 defense: a top-level newline ends
                                 * the assignment statement. Never swallow it
                                 * into the value — doing so reparsed the
                                 * tail of the NEXT line as a command. */
                                break;
                            }
                            if ((c == ' ' || c == '\t') && pdepth == 0 && !btick) {
                                /* Stop here. The peek decides whether what
                                 * follows is another assignment (continue
                                 * multi-assign loop) or a command (run with
                                 * scoped env). */
                                break;
                            }
                            end++;
                        }
                    }
                    /* Trim trailing whitespace of this value slice (quote-aware, JINV-063) */
                    unsigned vlen = (unsigned)(end - raw_val);
                    /* JINV-086: aqui morava o corte mudo em 1023 bytes. */
                    char  vbuf_inline[1024];
                    char* vbuf = vbuf_inline;
                    if (vlen >= sizeof(vbuf_inline)) {
                        if ((size_t)vlen + 1 > vbuf_cap) {
                            char* n = (char*)realloc(vbuf_heap, (size_t)vlen + 1);
                            if (!n) {
                                jash_writef(jash_stderr_writer, 0,
                                            "%sjash: %s: sem memoria para o valor — a atribuicao NAO foi feita%s\n",
                                            J_ROLE_ERR, vname, J_RESET);
                                if (!s->interactive) s->exit_requested = 1;
                                assign_rc = 2; assign_abort = 1;
                                goto assign_done;
                            }
                            vbuf_heap = n; vbuf_cap = (size_t)vlen + 1;
                        }
                        vbuf = vbuf_heap;
                    }
                    memcpy(vbuf, raw_val, vlen);
                    vbuf[vlen] = '\0';
                    vlen = (unsigned)jash_rtrim_unquoted(vbuf);

                    /* Expand: ansi-c -> cmdsub -> arith -> param. */
                    const char* val = vbuf;
                    /* ADR-0036 W4: depth-indexed heap slot, not a stack-local
                     * ctx — this function recurses up to JASH_EVAL_MAX_DEPTH,
                     * and `val` outlives this block, so a per-call stack ctx
                     * here would cost up to ~20MB of stack at max recursion. */
                    { jash_expand_ctx_t* ectx = eval_expand_ctx_slot();
                      if (ectx) { const char* _a = jash_expand_ansi_c(val, ectx); if (_a) val = _a;
                                  /* JINV-041 */
                                  { const char* _t = jash_expand_tilde_value(s, val, ectx); if (_t) val = _t; } } }
                    if (jash_has_cmdsub(val)) {
                        /* BUG-031: assignment RHS is NOT field-split (POSIX), so
                         * `VAR=$(...)` must keep embedded newlines/spaces. */
                        extern int g_jash_cmdsub_no_split;
                        int _ns = g_jash_cmdsub_no_split; g_jash_cmdsub_no_split = 1;
                        const char* x = jash_expand_cmdsub_known(s, val);
                        g_jash_cmdsub_no_split = _ns;
                        if (x) val = x;
                        if (jash_cmdsub_error()) {
                            jash_writes(jash_stderr_writer, 0, jash_cmdsub_error_msg());
                            jash_writes(jash_stderr_writer, 0, "\n");
                            assign_rc = 1; assign_abort = 1;
                            goto assign_done;
                        }
                        had_cmdsub = 1;  /* BUG-074d: $? comes from the cmdsub */
                    }
                    if (jash_has_arithmetic(val)) {
                        jash_arith_clear_error();
                        const char* x = jash_expand_arithmetic_known(val, s);
                        if (x) val = x;
                        if (jash_arith_error()) {   /* BUG-126: div/mod by 0 */
                            jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
                            assign_rc = 1; assign_abort = 1;   /* JINV-086 */
                            goto assign_done;
                        }
                    }
                    if (jash_has_param_expansion(val)) {
                        const char* x = jash_param_expand(val, s);
                        if (x) val = x;
                    }
                    if (jash_param_expand_error()) {
                        jash_writes(jash_stderr_writer, 0, jash_param_expand_error_msg());
                        jash_writes(jash_stderr_writer, 0, "\n");
                        if (!s->interactive) s->exit_requested = 1;
                        assign_rc = 1; assign_abort = 1;   /* JINV-086 */
                        goto assign_done;
                    }
                    jash_cmdsub_unprotect((char*)val);  /* SECURITY #15 */

                    /* POSIX quote removal on the final value. The old code only
                     * stripped ONE fully-surrounding pair, so mixed forms
                     * (`v=a'b c'`) kept their quotes and, after BUG-059 made
                     * param expansion preserve \' , `v=a\'b` kept the
                     * backslash. jash_unquote is the real thing: partial
                     * quotes, escapes, everything. On failure (odd quote from
                     * a cmdsub output, e.g. `v=$(echo "don't")`) keep the
                     * value untouched — expansion output is not re-quoted. */
                    {
                        extern int jash_unquote(const char* in, char* out, size_t out_size);
                        extern int jash_has_quotes(const char* str);
                        if (jash_has_quotes(val) || strchr(val, '\\')) {
                            /* JINV-086: acima de JASH_ENV_VAL_LEN (8 KiB,
                             * core/limits.h) o unquote recusava — correto por
                             * si (JINV-027) — e o valor seguia COM as aspas,
                             * medindo 2 bytes a mais que o do bash. O buffer
                             * cresce para o tamanho exato em vez de recusar. */
                            char*  ub = assign_unq;
                            size_t uc = sizeof(assign_unq);
                            size_t need = strlen(val) + 1;
                            if (need > uc) {
                                if (need > unq_cap) {
                                    char* n = (char*)realloc(unq_heap, need);
                                    if (!n) {
                                        jash_writef(jash_stderr_writer, 0,
                                                    "%sjash: %s: sem memoria para remover aspas — a atribuicao NAO foi feita%s\n",
                                                    J_ROLE_ERR, vname, J_RESET);
                                        if (!s->interactive) s->exit_requested = 1;
                                        assign_rc = 2; assign_abort = 1;
                                        goto assign_done;
                                    }
                                    unq_heap = n; unq_cap = need;
                                }
                                ub = unq_heap; uc = unq_cap;
                            }
                            if (jash_unquote(val, ub, uc) >= 0) val = ub;
                        }
                    }
                    /* BUG-116a: unshield AFTER quote removal — restoring the
                     * literal quote first let jash_unquote eat it (`w=$v` with
                     * a quote-bearing $v lost the quote). Idempotent when the
                     * unquote path already unshielded; covers the no-quotes
                     * path (0x03 is not a syntactic quote for has_quotes). */
                    jash_quote_unshield((char*)val);    /* BUG-096/116a */

                    /* +=: append to current value.
                     * For declare -i / typeset -i variables the RHS is
                     * evaluated as an arithmetic expression and the result
                     * is added numerically.  For plain variables the fast
                     * path jash_env_append concatenates in-place. */
                    if (append) {
                        char _int_key[160];
                        snprintf(_int_key, sizeof(_int_key), "__jash_int__%s", vname);
                        if (jash_env_get(s, _int_key)) {
                            const char* _cur = jash_env_get(s, vname);
                            long _cv = _cur ? atol(_cur) : 0L;
                            long _rv = jash_eval_arithmetic(val, s);
                            char _res[64];
                            snprintf(_res, sizeof(_res), "%ld", _cv + _rv);
                            jash_env_set(s, vname, _res);
                        } else {
                            /* BUG-106: `array+=string` string-appends to
                             * element 0 in bash. The scalar name is unset for
                             * arrays (elements live in name[0]…), so the
                             * append was a silent no-op. */
                            const char* tgt = vname;
                            char e0key[160];
                            if (!jash_env_get(s, vname)) {
                                snprintf(e0key, sizeof(e0key), "%s[0]", vname);
                                if (jash_env_get(s, e0key)) tgt = e0key;
                            }
                            if (jash_env_append(s, tgt, val) < 0)
                                jash_env_set(s, tgt, val);
                        }
                    } else {
                        /* Snapshot previous value for restore (in case the
                         * trailing token turns out to be a command). */
                        if (inline_n < (int)(sizeof(inline_save)/sizeof(inline_save[0]))) {
                            unsigned kl = (unsigned)strlen(vname);
                            if (kl >= sizeof(inline_save[inline_n].k))
                                kl = sizeof(inline_save[inline_n].k) - 1;
                            memcpy(inline_save[inline_n].k, vname, kl);
                            inline_save[inline_n].k[kl] = '\0';
                            const char* prev = jash_env_get(s, vname);
                            inline_save[inline_n].had_old = prev ? 1 : 0;
                            if (prev) {
                                unsigned pl = (unsigned)strlen(prev);
                                if (pl >= sizeof(inline_save[inline_n].old))
                                    pl = sizeof(inline_save[inline_n].old) - 1;
                                memcpy(inline_save[inline_n].old, prev, pl);
                                inline_save[inline_n].old[pl] = '\0';
                            } else {
                                inline_save[inline_n].old[0] = '\0';
                            }
                            inline_n++;
                        }
                        /* Integer-typed var: evaluate RHS as arithmetic */
                        char _ik[160];
                        snprintf(_ik, sizeof(_ik), "__jash_int__%s", vname);
                        if (jash_env_get(s, _ik)) {
                            char _ir[64];
                            snprintf(_ir, sizeof(_ir), "%ld",
                                     jash_eval_arithmetic(val, s));
                            jash_env_set(s, vname, _ir);
                        } else {
                            jash_env_set(s, vname, val);
                        }
                    }

                    /* More assignments after this value? */
                    if (!*end) break;
                    if (*end == '\n') {
                        /* BUG-154 defense: newline = command separator, NOT
                         * the inline-env `VAR=v cmd` form. The assignments
                         * PERSIST; the rest is a separate statement. */
                        const char* rest = end + 1;
                        while (*rest == ' ' || *rest == '\t' || *rest == '\n') rest++;
                        if (!*rest) break;
                        /* JINV-086: libera ANTES de reentrar — `rest` pode ser
                         * um script inteiro, e segurar os buffers durante a
                         * recursão os multiplicaria por nível. */
                        assign_tail = rest;
                        goto assign_done;
                    }
                    cursor = skip_ws(end);
                    if (!((*cursor >= 'a' && *cursor <= 'z') ||
                          (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_')) {
                        /* Non-identifier next → trailing command */
                        if (*cursor) command_remainder = cursor;
                        break;
                    }
                    /* Identifier could be `NAME=val` (another assignment)
                     * or `cmd args` (command). Peek for `=`. */
                    {
                        const char* lk = cursor;
                        while ((*lk >= 'a' && *lk <= 'z') ||
                               (*lk >= 'A' && *lk <= 'Z') ||
                               (*lk >= '0' && *lk <= '9') || *lk == '_') lk++;
                        if (*lk != '=' && !(lk[0] == '+' && lk[1] == '=')) {
                            command_remainder = cursor;
                            break;
                        }
                    }
                }

                /* No trailing command: assignments persist (normal POSIX).
                 * BUG-074d: `x=$(cmd)` sets $? to the substitution's status,
                 * not 0 — a plain `x=literal` still yields 0. */
                if (had_cmdsub) {
                    extern int g_jash_cmdsub_last_status;
                    assign_rc = g_jash_cmdsub_last_status;   /* JINV-086 */
                } else {
                    assign_rc = 0;
                }

assign_done:                     /* JINV-086: porta única de saída */
                if (vbuf_heap) free(vbuf_heap);
                if (unq_heap)  free(unq_heap);
                /* A recursão acontece SÓ depois de liberar — os dois casos que
                 * reentram (`VAR=v\ncmd` e o prefixo `VAR=v cmd`) podem descer
                 * um script inteiro, e o heap segurado se multiplicaria por
                 * nível de recursão. JINV-086 */
                if (assign_tail) {
                    int rc = jash_eval_line(s, w, ctx, assign_tail);
                    s->last_status.code = rc;
                    return rc;
                }
                if (!assign_abort && command_remainder) {
                    /* Inline env: assignments are scoped to this command only.
                     * Dispatch the remainder, then restore the prior values. */
                    int rc = jash_eval_line(s, w, ctx, command_remainder);
                    for (int i = inline_n - 1; i >= 0; i--) {
                        if (inline_save[i].had_old)
                            jash_env_set(s, inline_save[i].k, inline_save[i].old);
                        else
                            jash_env_unset(s, inline_save[i].k);
                    }
                    s->last_status.code = rc;
                    return rc;
                }
                s->last_status.code = assign_rc;
                return assign_rc;
            }
        }
    }

    /* split_first used by other paths below (alias, builtin lookup) */
    char first_token[128];
    const char* rest = split_first(line, first_token, sizeof(first_token));
    (void)rest;
    /* Not an assignment — fall through to normal command dispatch. */

    /* Decide whether to skip global expansion for pipelines containing
     * compound commands (while/for/case/if/until/select, subshells, brace
     * groups). Their bodies must keep $(...), $vars, $((...)) literal so the
     * pipeline engine expands them in the correct per-segment context.
     * Without this, `echo a | while … do out=$(cmd 2>&1); …` expands the
     * command substitution globally (prematurely grabbing redirects). */
    int skip_global_expand = 0;
    if (jash_line_has_pipe(line)) {
        const char* kws[] = {"while ", "until ", "for ", "case ", "if ", "select ", NULL};
        for (int ki = 0; kws[ki]; ki++) {
            const char* kw = kws[ki];
            size_t kwlen = strlen(kw);
            const char* p = line;
            while ((p = strstr(p, kw))) {
                if (p == line || isspace(p[-1]) || p[-1] == '|' || p[-1] == ';') {
                    skip_global_expand = 1;
                    break;
                }
                p += kwlen;
            }
            if (skip_global_expand) break;
        }
        if (!skip_global_expand) {
            /* JINV-124: um `(` que vem logo depois de `$` e EXPANSAO, nunca
             * subshell. Esta varredura lia o SEGUNDO `(` de `$((` como uma
             * abertura de subshell em posicao de comando, erguia
             * skip_global_expand para a LINHA INTEIRA e com isso desligava a
             * expansao global de aritmetica E de cmdsub. A engine de pipeline
             * so re-expande PARAMETROS por segmento, entao `$VAR` sobrevivia e
             * `$((x*3))` saia literal — a assimetria que fazia o bug parecer
             * arbitrario (`echo $((x*3)) $HOME | cat` → `$((x*3)) /Users/...`).
             *
             * Mesma familia do JINV-098/116/117: varredura de texto de shell
             * sem carregar estado de aspas. Por isso `jash_quote_step` aqui —
             * sem ele um `|` DENTRO de aspas (`echo "| (" | cat`) devolvia a
             * posicao de comando e o `(` seguinte disparava o mesmo engano. */
            char qs = 0;   /* estado de aspas, JINV-116/117 */
            int  back = 0; /* dentro de `...` — inerte */
            int at_cmd = 1;
            for (const char* p = line; *p; ) {
                if (back) { if (*p == '`') back = 0; p++; continue; }

                const char* nx = jash_quote_step(p, &qs);
                if (nx) { at_cmd = 0; p = nx; continue; }  /* aspas/escape = DADO */

                if (*p == '`') { back = 1; at_cmd = 0; p++; continue; }
                if (*p == ' ' || *p == '\t') { p++; continue; }  /* nao mexe em at_cmd */

                /* `$( … )`, `$(( … ))`, `${ … }`: consome a regiao inteira como
                 * UMA palavra, com aninhamento contado e ciente de aspas. */
                if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
                    const char open  = p[1];
                    const char close = (open == '(') ? ')' : '}';
                    int depth = 0;
                    char iq = 0;
                    p++;                       /* fica sobre o abridor */
                    while (*p) {
                        const char* qn = jash_quote_step(p, &iq);
                        if (qn) { p = qn; continue; }
                        if (*p == open)  { depth++; p++; continue; }
                        if (*p == close) { depth--; p++; if (depth <= 0) break; continue; }
                        p++;
                    }
                    at_cmd = 0;
                    continue;
                }

                if (at_cmd && *p == '(') { skip_global_expand = 1; break; }
                if (at_cmd && *p == '{' &&
                    (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
                    skip_global_expand = 1; break;
                }
                at_cmd = (*p == '|' || *p == ';' || *p == '&' ||
                          *p == '(' || *p == '{');
                p++;
            }
        }
    }

    /* v0.6: Command substitution $(cmd) and `cmd` — done FIRST since
     * its output can itself contain arithmetic/var refs. BUT skip for
     * pipelines with compound commands (compound handlers expand cmdsub
     * in their own context, preserving redirects inside $()). */
    /* JINV-090: `line` comeca apontando para a ENTRADA, que pode ser const.
     * So depois que uma passada de expansao o troca por um buffer proprio e
     * que se pode ESCREVER nele. Este sinalizador e a licenca de escrita —
     * sem ele, blindar o `$` residual mais abaixo escreveria em memoria de
     * so-leitura. (O restauro que morava la ja dependia disso, mas de forma
     * implicita: "sentinela so existe se cmdsub rodou".) */
    int line_mutable = 0;   /* JINV-090 */
    int has_pipe = 0;

    /* One fused presence scan for cmdsub/arith/param/quoted-heredoc
     * (was three quote-aware passes plus four strstr calls per command).
     * After any stage rewrites the line, rescan: an expansion can
     * introduce new triggers (e.g. cmdsub emitting `$HOME`). */
    unsigned exp_mask = 0;
    if (!skip_global_expand) exp_mask = jash_expansion_mask(line);
    if (exp_mask & JASH_EXP_CMDSUB) {
        const char* cs_expanded = jash_expand_cmdsub_known(s, line);
        if (jash_cmdsub_error()) {
            jash_writes(jash_stderr_writer, 0, jash_cmdsub_error_msg());
            jash_writes(jash_stderr_writer, 0, "\n");
            s->last_status.code = 1;
            return 1;
        }
        if (cs_expanded && cs_expanded != line) {
            line = skip_ws(cs_expanded);
            line_mutable = 1;   /* JINV-090 */
            if (!*line) { s->last_status.code = 0; return 0; }
            exp_mask = jash_expansion_mask(line);
        }
    }

    /* Protect quoted heredoc bodies from expansion (<<'EOF' suppresses $VAR).
     * Must run BEFORE arithmetic and parameter expansion passes. */
    if ((exp_mask & JASH_EXP_HEREDOCQ) ||
        (skip_global_expand &&
         (strstr(line, "<<'") || strstr(line, "<<\"") ||
          strstr(line, "<<-'") || strstr(line, "<<-\"")))) {
        /* The protect rewrite below changes the line, so a stale mask
         * would gate the arith stage on pre-rewrite bytes. Rescan
         * (heredoc markers are rare; the common path pays nothing). */
        size_t llen = strlen(line);
        char* mcopy = (char*)malloc(llen + 1);
        if (mcopy) {
            memcpy(mcopy, line, llen + 1);
            jash_protect_quoted_heredocs(mcopy);
            line = mcopy;
            line_mutable = 1;   /* JINV-090 */
            if (!skip_global_expand) exp_mask = jash_expansion_mask(line);
        }
    }

    /* v0.5 FIX #3: Arithmetic expansion $((expr)).
     * Done FIRST so $((X+1)) becomes literal "11" before param_expand runs.
     * NOTE: expansion is UNCONDITIONAL here. A previous attempt guarded it
     * behind "no separator / no pipe" assuming the pipeline engine expands
     * per-segment — it does NOT, which silently turned `echo $X | cat` into
     * literal `$X`. Compound keywords (while/for/if/case) are intercepted
     * well before this point, so unconditional expansion is safe. */
    if (exp_mask & JASH_EXP_ARITH) {
        jash_arith_clear_error();
        const char* arith_expanded = jash_expand_arithmetic_known(line, s);
        /* bash parity: a division/modulo by zero is an error — report to stderr,
         * set rc 1, and ABORT the command (do not run it with a bogus 0). */
        if (jash_arith_error()) {
            jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
            s->last_status.code = 1;
            return 1;
        }
        if (arith_expanded && arith_expanded != line) {
            line = skip_ws(arith_expanded);
            line_mutable = 1;   /* JINV-090 */
            if (!*line) { s->last_status.code = 0; return 0; }
            /* No rescan: an arithmetic rewrite replaces `$((expr))`
             * with decimal digits, which introduce no `$`, backtick
             * or `<<` trigger — the remaining mask bits stay valid. */
        }
    }

    /* v0.5 FIX #2: Variable expansion ($VAR, ${VAR...}, $?).
     * Done before command dispatch so builtins/externals see expanded args.
     * Skip global expansion for pipelines containing compound commands
     * (while/for/case/if/select) so variables in their bodies are expanded
     * in the correct segment context by the pipeline engine. */
    if (exp_mask & JASH_EXP_PARAM) {
        /* BUG-010: command-argument context → IFS-split unquoted expansions
         * (`set -- $X` with IFS=":" yields 3 params). Assignment RHS uses the
         * plain jash_param_expand elsewhere, so `Y=$X` keeps its separators. */
        const char* pe_split = jash_param_expand_split_known(line, s);
        if (jash_param_expand_error()) {
            jash_writes(jash_stderr_writer, 0, jash_param_expand_error_msg());
            jash_writes(jash_stderr_writer, 0, "\n");
            s->last_status.code = 1;
            /* Parameter errors (set -u nounset, ${var?msg}) terminate the
             * shell in non-interactive (script) mode.  In interactive mode,
             * set -e will trigger exit if set; otherwise the shell continues. */
            if (!s->interactive) s->exit_requested = 1;
            return 1;
        }
        if (pe_split && pe_split != line) {
            /* Re-skip leading ws on expanded buffer */
            line = skip_ws(pe_split);
            line_mutable = 1;   /* JINV-090 */
            if (!*line) { s->last_status.code = 0; return 0; }
        }
    }

    /* Only probe native output builtins, using the EXPANDED command word.
     * Prove the remaining word layer is inert before delegating; all other
     * commands pay just the prefix check, not another full-line scan. */
    if (!skip_global_expand &&
        (strncmp(line, "printf ", 7) == 0 || strncmp(line, "echo ", 5) == 0)) {
        const jash_builtin_t* b = jash_builtin_lookup(line[0] == 'p' ? "printf" : "echo");
        if (b && (b->argv_fn == jash_b_printf || b->argv_fn == jash_b_echo_argv) &&
            !jash_glob_error()) {
            char quote = 0;
            int has_redir = 0;
            size_t n = 0;
            /* Match the word layer's n + 1 >= cap fallback, including its
             * diagnostic. All three of its buffers use JASH_LINE_CAP_STACK. */
            for (; n + 1 < JASH_LINE_CAP_STACK && line[n]; n++) {
                unsigned char c = (unsigned char)line[n];
                if (c < 0x20 || c >= 0x7f) break; /* includes ALL sentinels */
                switch (c) {
                    case '$': case '`': case '*': case '?': case '[': case ']':
                    case '{': case '}': case '~': case '(': case ')':
                    case '&': case ';': case '\\':
                        goto full_word_expansion;
                    case '|':
                        if (quote || !n || line[n - 1] != '>') goto full_word_expansion;
                        break; /* >| is a redirection, not a pipeline. */
                    case '\'': case '"':
                        (void)jash_quote_step(line + n, &quote);
                        break;
                    case '>': case '<':
                        if (!quote) has_redir = 1;
                        break;
                }
            }
            if (n + 1 < JASH_LINE_CAP_STACK && !line[n] && !quote && has_redir) {
                /* Same native-builtin precedence as the redirected branch
                 * below: do not consume skip_function_lookup or replace w/ctx. */
                int rc = jash_eval_builtin_with_redir(s, w, ctx, b, line);
                s->last_status.code = rc;
                return rc;
            }
        }
    }

full_word_expansion:
    /* SECURITY (audit rank 15): param/arith expansion has now run, so restore
     * the '$' and '`' bytes that jash_expand_cmdsub shielded in the command
     * substitution output. They were protected so they could NOT be re-expanded
     * (variable/arith re-eval of command output); now they pass on as inert
     * literal data. In-place is safe: a sentinel is present only when cmdsub
     * ran, in which case `line` points into a mutable expansion buffer, never
     * the const input.
     *
     * ═══ JINV-090 — o VALOR expandido não volta a ser sintaxe ═══
     *
     * "Agora rodou a expansão" era verdade só para a linha que NÃO vira cano.
     * Para a que vira, este restauro é PREMATURO: logo abaixo o motor de canos
     * expande OUTRA VEZ, por segmento (pipeline.c chama jash_param_expand em
     * cada um), e um `$` restaurado aqui chega vivo lá e é expandido uma
     * SEGUNDA vez. POSIX XCU 2.6 é explícito: o resultado de uma expansão não
     * é reexaminado para novas expansões — é texto final.
     *
     *   V=VAL; echo "[$(echo '$V')]" | cat
     *     bash: [$V]        jash: [VAL]    ← o dado voltou a ser sintaxe
     *
     * Mesma forma dos JINV-016/020/032/083/085/087/089: a marca "isto é dado"
     * EXISTIA e morreu antes do ponto que precisava dela. E o conserto é o
     * mesmo que o JINV-089 fez em case_stmt.c — o restauro MUDOU DE LUGAR,
     * não sumiu: acontece em pipeline.c, depois da expansão de cada segmento
     * (build_argv_local e os dois caminhos de builtin).
     *
     * ═══ E o `$` que a expansao DEIXOU para tras tambem e dado ═══
     *
     * O escudo do cmdsub cobre o `$` que veio da SAIDA de um comando. Falta o
     * que veio do VALOR de uma variavel — e nesse nao ha sentinela nenhum,
     * porque `jash_param_expand` devolve texto cru:
     *
     *   X=SEGREDO; V='$X'; echo "[$V]" | cat
     *     bash: [$X]        jash: [SEGREDO]   ← o motor de canos expandiu de novo
     *
     * A informacao que falta existe EXATAMENTE aqui e em nenhum outro lugar:
     * as passadas de expansao acabaram de rodar por completo, logo qualquer
     * `$` que SOBROU e um `$` que o shell ja decidiu nao expandir — literal
     * da fonte, citado, escapado, ou vindo de um valor. Nos tres casos: dado.
     * Gravar a marca aqui e o mesmo idioma do JINV-089 — grave onde ainda se
     * sabe, restaure depois do ultimo ponto que poderia reler.
     *
     * So para a linha que vai virar cano, e so quando ESTA funcao foi quem
     * expandiu (`!skip_global_expand`): quando a expansao foi adiada para o
     * motor de compostos, a sintaxe ainda esta viva e blinda-la a mataria.
     * E so quando `line` ja e buffer proprio (`line_mutable`) — a entrada
     * pode ser const.
     *
     * Gate: tests/test_jinv090_eval_no_reexpand.sh */
    /* Computed once and reused below: the two downstream has_pipe checks
     * run on this same line unless the word layer rewrites it (which
     * rescans — a rewrite can introduce or remove operators). */
    has_pipe = jash_line_has_pipe(line);
    if (!has_pipe) {
        jash_cmdsub_unprotect((char*)line);
    } else if (line_mutable && !skip_global_expand) {
        jash_cmdsub_protect_residual((char*)line);   /* JINV-090 */
    }

    /* v0.7: word expansions — tilde (~), brace ({a,b}/ {1..9}), glob (*?[]).
     * Run after $-expansions so e.g. ~/$DIR/(*.c) works end-to-end.
     * Skipped for shell keywords (if/for/while/case) so their own
     * parsers see the raw line. */
    if (strncmp(line, "if ", 3) != 0 && strncmp(line, "for ", 4) != 0 &&
        strncmp(line, "while ", 6) != 0 && strncmp(line, "until ", 6) != 0 &&
        strncmp(line, "case ", 5) != 0 && strncmp(line, "[[", 2) != 0) {
        jash_expand_ctx_t* ectx = eval_expand_ctx_slot();
        if (ectx) {
            const char* we = jash_expand_word_layer(s, line, ectx);
            if (jash_glob_error()) {
                jash_writes(jash_stderr_writer, 0, jash_glob_error_msg());
                jash_writes(jash_stderr_writer, 0, "\n");
                s->last_status.code = 1;
                return 1;
            }
            if (we && we != line) {
                line = skip_ws(we);
                has_pipe = jash_line_has_pipe(line);
                if (!*line) { s->last_status.code = 0; return 0; }
            }
        }
    }

    /* Pipeline (|) — delegate to pipeline engine after expansions
     * so that simple commands see expanded variables, while compound
     * commands in pipelines skip global expansion (handled per-segment). */
    if (has_pipe) {
        /* A trailing `&` is job control, not pipeline input: `a | b &`
         * backgrounds the whole pipeline as one job. Without this the
         * engine takes `&` as a literal argument of the last segment
         * (and never backgrounds at all). */
        unsigned plen = (unsigned)jash_rtrim_unquoted(line);
        if (plen > 1 && line[plen-1] == '&') {
            char* bl = (char*)malloc(plen + 1);
            if (bl) {
                unsigned bn = plen - 1;
                for (unsigned i = 0; i < bn; i++) bl[i] = line[i];
                bl[bn] = '\0';
                bn = (unsigned)jash_rtrim_unquoted(bl);
                bl[bn] = '\0';
                int brc = jash_bg_spawn(s, w, ctx, bl);
                free(bl);
                s->last_status.code = brc;
                return brc;
            }
            /* malloc failure: fall through to the engine (loud, not silent) */
        }
        int rc = jash_exec_pipeline(s, w, ctx, line);
        s->last_status.code = rc;
        return rc;
    }

    /* A6: if-then-else-fi statement */
    /* MOVED: now checked BEFORE command list splitter (line ~238) */

    /* A6: for VAR in WORDS; do BODY; done */
    /* MOVED: now checked BEFORE command list splitter (line ~238) */

    /* A6: while COND; do BODY; done */
    /* MOVED: now checked BEFORE command list splitter (line ~238) */

    /* A6: case EXPR in PATTERN) CMD ;; esac */
    /* MOVED: now checked BEFORE command list splitter (line ~238) */

    /* GAP #10: [[ ... ]] conditional test */
    if (strncmp(line, "[[", 2) == 0) {
        const char* expr_start = line + 2;
        /* Must skip [[:class:]] bracket expressions to find the real closing ]].
         * A naive strstr("]]") stops at the first ]] inside [[:alpha:]] etc. */
        const char* expr_end = NULL;
        {
            const char* sp = expr_start;
            int sq = 0, dq = 0;
            while (*sp) {
                if (sq) { if (*sp == '\'') sq = 0; sp++; continue; }
                if (dq) { if (*sp == '"')  dq = 0; sp++; continue; }
                if (*sp == '\'') { sq = 1; sp++; continue; }
                if (*sp == '"')  { dq = 1; sp++; continue; }
                /* Skip bracket expressions [abc], [a-z], [[:class:]], [!set]
                 * These are glob patterns that may contain ]] inside them. */
                if (*sp == '[') {
                    const char* bstart = sp + 1;
                    /* Skip leading ! or ^ negation */
                    if (*bstart == '!' || *bstart == '^') bstart++;
                    /* Scan for the closing ] — must skip [: :] POSIX classes */
                    const char* bp = bstart;
                    int found_close = 0;
                    while (*bp) {
                        if (bp[0] == '[' && bp[1] == ':') {
                            bp += 2;
                            while (*bp && !(bp[0] == ':' && bp[1] == ']')) bp++;
                            if (bp[0] == ':') bp += 2;
                        } else if (*bp == ']') {
                            found_close = 1;
                            sp = bp + 1;
                            break;
                        } else {
                            bp++;
                        }
                    }
                    if (found_close) continue;
                    /* Malformed bracket — treat as literal [ */
                }
                if (sp[0] == ']' && sp[1] == ']') { expr_end = sp; break; }
                sp++;
            }
        }
        if (expr_end) {
            char expr[512];
            unsigned elen = (unsigned)(expr_end - expr_start);
            if (elen > 0 && elen + 1 < sizeof(expr)) {
                for (unsigned i = 0; i < elen; i++) expr[i] = expr_start[i];
                expr[elen] = '\0';
                /* Expand $VAR inside [[ ]] expression. Copy to local buffer
                 * first because jash_param_expand uses a static buffer that
                 * may be overwritten by nested expansions. */
                char expanded_buf[512];
                const char* expanded_expr = expr;
                if (jash_has_param_expansion(expr)) {
                    const char* ex = jash_param_expand(expr, s);
                    if (ex && ex != expr) {
                        size_t exlen = strlen(ex);
                        if (exlen >= sizeof(expanded_buf)) exlen = sizeof(expanded_buf) - 1;
                        memcpy(expanded_buf, ex, exlen);
                        expanded_buf[exlen] = '\0';
                        expanded_expr = expanded_buf;
                    }
                }
                int result = jash_parse_conditional(expanded_expr, s);
                s->last_status.code = result ? 0 : 1;
                return result ? 0 : 1;
            }
        }
    }

    /* time KEYWORD — measure execution time of the next pipeline/command. */
    if (strncmp(line, "time ", 5) == 0 || strcmp(line, "time") == 0) {
        const char* rest_time = line + 4;
        while (*rest_time == ' ' || *rest_time == '\t') rest_time++;
        if (!*rest_time) {
            /* bare 'time' — just print real/user/sys (like bash) */
            jash_writes(w, ctx, "0.000u 0.000s 0.000 0.00%\n");
            s->last_status.code = 0;
            return 0;
        }
        /* Time the sub-command */
        struct timeval tv_start, tv_end;
        struct rusage ru_start, ru_end;
        gettimeofday(&tv_start, NULL);
        getrusage(RUSAGE_CHILDREN, &ru_start);
        int rc = jash_eval_line(s, w, ctx, rest_time);
        gettimeofday(&tv_end, NULL);
        getrusage(RUSAGE_CHILDREN, &ru_end);
        
        double real = (tv_end.tv_sec - tv_start.tv_sec) +
                      (tv_end.tv_usec - tv_start.tv_usec) / 1000000.0;
        double user = (ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec) +
                      (ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec) / 1000000.0;
        double sys = (ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec) +
                     (ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec) / 1000000.0;
        
        /* Check for TIMEFORMAT variable */
        const char* fmt = jash_env_get(s, "TIMEFORMAT");
        char tbuf[512];
        if (fmt && *fmt) {
            /* Parse TIMEFORMAT: %R=real, %U=user, %S=sys, %P=CPU%, %%=% */
            unsigned ti = 0;
            const char* fp = fmt;
            while (*fp && ti + 1 < sizeof(tbuf)) {
                if (*fp == '%' && fp[1]) {
                    fp++;
                    if (*fp == 'R') {
                        int n = snprintf(tbuf + ti, sizeof(tbuf) - ti, "%.3f", real);
                        ti += (n > 0) ? n : 0;
                    } else if (*fp == 'U') {
                        int n = snprintf(tbuf + ti, sizeof(tbuf) - ti, "%.3f", user);
                        ti += (n > 0) ? n : 0;
                    } else if (*fp == 'S') {
                        int n = snprintf(tbuf + ti, sizeof(tbuf) - ti, "%.3f", sys);
                        ti += (n > 0) ? n : 0;
                    } else if (*fp == 'P') {
                        double cpu = (real > 0) ? ((user + sys) / real * 100.0) : 0.0;
                        int n = snprintf(tbuf + ti, sizeof(tbuf) - ti, "%.1f", cpu);
                        ti += (n > 0) ? n : 0;
                    } else if (*fp == '%') {
                        tbuf[ti++] = '%';
                    } else {
                        tbuf[ti++] = '%';
                        if (ti + 1 < sizeof(tbuf)) tbuf[ti++] = *fp;
                    }
                    fp++;
                } else {
                    tbuf[ti++] = *fp++;
                }
            }
            tbuf[ti] = '\0';
        } else {
            /* Default bash format */
            snprintf(tbuf, sizeof(tbuf), "%.3fu %.3fs %.3f 0.0%%\n", user, sys, real);
        }
        jash_writes(w, ctx, tbuf);
        s->last_status.code = rc;
        return rc;
    }

    /* coproc KEYWORD — start a coprocess with bidirectional pipes.
     * Syntax: coproc [NAME] COMMAND [REDIRECTIONS]
     * Creates COPROC[0] (read fd) and COPROC[1] (write fd). */
    if (strncmp(line, "coproc ", 7) == 0) {
        const char* rest_cp = line + 7;
        while (*rest_cp == ' ' || *rest_cp == '\t') rest_cp++;

        /* Check for optional NAME: coproc NAME cmd where NAME doesn't
         * look like a shell keyword or contain special chars. */
        char cp_name[64] = "";
        const char* cmd_start = rest_cp;

        /* Heuristic: if the first token is a valid identifier and
         * there's more after it, treat it as the coproc name. */
        if ((*rest_cp >= 'a' && *rest_cp <= 'z') || (*rest_cp >= 'A' && *rest_cp <= 'Z') || *rest_cp == '_') {
            const char* scan = rest_cp;
            int looks_like_name = 1;
            while (*scan && *scan != ' ' && *scan != '\t') {
                if (!((*scan >= 'a' && *scan <= 'z') || (*scan >= 'A' && *scan <= 'Z') ||
                      (*scan >= '0' && *scan <= '9') || *scan == '_')) {
                    looks_like_name = 0;
                    break;
                }
                scan++;
            }
            /* Check there's a space then more content.
             * Bash named-coproc forms:
             *   coproc NAME { CMD; }
             *   coproc NAME ( CMD )
             *   coproc NAME simple-command args      (NAME is a valid id and
             *                                         the next word starts a
             *                                         command, not a redirect)
             * The simple-command form is what `coproc mycat cat` uses; without
             * it NAME was swallowed into the command -> "mycat: command not
             * found". Anonymous coproc is `coproc CMD` (first word IS the cmd),
             * but a bare single-word `coproc foo` stays anonymous (foo = cmd).
             * F E R M I ∞ H A R T */
            if (looks_like_name && (*scan == ' ' || *scan == '\t')) {
                const char* after_name = scan;
                while (*after_name == ' ' || *after_name == '\t') after_name++;
                /* Named iff there is a real command word after NAME. A
                 * redirect/operator there means NAME was actually the command
                 * (anonymous), e.g. `coproc cat <&3`. */
                int next_is_cmd = (*after_name && *after_name != '\n' &&
                                   *after_name != '<' && *after_name != '>' &&
                                   *after_name != '|' && *after_name != '&' &&
                                   *after_name != ';');
                if (next_is_cmd) {
                    /* It's a named coproc. */
                    unsigned nlen = (unsigned)(scan - rest_cp);
                    if (nlen >= sizeof(cp_name)) nlen = sizeof(cp_name) - 1;
                    for (unsigned i = 0; i < nlen; i++) cp_name[i] = rest_cp[i];
                    cp_name[nlen] = '\0';
                    cmd_start = after_name;
                    /* Strip an enclosing { CMD; } or ( CMD ) wrapper so the
                     * child's /bin/sh -c sees a clean command. */
                    if (*cmd_start == '{' || *cmd_start == '(') {
                        char open = *cmd_start, close = (open == '{') ? '}' : ')';
                        const char* inner = cmd_start + 1;
                        while (*inner == ' ' || *inner == '\t') inner++;
                        /* find last close on the line */
                        const char* last = inner;
                        const char* q = inner;
                        while (*q) { if (*q == close) last = q; q++; }
                        if (last > inner) {
                            static char cp_body[1024];
                            unsigned bl = (unsigned)(last - inner);
                            if (bl >= sizeof(cp_body)) bl = sizeof(cp_body) - 1;
                            for (unsigned i = 0; i < bl; i++) cp_body[i] = inner[i];
                            /* trim trailing ws and a single ';' */
                            while (bl > 0 && (cp_body[bl-1] == ' ' || cp_body[bl-1] == '\t')) bl--;
                            if (bl > 0 && cp_body[bl-1] == ';') bl--;
                            cp_body[bl] = '\0';
                            cmd_start = cp_body;
                        }
                    }
                }
            }
        }

        /* Reap any finished coprocs before starting a new one. */
        if (s->coprocs) jash_coproc_reap(s->coprocs, s);

        int rc = jash_coproc_start(s->coprocs, s, cp_name, cmd_start);
        if (rc < 0) {
            jash_writes(w, ctx, "jash: coproc: failed to start\n");
            s->last_status.code = 1;
        } else {
            s->last_status.code = 0;
        }
        return s->last_status.code;
    }

    /* Background operator: cmd & */
    {
        unsigned len = (unsigned)jash_rtrim_unquoted(line);
        if (len > 1 && line[len-1] == '&') {
            /* Strip the & — exact-size buffer, never truncated: a cut
             * background line would run a DIFFERENT command (JINV-029). */
            unsigned bg_cap = len;   /* len-1 chars + NUL */
            char* bg_line = (char*)malloc(bg_cap);
            if (!bg_line) {
                jash_writes(jash_stderr_writer, 0,
                    "jash: sem memoria para o comando em background — NAO executado\n");
                s->last_status.code = 1;
                return 1;
            }
            unsigned bg_len = len - 1;
            for (unsigned i = 0; i < bg_len; i++) bg_line[i] = line[i];
            bg_line[bg_len] = '\0';
            bg_len = (unsigned)jash_rtrim_unquoted(bg_line);
            bg_line[bg_len] = '\0';
            int bg_rc = jash_bg_spawn(s, w, ctx, bg_line);
            free(bg_line);
            s->last_status.code = bg_rc;
            return bg_rc;
        }
    }

    /* Redirect-only (no pipe) on a builtin: dup2 in-parent so builtins
     * that write via the writer (which goes to fd 1) end up in the file.
     * If the cmd isn't a builtin, fall through to pipeline engine which
     * will exec the external with redirs applied in the child. */
    if (jash_line_has_redir(line)) {
        /* Try builtin lookup on first token. */
        char tn[64];
        const char* targs = split_first(line, tn, sizeof(tn));
        const jash_builtin_t* tb = jash_builtin_lookup(tn);
        if (!tb) {
            /* Check for user-defined function: must run in the current shell
             * with fds temporarily redirected, never in a forked subshell.
             * BUG-013: `command` skips this so a shadowing function is bypassed
             * (consume the one-shot flag here so it never leaks to later cmds). */
            int redir_skip_fn = s->skip_function_lookup;
            if (redir_skip_fn) s->skip_function_lookup = 0;
            if (s->functions && !redir_skip_fn) {
                const char* fbody = jash_function_lookup(s->functions, tn);
                if (fbody) {
                    extern int jash_eval_function_with_redir(jash_state_t*,
                                                             jash_writer_t,
                                                             void*,
                                                             const char* fname,
                                                             const char* fn_args);
                    int rc = jash_eval_function_with_redir(s, w, ctx, tn, targs);
                    s->last_status.code = rc;
                    return rc;
                }
            }
            /* Not a builtin or function — pipeline engine handles redir for externals. */
            int rc = jash_exec_pipeline(s, w, ctx, line);
            s->last_status.code = rc;
            return rc;
        }
        /* Builtin path: extract redirs, dup2 stdout/stdin, call builtin,
         * restore. Reuse pipeline parser by calling pipeline engine —
         * but the pipeline engine forks. Instead do a minimal inline
         * handler here. */
        extern int jash_eval_builtin_with_redir(jash_state_t*, jash_writer_t,
                                                void*, const jash_builtin_t*,
                                                const char* line);
        /* Special case: exec with redirections but no command.
         * POSIX: 'exec [redirections]' applies redirections permanently
         * to the current shell (no save/restore). Without this, 'exec 3>&-'
         * hits the usage error path. */
        if (strcmp(tn, "exec") == 0) {
            int rc = jash_exec_apply_redirs_permanently(s, line);
            if (rc >= 0) {
                s->last_status.code = rc;
                return rc;
            }
        }
        int rc = jash_eval_builtin_with_redir(s, w, ctx, tb, line);
        s->last_status.code = rc;
        return rc;
    }

    /* DISABLED: Quote removal was happening too early, before word splitting.
     * This destroyed argument boundaries (e.g., printf "%s" "hello world" 
     * became printf %s hello world, losing the grouping).
     * 
     * Quote removal should happen AFTER word splitting, inside build_argv.
     * For now, disable this to fix printf and other builtins.
     * 
     * TODO: Implement proper quote removal in build_argv/exec path.
     */
#if 0
    {
        int is_set_cmd = (strncmp(line, "set ", 4) == 0) ||
                         (strncmp(line, "set\t", 4) == 0) ||
                         (strcmp(line, "set") == 0);
        if (!is_set_cmd && jash_has_quotes(line)) {
            char unq_line[2048];
            int uql = jash_unquote(line, unq_line, sizeof(unq_line));
            if (uql >= 0) {
                line = unq_line;
            }
            /* uql < 0 → unterminated quote; leave line untouched so the
             * downstream command path reports a sensible error. */
        }
    }
#endif

simple_command:
    /* JINV-063 — o espaco em branco do FIM da linha e ESTRUTURA, e morre aqui.
     *
     * Este e o par simetrico do `line = skip_ws(line)` la em cima: um corta o
     * separador da esquerda, o outro o da direita. O corte tem que acontecer
     * NESTE ponto — depois dele `args` e desmontado (unquote em ~L3690, argv em
     * jash_builtin_dispatch), e ali `echo "x "` e `echo x␣` ja sao a mesma
     * string. Contrato e motivo completos em parser/quotes.h.
     *
     * Caminho quente: a saida rapida vive dentro de jash_rtrim_unquoted, entao
     * a linha comum (sem separador no fim) nao paga varredura nem buffer. O
     * buffer por profundidade e o mesmo mecanismo do `seg`/`unq_args`,
     * dimensionado pelo teto canonico de linha — nunca trunca (JINV-026/030):
     * se a copia nao couber, a linha segue INTACTA. */
    {
        size_t keep = jash_rtrim_unquoted(line);
        if (line[keep] != '\0') {
            static char* trim_slots[JASH_EVAL_MAX_DEPTH];
            char* tb = eval_depth_buf(trim_slots, JASH_LINE_CAP);
            if (tb && keep < (size_t)JASH_LINE_CAP) {
                memcpy(tb, line, keep);
                tb[keep] = '\0';
                line = tb;
            }
        }
    }

    char name[JASH_CMDWORD_MAX];  /* JINV-019 */
    const char* args = split_first(line, name, sizeof(name));

    /* BUG-028: POSIX quote removal applies to the command WORD too, not just
     * args. `"echo"`, `'echo'`, `"$CMD"` were resolved with the literal quotes
     * still attached → spurious `"echo": command not found`. Strip them once,
     * before alias/function/builtin/external lookup. Standalone assignments are
     * handled far earlier (~L1462), so `name` here is a genuine command word. */
    if (jash_has_quotes(name)) {
        extern int jash_unquote(const char* in, char* out, size_t out_size);
        char unq_name[JASH_CMDWORD_MAX];  /* JINV-019 */
        int unl = jash_unquote(name, unq_name, sizeof(unq_name));
        if (unl >= 0) memcpy(name, unq_name, (size_t)unl + 1);
    }

    if (strcmp(name, "cd..") == 0 && (!args || !*args)) {
        int rc = jash_b_cd(s, w, ctx, "..");
        s->last_status.code = rc;
        return rc;
    }

    /* Alias expansion (one level — no recursion to avoid loops). */
    const char* aval = jash_alias_get(s, name);
    if (aval) {
        /* Re-split using alias body + original args concatenated.
         * If alias body itself has a space, the part before is the new
         * command and the rest becomes prefix to args. */
        char alias_line[512];
        unsigned el = 0;
        while (aval[el] && el + 1 < sizeof(alias_line)) {
            alias_line[el] = aval[el]; el++;
        }
        if (*args && el + 1 < sizeof(alias_line)) alias_line[el++] = ' ';
        unsigned ai = 0;
        while (args[ai] && el + 1 < sizeof(alias_line)) {
            alias_line[el++] = args[ai++];
        }
        alias_line[el] = '\0';

        /* Re-split the expanded line — but DO NOT recurse alias lookup,
         * to keep things simple and prevent infinite loops. */
        const char* exp = skip_ws(alias_line);
        char ename[64];
        const char* eargs = split_first(exp, ename, sizeof(ename));
        /* Try built-in first. */
        const jash_builtin_t* b = jash_builtin_lookup(ename);
        if (b) {
            if (jash_guard_bypass_detect(ename, eargs, w, ctx)) {
                s->last_status.code = 126;
                return 126;
            }
            int rc = jash_builtin_dispatch(b, s, w, ctx, eargs);
            s->last_status.code = rc;
            return rc;
        }
        /* Then try PATH lookup via exec_external. */
        int rc = jash_exec_external(s, w, ctx, ename, eargs);
        s->last_status.code = rc;
        return rc;
    }

    /* v0.7: user-defined function call. Checked before builtins so a
     * function may shadow an external, matching bash. Positional args
     * $1..$N are set from `args`, the body is evaluated, then the old
     * positionals are restored. */
    {
        /* BUG-013: `command NAME` bypasses a shadowing function. Consume the
         * one-shot flag here (the resolution point) so it never leaks. */
        const char* fbody = NULL;
        if (s->skip_function_lookup) {
            s->skip_function_lookup = 0;
        } else {
            fbody = jash_function_lookup(s->functions, name);
        }
        if (fbody) return jash_eval_function(s, w, ctx, name, fbody, args);
    }

    /* Plain builtin lookup. */
    const jash_builtin_t* b = jash_builtin_lookup(name);
    if (b) {
        /* Quote removal for builtins without redirections.
         * Same logic as in builtin_redir.c: remove quotes from args
         * before passing to builtin.
         * EXCEPTION: printf and set do their own quote-aware parsing
         * (split_args / tokenise_set_args), so they receive raw args to
         * preserve word boundaries inside quoted strings —
         * `set -- "hello world" foo` must yield $1="hello world". */
        extern int jash_unquote(const char* in, char* out, size_t out_size);
        /* JINV-039: 16 KB fora da PILHA do ciclo de recursao — este bloco
         * roda a cada despacho de builtin, entao o buffer entrava no custo
         * por nivel de recursao de funcao (~300 KB/nivel, o que fixa o teto
         * de 32). Slot de heap por profundidade, mesmo mecanismo do `seg`
         * logo acima e do jash_expand_ctx_t (eval_depth_buf). */
        static char* unq_slots[JASH_EVAL_MAX_DEPTH];
        enum { JEVAL_UNQ_CAP = 16384 };
        char* unq_args = eval_depth_buf(unq_slots, JEVAL_UNQ_CAP);
        if (!unq_args) { s->last_status.code = 1; return 1; }
        const char* final_args = args;
        /* Assignment-style builtins (export/declare/readonly/local/typeset)
         * must see raw args so quoted values keep word boundaries:
         * `export FOO="multi word"` must reach export as `FOO="multi word"`,
         * not the unquoted `FOO=multi word` which the builtin would split. */
        /* ADR-0036 W2: any builtin migrated to the argv_fn calling convention
         * gets raw args unconditionally — jash_builtin_dispatch's own
         * quote-aware tokenizer (core/argv.c) handles it correctly, so
         * pre-unquoting here would just destroy boundaries before that
         * tokenizer ever sees them. Subsumes (rather than duplicates) the
         * per-name allowlist below for anything actually migrated. */
        int skip_unquote = (b->argv_fn != NULL) ||
                           jash_skip_unquote_builtin(name);
        if (!skip_unquote && args && jash_has_quotes(args)) {
            int unq_len = jash_unquote(args, unq_args, JEVAL_UNQ_CAP);
            if (unq_len >= 0) {
                final_args = unq_args;
            }
        } else if (!skip_unquote && args &&
                   (strchr(args, (char)0x03) || strchr(args, (char)0x04) ||
                    strchr(args, (char)0x05) || strchr(args, (char)0x06) ||
                    /* JINV-096: a licao do JINV-033 aplicada NA HORA — um
                     * sentinela novo obriga a varrer TODOS os pontos que testam
                     * a familia, nao so o que motivou a mudanca. Aqui o gatilho
                     * e o mesmo: um valor com `|` nao tem aspa nenhuma, entao
                     * jash_has_quotes e falso e so este resgate desblinda. */
                    jash_meta_shield_present(args))) {   /* JINV-096 */
            /* BUG-096: no quote removal ran (no syntactic quotes), but the
             * expansion shielded literal quote chars in a VALUE — restore
             * them before the builtin sees the args (echo $q). skip_unquote
             * builtins keep raw args: their own tokenizers unshield.
             *
             * JINV-033: o 0x06 faltava nesta lista, e o bug foi MEU. O JINV-009
             * criou o sentinela de ESPACO (para que `echo *` nao partisse
             * "meu arquivo.txt" em duas palavras) mas nao o acrescentou aqui.
             * Resultado: o resultado de um glob nao tem aspas nenhuma, entao
             * `jash_has_quotes` era falso, o ramo de remocao de aspas nao
             * rodava, este resgate nao reconhecia o 0x06 — e o sentinela
             * chegava ATE A SAIDA:
             *
             *   $ echo *.txt        (com um arquivo "com espaco.txt")
             *   com<0x06>espaco.txt      exibido como "comespaco.txt"
             *
             * O nome do arquivo saia corrompido, em silencio. `printf` nao
             * sofria porque esta na lista skip_unquote e desblinda por conta
             * propria — a discrepancia entre os dois builtins foi o que
             * denunciou o bug numa tabela de comparacao com o bash.
             *
             * Licao registrada no gate: acrescentar um sentinela novo exige
             * varrer TODOS os pontos que testam a familia, nao so o que
             * motivou a mudanca. */
            size_t al = strlen(args);
            if (al < JEVAL_UNQ_CAP) {
                memcpy(unq_args, args, al + 1);
                jash_quote_unshield(unq_args);
                final_args = unq_args;
            }
        }
        if (jash_guard_bypass_detect(name, final_args, w, ctx)) {
            s->last_status.code = 126;
            return 126;
        }
        /* ADR-0038 W1: read memoization for whitelisted read-only builtins
         * (grep/ls shadow their coreutils here). mode 2 = served, skip the
         * dispatch; mode 1 = candidate, tee the output and record it. */
        int memo_rc = 0;
        int memo_mode = jash_memo_begin(s, name, final_args ? final_args : "",
                                        w, ctx, &memo_rc);
        if (memo_mode == 2) {
            s->last_status.code = memo_rc;
            return memo_rc;
        }
        /* ADR-0044: output-budget governing for a single top-level builtin —
         * the same protection external commands get in jash_exec_external. Only
         * when stdout is NOT a tty AND a budget is set (JASH_LLM_BUDGET): cap the
         * shown bytes and spill the rest to ~/.jash/overflow (recall-able), so a
         * builtin like `cat bigfile` or `ls node_modules` never floods the agent
         * window. This site is single-command only — pipes reach pipeline.c and
         * file redirects reach builtin_redir.c, so both are untouched; cmdsub
         * captures are capped exactly as external commands already are. Wrap
         * order: builtin → memo tee (captures FULL for memoization) → budget wrap
         * (caps to the real writer). Only memoize when NOT truncated (a capped
         * view must never be served back as the command's full output).
         *
         * "single-command only" vale para a LINHA DO USUÁRIO, não para quem
         * re-entra aqui: `xargs` roda cada lote por jash_eval_line() de
         * propósito (atestação/undo/governo), e nesse ponto o fd 1 já é o pipe
         * ou o arquivo do redirect de cima. Só `!isatty` não distingue os dois,
         * então `xargs cat < lista > f` gravava 500 B de 2380000 B, rc=0.
         * jash_exec_stdout_is_session() é a pergunta que distingue: o fd 1
         * ainda é o stdout que a sessão tinha no arranque? Gate:
         * tests/test_builtin_governor.sh casos 7-9. */
        int use_budget = jash_budget_limit() > 0 && !isatty(STDOUT_FILENO) &&
                         jash_exec_stdout_is_session();
        jash_budget_wrap_t budget_wrap;
        jash_writer_t base_w = w;
        void* base_ctx = ctx;
        if (use_budget) {
            jash_budget_wrap_init(&budget_wrap, w, ctx, name);
            base_w = jash_budget_wrap_writer(&budget_wrap);
            base_ctx = &budget_wrap;
        }
        jash_memo_tee_t memo_tee;
        jash_writer_t dw = base_w;
        void* dctx = base_ctx;
        if (memo_mode == 1) {
            jash_memo_tee_init(&memo_tee, base_w, base_ctx);
            dw = jash_memo_tee_write;
            dctx = &memo_tee;
        }
        /* ADR-0042 F3: loopguard — anti-loop circuit breaker.
         * `!cmd` prefix overrides the block (attested as override in chain). */
        {
            int lg_override = (line[0] == '!');
            const char* lg_cmd = lg_override ? line + 1 : line;
            while (*lg_cmd == ' ') lg_cmd++;
            int lg = lg_override ? 0 : jash_loopguard_verdict(name, lg_cmd);
            if (lg == 2) {
                jash_writes(jash_stderr_writer, 0,
                    "\033[1;31m[jash loopguard] BLOCKED — 5 similar failures. "
                    "Use `!cmd` to override, or `recall failed` / `rewind auto`. "
                    "(JASH_LOOPGUARD=off to disable)\033[0m\n");
                s->last_status.code = 2;
                return 2;
            }
            if (lg == 1) {
                jash_writes(jash_stderr_writer, 0,
                    "\033[1;33m[jash loopguard] WARNING — similar command failing "
                    "repeatedly. Consider `recall failed` or `rewind auto`. "
                    "(JASH_LOOPGUARD=off to disable)\033[0m\n");
            }
        }
        int rc = jash_builtin_dispatch(b, s, dw, dctx, final_args);
        /* Emit the "[context-governor] output capped … recall X" footer (only
         * fires if the output actually overflowed the budget). */
        if (use_budget) jash_budget_wrap_finish(&budget_wrap, w, ctx);
        /* W1: only a run the agent saw IN FULL may be memoized — a capped view
         * must never be served back as "unchanged". */
        if (memo_mode == 1 && !(use_budget && budget_wrap.truncated))
            jash_memo_store_from_tee(s, name, final_args ? final_args : "",
                                     &memo_tee, rc);
        s->last_status.code = rc;
        copy_to(s->last_status.cmd, sizeof(s->last_status.cmd), line,
                (unsigned)((args ? (args + 0) : line + 0) - line) + 1);
        if (rc != 0)
            jash_run_trap_err(s, w, ctx);
        /* adoption (ADR-0040 W4): moment-of-relevance hints, budget-capped */
        jash_hint_post_exec(name, final_args, rc, w, ctx);
        jash_loopguard_record(name, line, rc);
        /* Predictions feed readline autosuggestions; main deliberately skips
         * their initialization in -c mode. Gate before constructing
         * the context: localtime() can stat /etc/localtime on EVERY command. */
        if (s->interactive && rc == 0 && name[0]) {
            char pred_ctx[PREDICT_CONTEXT_LEN];
            predict_make_context(pred_ctx, sizeof(pred_ctx), s->cwd);
            predict_record(&s->predictions, name, pred_ctx);
            predict_save(&s->predictions);
        }
        return rc;
    }

    /* Smart Jump: a single directory token means cd, but only after aliases,
     * functions, and builtins have had priority.
     *
     * JINV-115: and ONLY when interactive. This is bash's `autocd`, which the
     * manual pins to interactive shells alone — for a reason. In a script a
     * bare directory name is almost always a command that should have failed:
     * a typo, a binary that did not get built, a tool missing from $PATH.
     * Auto-cd'ing turns that into rc=0 plus a silently moved cwd, so `set -e`
     * does not fire, `&&` chains keep going, and every later relative path
     * resolves somewhere else. `make && dist` goes green having shipped
     * nothing. Non-interactive falls through to the PATH walk, which reports
     * the honest 127/126 below. */
    if (s->interactive && (!args || !*args) && is_directory_path(name)) {
        int rc = jash_b_cd(s, w, ctx, name);
        s->last_status.code = rc;
        return rc;
    }

    /* Module verb dispatch (ADR-0003 M2): try modules before PATH. */
#ifndef JASH_AGENT_ONLY
    if (g_modules.count > 0) {
        jmod_call_t mc = { s, (jmod_writer_t)w, ctx, name, args };
        int mrc = jmod_dispatch(name, &mc);
        if (mrc >= 0) {
            s->last_status.code = mrc;
            return mrc;
        }
    }
#endif

    /* Not a built-in — try PATH. */
    /* ADR-0042 F3: loopguard — anti-loop circuit breaker.
     * `!cmd` prefix overrides the block (attested as override in chain). */
    {
        char full_line[1280];
        if (args && *args) snprintf(full_line, sizeof(full_line), "%s %s", name, args);
        else snprintf(full_line, sizeof(full_line), "%s", name);
        int lg_override = (full_line[0] == '!');
        const char* lg_cmd = lg_override ? full_line + 1 : full_line;
        while (*lg_cmd == ' ') lg_cmd++;
        int lg = lg_override ? 0 : jash_loopguard_verdict(name, lg_cmd);
        if (lg == 2) {
            jash_writes(jash_stderr_writer, 0,
                "\033[1;31m[jash loopguard] BLOCKED — 5 similar failures. "
                "Use `!cmd` to override, or `recall failed` / `rewind auto`. "
                "(JASH_LOOPGUARD=off to disable)\033[0m\n");
            s->last_status.code = 2;
            return 2;
        }
        if (lg == 1) {
            jash_writes(jash_stderr_writer, 0,
                "\033[1;33m[jash loopguard] WARNING — similar command failing "
                "repeatedly. Consider `recall failed` or `rewind auto`. "
                "(JASH_LOOPGUARD=off to disable)\033[0m\n");
        }
    }
    int ext_rc = jash_exec_external(s, w, ctx, name, args);
    s->last_status.code = ext_rc;
    if (ext_rc != 0)
        jash_run_trap_err(s, w, ctx);
    /* adoption (ADR-0040 W4): moment-of-relevance hints, budget-capped */
    jash_hint_post_exec(name, args, ext_rc, w, ctx);
    {
        char full_line[1280];
        if (args && *args) snprintf(full_line, sizeof(full_line), "%s %s", name, args);
        else snprintf(full_line, sizeof(full_line), "%s", name);
        jash_loopguard_record(name, full_line, ext_rc);
    }
    /* Same REPL-only prediction boundary as the builtin path above. */
    if (s->interactive && ext_rc == 0 && name[0]) {
        char pred_ctx[PREDICT_CONTEXT_LEN];
        predict_make_context(pred_ctx, sizeof(pred_ctx), s->cwd);
        predict_record(&s->predictions, name, pred_ctx);
        predict_save(&s->predictions);
    }
    return ext_rc;
}

/* Background spawner (see forward declaration near the top of this file).
 * bg_line must already be stripped of the trailing `&`. */
static int jash_bg_spawn(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* bg_line) {
    /* Run in background via fork, in its OWN process group so the
     * whole job can be signaled with kill(-pgid, SIG). Parenthood
     * is what makes `wait`/`jobs`/`fg`/`kill %n`/exit-status work:
     * the shell must remain the parent to reap (POSIX job control;
     * bash forks a subshell for async lists for the same reason — a
     * bg line can be ANY shell code: builtins, functions, compounds,
     * lists, pipelines). */
    long bg_pid = sys_fork();
    if (bg_pid == 0) {
        /* Child: become leader of a new process group, then run.
         * setpgid(0,0) makes pgid == our pid; descendants inherit it. */
        setpgid(0, 0);
        /* Background jobs must not die from terminal signals; restore
         * default disposition so the command itself behaves normally
         * but is detached from the shell's SIGINT via the group. */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        /* No job control (non-interactive / agent `-c`): stdin comes
         * from /dev/null (bash/POSIX parity) so a background job can
         * neither steal the spawner's input nor block on it. An
         * explicit `<file` redirect applied later still wins.
         * Interactive shells keep the tty (the job gets SIGTTIN on
         * read, as usual). */
        if (!s->job_control_active) {
            int nfd = open("/dev/null", O_RDONLY);
            if (nfd >= 0) { dup2(nfd, 0); if (nfd > 0) close(nfd); }
        }
        /* Single external command: exec in place (same parse /
         * classify / PATH / expansion / assigns / redirects as the
         * pipeline fork child). No second fork, no waiter: with
         * stdout/stderr redirected the spawner's pipes are released
         * immediately (bash parity), while the child stays the job
         * leader so reaping/status are unchanged. */
        jash_pipeline_bg_exec(s, w, ctx, bg_line);
        int _bg_rc = jash_eval_line(s, w, ctx, bg_line);
        sys_exit(_bg_rc & 0xFF);
    }
    if (bg_pid > 0) {
        char bg_pid_buf[32];
        snprintf(bg_pid_buf, sizeof(bg_pid_buf), "%ld", bg_pid);
        jash_env_set(s, "!", bg_pid_buf);
        /* Parent: set the child's pgid too (race-free duplication of
         * the child's setpgid; one of them wins, both agree). */
        setpgid((pid_t)bg_pid, (pid_t)bg_pid);
        /* Register in job table keyed by pgid (== bg_pid). */
        if (!s->jobs) s->jobs = jash_job_table_create();
        if (s->jobs) {
            int jid = jash_job_add(s->jobs, (pid_t)bg_pid, bg_line);
            if (s->interactive)
                jash_writef(w, ctx, "[%d] %ld\n", jid, bg_pid);
        } else {
            if (s->interactive)
                jash_writef(w, ctx, "[1] %ld\n", bg_pid);
        }
    }
    return 0;
}

int jash_eval_function(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* name, const char* fbody, const char* args) {
    /* Parse $1..$N from already-expanded, quoted args first: when the
     * callee takes no args and the caller has no positionals, the whole
     * snapshot/replace/restore frame is provably a no-op and is skipped
     * (no calloc, no scans, no strdups). The parse itself never touches
     * the env table, so running it before the snapshot is safe. */
    int caller_pos_count = 0, caller_pos_max = 0;
    if (jash_env_positional_count(s, &caller_pos_count,
                                  &caller_pos_max) != 0)
        return 1;
    char* new_pos[JASH_POS_MAX] = {0};
    int idx = 0;
    {
        int parse_rc = jash_parse_function_positionals(args, new_pos, &idx);
        if (parse_rc != 0) {
            for (int i = 0; i < JASH_POS_MAX; i++) free(new_pos[i]);
            if (parse_rc == -2)
                jash_writef(jash_stderr_writer, 0,
                            "jash: %s: more than %d positional parameters — refusing\n",
                            name, JASH_POS_MAX);
            else
                jash_writef(jash_stderr_writer, 0,
                            "jash: %s: unable to store positional parameters — refusing\n",
                            name);
            return 1;
        }
    }
    int need_frame = (idx != 0 || caller_pos_count != 0);
    /* Save caller $1..$N. NULL entries record unset caller keys,
     * including gaps left by unset N. */
    uint32_t caller_saved_count = 0;
    char** saved = NULL;
    uint32_t prior_env_reservation = s->env_reserved_slots;
    if (need_frame) {
        saved = eval_snapshot_positionals(s, &caller_saved_count);
        if (!saved) {
            for (int i = 0; i < JASH_POS_MAX; i++) free(new_pos[i]);
            return 1;
        }
        if (caller_saved_count > s->env_reserved_slots)
            s->env_reserved_slots = caller_saved_count;
        if (jash_env_replace_positionals_owned_known(s, new_pos, idx,
                                                     caller_pos_count,
                                                     caller_pos_max) != 0) {
            for (int i = 0; i < JASH_POS_MAX; i++) free(new_pos[i]);
            eval_free_positionals(saved);
            s->env_reserved_slots = prior_env_reservation;
            jash_writef(jash_stderr_writer, 0,
                        "jash: %s: positional storage exhausted — refusing\n",
                        name);
            return 1;
        }
    }
    /* Run the body (may contain ; chains, other calls).
     *
     * JINV-086 — o TERCEIRO teto em série do caminho de função, e o
     * mais traiçoeiro dos três: aqui o corpo já tinha sido guardado
     * inteiro, e era a CHAMADA que o cortava. `malloc(2048)` com
     * `while (fbody[bl] && bl + 1 < 2048)` — `declare -f` mostrava o
     * corpo completo enquanto a execução rodava a metade de cima, o
     * que torna o defeito invisível para quem inspeciona.
     *
     * O corpo é avaliado in place: jash_eval_line recebe `const char*`
     * e nunca grava no buffer de entrada, então a cópia defensiva
     * (malloc+memcpy+free por chamada) era custo sem função. */
    const char* bodybuf = fbody;

    /* Native frame depth doubles as the recursion counter (was: env
     * get+atoi per call). FUNCNEST stays table-read (user knob). */
    int depth = s->func_depth;
    /* SECURITY: bound function-call recursion (bash FUNCNEST). Each
     * frame here is multi-KB of stack (calloc/malloc + 1KB+ buffers),
     * so the generic g_eval_depth=200 governor is reached only AFTER
     * the 8MB stack is already exhausted -> SIGSEGV. Cap nesting low
     * enough that a runaway `f(){ f; }; f` returns a clean error
     * instead of crashing the shell (and, in-kernel, the OS). */
    /* bash parity: FUNCNEST, when set to N>0, lowers the limit. */
    int func_max = JASH_FUNC_MAX_DEPTH;
    {
        const char* fn = jash_env_get(s, "FUNCNEST");
        if (fn && *fn) {
            int v = atoi(fn);
            if (v > 0 && v < func_max) func_max = v;
        }
    }
    if (depth >= func_max) {
        /* BUG-057: report on the SHELL's stderr, never the passed-in
         * writer — inside $(...) that writer is the capture buffer,
         * so the old message was swallowed and the caller computed
         * arithmetic on an empty substitution (silently wrong). */
        jash_writef(jash_stderr_writer, 0,
            "jash: %s: maximum function recursion depth (%d) exceeded\n",
            name, func_max);
        if (need_frame) {
            eval_restore_positionals(s, saved);
            s->env_reserved_slots = prior_env_reservation;
        }
        s->last_status.code = 1;
        return 1;
    }
    /* Push the native frame: depth + FUNCNAME go native (no table
     * traffic); the locals dance below stays table-managed. */
    if (jash_frame_enter(s, name) != 0) {
        jash_writef(jash_stderr_writer, 0,
            "jash: %s: sem memoria para o frame da funcao — ela NAO foi executada\n",
            name);
        if (need_frame) {
            eval_restore_positionals(s, saved);
            s->env_reserved_slots = prior_env_reservation;
        }
        s->last_status.code = 1;
        return 1;
    }
    char depth_key[64];
    snprintf(depth_key, sizeof(depth_key), "__LOCAL_VARS_%d", depth);
    const char *saved_local_list = jash_env_get(s, "__LOCAL_VARS__");
    if (saved_local_list) {
        jash_env_set_nomirror(s, depth_key, saved_local_list);
        /* Also save all __local_<name> values for this depth */
        const char *lp = saved_local_list;
        while (*lp) {
            while (*lp == ' ') lp++;
            if (!*lp) break;
            const char *start = lp;
            while (*lp && *lp != ' ') lp++;
            char save = *lp; *(char*)lp = 0;
            char local_key[128];
            snprintf(local_key, sizeof(local_key), "__local_%s", start);
            const char *val = jash_env_get(s, local_key);
            if (val) {
                char depth_local_key[192];
                snprintf(depth_local_key, sizeof(depth_local_key), "__local_%s_%d", start, depth);
                jash_env_set(s, depth_local_key, val);
            }
            *(char*)lp = save;
            if (save) lp++;
        }
    } else {
        jash_env_set_nomirror(s, depth_key, "");
    }

    /* Clear local variables list before function execution */
    jash_env_set_nomirror(s, "__LOCAL_VARS__", "");
    /* Depth + FUNCNAME push/pop live in the native frame now (pushed by
     * jash_frame_enter above); the table is untouched. */
    int frc = jash_eval_line(s, w, ctx, bodybuf);
    /* BUG-012: consume the `return` signal at this function boundary so
     * it does not leak into the caller's command list; the function's
     * status becomes the return code. This is what makes early `return`
     * (incl. `... && return`) actually unwind — fixes infinite recursion. */
    if (s->return_requested) {
        s->return_requested = 0;
        frc = s->return_code;
        s->last_status.code = frc;
    }
    /* Pop the native frame (depth + FUNCNAME); table slots a sync may
     * have clobbered are restored from the enter-time snapshot. */
    jash_frame_exit(s);
    /* Restore local variables */
    const char *local_list = jash_env_get(s, "__LOCAL_VARS__");
    if (local_list && *local_list) {
        char list_copy[1024];
        unsigned lci = 0;
        while (local_list[lci] && lci + 1 < sizeof(list_copy)) {
            list_copy[lci] = local_list[lci]; lci++;
        }
        list_copy[lci] = 0;
        char *lp = list_copy;
        while (*lp) {
            while (*lp == ' ') lp++;
            if (!*lp) break;
            char *start = lp;
            while (*lp && *lp != ' ') lp++;
            char save = *lp; *lp = 0;
            /* BUG-102: a bare NAME token (no '[') covers the ARRAY
             * too — sweep-unset every NAME[...] element created
             * inside the function BEFORE restoring; the snapshot
             * element tokens that follow re-add the global values. */
            if (!strchr(start, '[')) {
                size_t snl = strlen(start);
                int removed;
                do {
                    removed = 0;
                    uint32_t en = jash_env_count(s);
                    for (uint32_t ei = 0; ei < en; ei++) {
                        const char *ek = 0, *ev = 0;
                        if (!jash_env_at(s, ei, &ek, &ev)) continue;
                        if (strncmp(ek, start, snl) == 0 && ek[snl] == '[' &&
                            strncmp(ek, "__local_", 8) != 0) {
                            char ekb[128];
                            snprintf(ekb, sizeof(ekb), "%s", ek);
                            jash_env_unset(s, ekb);
                            removed = 1;
                            break;
                        }
                    }
                } while (removed);
            }
            /* Restore from __local_<name> */
            char local_key[128];
            int li = 0;
            const char *lk = "__local_";
            while (*lk && li < 120) local_key[li++] = *lk++;
            {
                int vi = 0;
                while (start[vi] && li < 120) local_key[li++] = start[vi++];
            }
            local_key[li] = 0;
            const char *saved_val = jash_env_get(s, local_key);
            if (saved_val) {
                if (strcmp(saved_val, "__UNSET__") == 0) {
                    jash_env_unset(s, start);
                } else {
                    jash_env_set(s, start, saved_val);
                }
                jash_env_unset(s, local_key);
            }
            *lp = save;
            if (save) lp++;
        }
    }
    jash_env_unset_nomirror(s, "__LOCAL_VARS__");

    /* Restore local variables context from function depth (`depth`
     * still holds the caller's level: the native frame owns the counter
     * now, no env traffic here). */
    snprintf(depth_key, sizeof(depth_key), "__LOCAL_VARS_%d", depth);
    const char *restored_local_list = jash_env_get(s, depth_key);
    if (restored_local_list && *restored_local_list) {
        jash_env_set_nomirror(s, "__LOCAL_VARS__", restored_local_list);
        /* Also restore all __local_<name> values for this depth */
        const char *lp = restored_local_list;
        while (*lp) {
            while (*lp == ' ') lp++;
            if (!*lp) break;
            const char *start = lp;
            while (*lp && *lp != ' ') lp++;
            char save = *lp; *(char*)lp = 0;
            char depth_local_key[192];
            snprintf(depth_local_key, sizeof(depth_local_key), "__local_%s_%d", start, depth);
            const char *val = jash_env_get(s, depth_local_key);
            if (val) {
                char local_key[128];
                snprintf(local_key, sizeof(local_key), "__local_%s", start);
                jash_env_set(s, local_key, val);
                jash_env_unset(s, depth_local_key);
            }
            *(char*)lp = save;
            if (save) lp++;
        }
    } else {
        jash_env_unset_nomirror(s, "__LOCAL_VARS__");
    }
    jash_env_unset_nomirror(s, depth_key);

    /* Run RETURN trap after function body. */
    jash_run_trap_return(s, w, ctx);
    /* Restore positionals (skipped with the frame when both sides are
     * empty: nothing was saved, replaced, or reserved). */
    if (need_frame) {
        if (eval_restore_positionals(s, saved) != 0) frc = 1;
        s->env_reserved_slots = prior_env_reservation;
    }
    s->last_status.code = frc;
    return frc;
}

int jash_get_lineno(void) {
    return g_lineno;
}
