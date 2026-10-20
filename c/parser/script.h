// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/script — if/for/while/case statement evaluation.

#ifndef JASH_PARSER_SCRIPT_H
#define JASH_PARSER_SCRIPT_H

#include <core/state.h>
#include <theme/io.h>
#include <stddef.h>

/* ═══ JINV-072 + JINV-083 — a ÚNICA leitura de palavra-chave da casa ═══════
 *
 * Uma palavra-chave só é palavra-chave quando é PALAVRA INTEIRA (fronteira
 * dos dois lados), está FORA DE ASPAS, e — para as que abrem/fecham bloco —
 * está em POSIÇÃO DE COMANDO. As três juntas, ou nenhuma serve:
 *
 *   fronteira sem aspas  →  `echo "ask for help"` abre um laço (JINV-083)
 *   aspas sem fronteira  →  `echo fordone` fecha um laço (JINV-083)
 *   as duas, sem posição →  `echo done` fecha um laço (JINV-083)
 *   nem uma nem outra    →  `echo first` fechava um `if` (JINV-072)
 *
 * `jash_find_keyword_set` é o scanner; tudo mais neste cabeçalho é conveniência
 * em cima dele. NÃO escreva uma segunda varredura — a auditoria achou sete
 * pares "duas implementações da mesma coisa, uma empobrecida"; este arquivo
 * existe para que não haja um oitavo. */

/* Terminadores aceitos DEPOIS da palavra (o fim da cadeia sempre vale). */
#define JASH_KW_END_ANY   " \t\n;&|)"  /* o conjunto histórico do JINV-072 */
#define JASH_KW_END_OPEN  " \t("       /* for/while/until/select — EXIGEM argumento
                                        * (`for)` e `for;` nunca são palavra-chave;
                                        * `(` é o `for((`/`while((` sem espaço) */
#define JASH_KW_END_CLOSE " \t\n;"     /* done/fi/esac — fecham um enunciado */
#define JASH_KW_END_BRACE " \t\n"      /* `{` — EXIGE branco depois (POSIX 2.9:
                                        * `{` é palavra reservada, então `{echo`
                                        * é o COMANDO `{echo`, e `{a,b}` é
                                        * expansão de chaves, não um grupo) */

typedef struct {
    const char* word;    /* a palavra procurada */
    const char* endset;  /* terminadores aceitos; NULL = JASH_KW_END_ANY */
    int cmdpos;          /* 1 = só vale em POSIÇÃO DE COMANDO (POSIX 2.9) —
                          * início da região, ou depois de `; \n & | ( ) { }`,
                          * ou depois de `do`/`then`/`else`/`elif`. É o que faz
                          * `echo done` ser DADO e `; done` ser ESTRUTURA. */
} jash_kw_t;

/* Ocorrência MAIS À ESQUERDA de qualquer palavra de `kws`, fora de aspas e com
 * fronteira de palavra dos dois lados. `*which` recebe o índice em `kws`,
 * `*after` o ponto logo depois da palavra (nenhum chamador precisa recontar
 * bytes). Devolve 0 quando não há. Ambos os ponteiros de saída são opcionais. */
const char* jash_find_keyword_set(const char* hay, const jash_kw_t* kws, size_t nk,
                                  size_t* which, const char** after);

/* Uma palavra só, com JASH_KW_END_ANY — o que `find_keyword` sempre foi. */
const char* jash_find_keyword(const char* hay, const char* kw, const char** after);

/* A palavra `kw` começa EXATAMENTE em `p`, como PALAVRA? (`endset` NULL =
 * JASH_KW_END_ANY.) Para quem já está posicionado num começo de cláusula e só
 * precisa da fronteira à direita — sem escrever um `strncmp` nu ao lado. */
int jash_keyword_at(const char* p, const char* kw, const char* endset);

/* `p` está em POSIÇÃO DE COMANDO dentro de `region`? A mesma regra de
 * `cmdpos`, lida para TRÁS — para quem já tem a própria varredura com estado e
 * não pode adotar o andar de `jash_find_keyword_set`. As duas formas moram no
 * mesmo arquivo, de propósito. */
int jash_at_cmd_pos(const char* region, const char* p);

/* O `done` que FECHA o laço já aberto em `body` (profundidade inicial 1),
 * contando aninhamento de for/while/until/select. Devolve 0 se não houver.
 *
 * JINV-083: existiam QUATRO cópias desta varredura (for_loop ×2, while_loop,
 * select_loop) e as quatro liam o texto cru — a de `select` sem sequer exigir
 * fronteira de palavra. Agora há uma. */
const char* jash_find_loop_close(const char* body);

/* ═══ JINV-084 — o `}` que fecha o GRUPO DE CHAVES já aberto ═══════════════
 *
 * `body` é o texto logo DEPOIS do `{` (profundidade inicial 1), exatamente
 * como `jash_find_loop_close`. Devolve o `}` que fecha, ou 0 se não houver.
 *
 * `{` e `}` não são palavras-chave comuns e é por isso que este scanner é o
 * MESMO e não um nono:
 *
 *   - `}` só fecha em POSIÇÃO DE COMANDO. Sem isso, `echo }` fecharia o grupo
 *     e o `case x in }) …` do autoconf viraria estrutura em vez de padrão.
 *   - `{` exige BRANCO depois (JASH_KW_END_BRACE). Sem isso, `{a,b}` (expansão
 *     de chaves) e `{echo` (um comando com esse nome, no bash também) abririam
 *     um grupo que ninguém fecha.
 *   - fora de aspas: `echo "{"` é DADO. E `${VAR}` sobrevive de graça — o `{`
 *     colado no `$` não tem fronteira de palavra à esquerda, então o scanner
 *     nem olha para ele.
 *
 * A contagem cega que existia em eval.c (`if (*p=='{') depth++;`) errava as
 * quatro. É a família do JINV-016/017/018/019/072/083: DADO virando ESTRUTURA
 * por leitura cega de texto. */
const char* jash_find_brace_close(const char* body);

/* Evaluate if-then-else-fi statement. Returns exit code of executed branch. */
int jash_eval_if_statement(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* if_line);

/* Evaluate if condition (a normal command). Returns 1 if true, 0 if false. */
int jash_eval_if_condition(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* condition_line);

/* Evaluate for VAR in WORDS; do BODY; done */
int jash_eval_for_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* for_line);

/* Evaluate for (( expr1; expr2; expr3 )); do BODY; done */
int jash_eval_for_c_style(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* for_line);

/* Evaluate while COND; do BODY; done */
int jash_eval_while_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* while_line);

/* Evaluate case EXPR in PATTERN) CMD ;; ... esac */
int jash_eval_case_stmt(jash_state_t* s, jash_writer_t w, void* ctx,
                        const char* case_line);

/* Evaluate select VAR in WORDS; do BODY; done */
int jash_eval_select_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* select_line);

/* Evaluate multi-line if/elif/else/fi block. Returns exit code of executed branch. */
int jash_eval_if_block(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* if_line);

/* Evaluate until COND; do BODY; done */
int jash_eval_until_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* until_line);

/* Parse and evaluate a multi-line script string (heredocs, functions, etc.).
 * Used by source, eval, and cmdsub builtins. */
int jash_run_multiline(jash_state_t* state, const char* sp);

#endif /* JASH_PARSER_SCRIPT_H */
