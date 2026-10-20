// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/export — set environment variables.

#include "builtin.h"
#include "shell_output.h"
#include <core/timeline.h>
#include <stdio.h>
#include <ctype.h>
#include <core/env.h>
#include <theme/colors.h>
#include <string.h>   /* JINV-086: strlen do valor sem teto */
#include <stdlib.h>   /* JINV-086: malloc/free do valor sem teto */

/* ═══ JINV-077 — `export NOME` sem `=valor` ══════════════════════════════
 *
 * POSIX (XCU, `export`) e o bash aceitam `export nome...`: marcar para
 * exportação uma variável que já existe. É a forma MAIS COMUM em script
 * real — atribui-se numa linha, exporta-se noutra:
 *
 *     CC=gcc
 *     export CC
 *
 * O jash respondia `export: usage: export KEY=VALUE` no stderr e devolvia
 * rc 2 para TODA ocorrência. Não era cosmético: em
 * `audits/BENCHMARK-scripts-reais.md`, das 38 invocações de `configure`
 * (`--help`/`--version`), **22 morriam exatamente aqui** — era o primeiro
 * erro dominante da amostra. Autoconf emite `export` nu dezenas de vezes
 * por script.
 *
 * Por que o no-op é a semântica CERTA no jash, e não uma gambiarra: a
 * tabela de env do jash é plana e já é a environment do filho — `V=1;
 * sh -c 'echo $V'` imprime `1` sem `export` nenhum. Logo, uma variável que
 * existe já está exportada, e `export NOME` não tem trabalho a fazer. Para
 * um nome que NÃO existe, o bash também não cria nada (`export NOPE; echo
 * ${NOPE-unset}` imprime `unset`, e `env` não mostra `NOPE`) — o no-op
 * coincide com o oráculo nos dois casos. O que muda é o contrato
 * observável: **rc 0 e stderr limpo.**
 *
 * O nome ainda é VALIDADO: `export 1bad` / `export a-b` / `export ""` são
 * `not a valid identifier` com rc 1, como no bash. Aceitar qualquer lixo em
 * silêncio seria trocar um erro barulhento por uma mentira silenciosa.
 *
 * Gate: tests/test_jinv077_export_bare.sh */
static int export_is_name(const char* k) {
    if (!k || !*k) return 0;
    if (!(isalpha((unsigned char)k[0]) || k[0] == '_')) return 0;
    for (const char* p = k + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}

/* Print one var as `export KEY='value'` with POSIX-safe single-quote escaping
 * (any `'` in value becomes `'\''`). Output is reusable as shell input. */
static void export_print_one(jash_writer_t w, void* ctx,
                              const char* k, const char* v) {
    jash_writes(w, ctx, "export ");
    jash_writes(w, ctx, k);
    jash_writes(w, ctx, "='");
    for (const char* p = v; *p; p++) {
        if (*p == '\'') {
            jash_writes(w, ctx, "'\\''");
        } else {
            char tmp[2] = { *p, '\0' };
            jash_writes(w, ctx, tmp);
        }
    }
    jash_writes(w, ctx, "'\n");
}

/* export       — list (same as env)
 * export -p    — dump as reusable `export K=V` lines (POSIX)
 * export K=V   — set
 * export -n K  — unset
 *
 * No quoting yet on set: V is the rest of the line after '='. */
int jash_b_export(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "export", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    if (!args || !*args) return jash_b_env(s, w, ctx, "");

    while (*args == ' ' || *args == '\t') args++;

    /* -p (no operand) : dump in reusable `export K=V` form */
    if (args[0] == '-' && args[1] == 'p' &&
        (args[2] == '\0' || args[2] == ' ' || args[2] == '\t')) {
        uint32_t n = jash_env_count(s);
        for (uint32_t i = 0; i < n; i++) {
            const char *k = 0, *v = 0;
            if (!jash_env_at(s, i, &k, &v)) continue;
            export_print_one(w, ctx, k, v);
        }
        return 0;
    }

    /* JINV-077: `export -n` sem operando é um no-op de rc 0 no bash. Antes
     * caía no laço de atribuições abaixo, virava a "chave" `-n` e saía com
     * o mesmo `usage` + rc 2. */
    if (args[0] == '-' && args[1] == 'n' && args[2] == '\0') return 0;

    if (args[0] == '-' && args[1] == 'n' &&
        (args[2] == ' ' || args[2] == '\t')) {
        args += 3;
        while (*args == ' ' || *args == '\t') args++;
        /* JINV-077: `export -n A B C` — o bash aceita VÁRIOS nomes, e não
         * reclama de nome que não estava exportado (rc 0). Antes a linha
         * inteira virava UM nome ("A B C"), o unset falhava e o rc era 1 —
         * um erro sobre um nome que ninguém escreveu.
         *
         * DIVERGÊNCIA CONHECIDA e deliberada: o bash mantém o VALOR da
         * variável (só tira da environment do filho); o jash tem uma tabela
         * plana — sem bit de exportação — então tirar do filho é remover.
         * Consertar isso exige um flag por entrada em `jash_env_entry_t` e
         * filtragem na montagem do envp do exec; é uma onda própria, não um
         * detalhe deste conserto. O gate afirma só o que o jash sustenta:
         * rc 0, stderr limpo, e a variável fora do ambiente do filho. */
        int nrc = 0;
        while (*args) {
            char nm[JASH_ENV_KEY_LEN];
            int ni = 0;
            while (*args && *args != ' ' && *args != '\t' &&
                   ni < (int)sizeof(nm) - 1) nm[ni++] = *args++;
            nm[ni] = '\0';
            /* nome maior que a tabela: consome o resto do token para não
             * deixar a cauda virar um "próximo nome" fantasma. */
            while (*args && *args != ' ' && *args != '\t') args++;
            while (*args == ' ' || *args == '\t') args++;
            if (!ni) continue;
            if (!export_is_name(nm)) {
                jash_writef(jash_stderr_writer, 0,
                            "%sexport: `%s': not a valid identifier%s\n",
                            J_ROLE_ERR, nm, J_RESET);
                nrc = 1;
                continue;
            }
            jash_env_unset(s, nm);   /* ausente = no-op silencioso (bash) */
        }
        return nrc;
    }

    /* Process multiple assignments: export A=1 B=2 C=3 */
    int overall_rc = 0;
    /* JINV-062 — guarda de progresso desta volta.
     *
     * Antes do JINV-077, todo caminho que nao encontrava `=` fazia `return`:
     * a volta nunca recomecava sem ter consumido. Agora ela `continue`ia (nome
     * nu, nome invalido, nome longo demais), e o avanco passou a ser delegado
     * as varreduras internas — cujo conjunto de PARADA ('=') nao esta contido
     * no de PULO (' ', '\t'). Se alguma delas parar sem consumir, a volta
     * recomeca IDENTICA: laco infinito, o shell trava, e nao ha saida para
     * quem chamou. E a familia do JINV-057 (`resume` travando num `)`).
     *
     * A guarda custa uma comparacao e torna o travamento impossivel por
     * construcao — independente de quantos `continue` esta volta venha a ter. */
    const char* iter_prev = NULL;
    while (*args) {
        if (args == iter_prev) args++;   /* a volta anterior nao consumiu nada */
        iter_prev = args;
        while (*args == ' ' || *args == '\t') args++;
        if (!*args) break;

        /* Find the key (up to '=' or whitespace) */
        char key[64];
        int ki = 0;
        while (*args && *args != '=' && *args != ' ' && *args != '\t'
               && ki < (int)sizeof(key) - 1) {
            key[ki++] = *args++;
        }
        key[ki] = '\0';

        if (*args != '=') {
            /* JINV-077: `export NOME` — a forma POSIX sem `=valor`. */

            /* Nome maior que a tabela (JASH_ENV_KEY_LEN). A extração acima
             * PAROU no meio do identificador; se a cauda voltasse ao laço
             * ela viraria um segundo "nome" (e, num `export <70chars>=v`,
             * gravaria a chave ERRADA). Consome o token inteiro e grita —
             * truncar em silêncio é pior que recusar alto. */
            if (ki == (int)sizeof(key) - 1 && *args &&
                *args != ' ' && *args != '\t') {
                while (*args && *args != ' ' && *args != '\t') args++;
                jash_writef(jash_stderr_writer, 0,
                            "%sexport: %s...: name too long (max %d)%s\n",
                            J_ROLE_ERR, key, (int)sizeof(key) - 1, J_RESET);
                overall_rc = 1;
                continue;
            }

            if (!export_is_name(key)) {
                jash_writef(jash_stderr_writer, 0,
                            "%sexport: `%s': not a valid identifier%s\n",
                            J_ROLE_ERR, key, J_RESET);
                overall_rc = 1;
                continue;
            }

            /* Tabela plana: toda variável já é herdada pelo filho, então
             * marcar para exportação não tem trabalho a fazer — nem para um
             * nome inexistente, que o bash também não cria. rc 0, sem ruído. */
            continue;
        }
        args++;  /* consume = */

        /* Quote-aware value parse. The whole token (until top-level
         * whitespace) is the value; quoted spans contribute their inner
         * chars literally. POSIX `export FOO="a b"` becomes FOO=`a b`. */
        /* ═══ JINV-086 — `export V=…` parava em JASH_ENV_VAL_LEN-1 ═════════
         *
         *     export V=<8192 a's>; echo ${#V}   jash 8191 · bash 8192
         *
         * rc 0, stderr vazio. O armazenamento em si nunca teve teto
         * (`heap_val`, core/state.h) — só este buffer de análise tinha, e
         * `JASH_ENV_VAL_LEN` está declarado em `core/limits.h` como teto de
         * PILHA, não do dado (o próprio comentário lá diz que é "POLÍTICA,
         * não layout").
         *
         * O resultado desta análise nunca é MAIOR que a entrada (ela só
         * remove aspas e escapes), então `strlen(args) + 1` é um tamanho
         * exato que jamais estoura. Os 8 KiB de pilha viram limiar de
         * desempenho; acima disso, heap exato. Sem memória: recusa ALTA.
         *
         * Gate: tests/test_jinv086_silent_caps.sh */
        char  value_inline[JASH_ENV_VAL_LEN];
        char* value_heap = NULL;
        char* value = value_inline;
        size_t value_cap = sizeof(value_inline);
        if (strlen(args) + 1 > value_cap) {
            value_heap = (char*)malloc(strlen(args) + 1);
            if (!value_heap) {
                jash_writef(jash_stderr_writer, 0,
                            "%sexport: sem memoria para o valor — %s NAO foi exportada%s\n",
                            J_ROLE_ERR, key, J_RESET);
                return 2;
            }
            value = value_heap;
            value_cap = strlen(args) + 1;
        }
        int vi = 0;
        char quote = 0;
        while (*args && vi < (int)value_cap - 1) {
            char c = *args;
            if (quote) {
                if (c == quote) { quote = 0; args++; continue; }
                if (quote == '"' && c == '\\' && args[1]) {
                    /* Inside "...": backslash only escapes $ ` " \ <newline> */
                    char nx = args[1];
                    if (nx == '$' || nx == '`' || nx == '"' ||
                        nx == '\\' || nx == '\n') {
                        if (nx == '\n') { args += 2; continue; }
                        value[vi++] = nx;
                        args += 2;
                        continue;
                    }
                }
                value[vi++] = c;
                args++;
                continue;
            }
            if (c == '\'' || c == '"') { quote = c; args++; continue; }
            if (c == '\\' && args[1]) {
                value[vi++] = args[1];
                args += 2;
                continue;
            }
            if (c == ' ' || c == '\t') break;
            value[vi++] = c;
            args++;
        }
        value[vi] = '\0';

        /* Timeline: snapshot the previous value BEFORE overwriting. */
        {
            const char* prev = jash_env_get(s, key);
            char desc[120];
            if (prev) {
                snprintf(desc, sizeof(desc), "export %s (was set)", key);
                jash_timeline_record(JASH_UNDO_ENV_SET, desc, key, prev);
            } else {
                snprintf(desc, sizeof(desc), "+export %s (was unset)", key);
                jash_timeline_record(JASH_UNDO_ENV_SET, desc, key, "");
            }
        }

        int rc = jash_env_set(s, key, value);
        if (value_heap) free(value_heap);   /* JINV-086 */
        if (rc != 0) {
            jash_writef(jash_stderr_writer, 0, "%sexport: failed (%d)%s\n", J_ROLE_ERR, rc, J_RESET);
            overall_rc = 1;
        }
    }
    return overall_rc;
}
