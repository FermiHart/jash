// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: function_defs — user-defined functions, function registry, return

#ifndef JASH_FUNCTION_DEFS_H
#define JASH_FUNCTION_DEFS_H

#include <core/limits.h>

/* ═══ JINV-086 — o corpo de uma função não pode ter teto mudo ═══════════════
 *
 * `char body[2048]` com `strncpy(..., sizeof(body) - 1)`: um corpo com mais
 * de 2047 bytes tinha a CAUDA APAGADA e a função era registrada assim mesmo.
 * Chamá-la devolvia rc 0, stderr VAZIO, e executava um programa que ninguém
 * escreveu. Penhasco medido (arquivo inteiro, `f(){ echo INICIO; <recheio>;
 * echo FIM; }; f`):
 *
 *     1835 B  →  INICIO/FIM   (igual ao bash)
 *     2735 B  →  INICIO       (o `echo FIM` sumiu, rc 0, nada no stderr)
 *
 * É a pior classe de defeito que resta no projeto, e é PIOR que o do JINV-080:
 * ali o corte às vezes gritava (`missing done`); aqui a função continua
 * sintaticamente válida depois de decapitada, então nada tem como gritar.
 * Uma função de 2 KB é tamanho de todo dia — todo `configure` de autoconf
 * define várias maiores que isso.
 *
 * Havia DOIS tetos em série no mesmo caminho, e consertar só um não move
 * nada: o buffer de PARSE (`char fbody[2048]` em `parser/eval.c`, com
 * `if (blen >= bcap) blen = bcap - 1;`) e este, o de ARMAZENAMENTO.
 *
 * Conserto (idioma do JINV-080/082): sem teto. Corpo até 2047 bytes continua
 * INLINE na entrada da tabela — caso comum, zero `malloc`, mesmo perfil de
 * memória de antes; acima disso, heap do tamanho EXATO. Falta de memória é
 * recusa ALTA (`JASH_FUNC_ENOMEM` → stderr + rc≠0, a função NÃO é definida),
 * nunca um corpo pela metade.
 *
 * ── de onde vem o número ────────────────────────────────────────────────
 * `JASH_FN_BODY_INLINE` NÃO é um teto novo: é o tamanho antigo mantido como
 * LIMIAR DE DESEMPENHO (o ponto onde o corpo deixa de caber inline e passa a
 * custar um `malloc`). Nenhum dado se perde acima dele. A relação que ele
 * precisa respeitar é a de `core/limits.h` — um limiar de armazenamento
 * inline nunca pode ser maior que o teto de pilha declarado da casa, porque
 * `jash_function` é copiada por valor no `delete` — e essa relação é
 * verificada abaixo, em tempo de compilação, exatamente como limits.h faz com
 * os seus.
 *
 * Gate: tests/test_jinv086_silent_caps.sh */
#define JASH_FN_BODY_INLINE 2048

#if JASH_FN_BODY_INLINE > JASH_LINE_CAP_STACK
#error "JASH_FN_BODY_INLINE nao pode exceder JASH_LINE_CAP_STACK (ver core/limits.h)"
#endif

/* Códigos de retorno de `jash_function_define` — separados de propósito:
 * "tabela cheia" e "sem memória" são recusas ALTAS diferentes, e quem chama
 * precisa dizer qual foi. `-1` continua sendo falha genérica para não quebrar
 * um teste de `< 0` já existente. */
#define JASH_FUNC_OK      0
#define JASH_FUNC_EFULL  (-1)   /* tabela cheia / argumento inválido */
#define JASH_FUNC_ENOMEM (-2)   /* JINV-086: sem memória para o corpo */

/* A user-defined function entry. */
typedef struct {
    char name[64];                    /* Function name */
    char body[JASH_FN_BODY_INLINE];   /* corpo curto — caminho rápido, zero malloc */
    /* JINV-086: != NULL quando o corpo não coube em `body`. Quando não é NULL,
     * ELE é o corpo e `body` está vazio. Invariante de posse: todo slot com
     * índice >= `count` tem `body_heap == NULL` (garantido pelo `init` e pelo
     * `delete`), então registrar uma função nova nunca sobrescreve um ponteiro
     * vivo nem libera um pendurado. */
    char* body_heap;
} jash_function;

/* Function table (max 32 functions). Public so callers can embed a
 * table by value (e.g. a process-local static) without a heap alloc. */
typedef struct jash_function_table {
    jash_function funcs[32];
    int count;
} jash_function_table;

/* JINV-086: a ÚNICA leitura correta do corpo de uma entrada. Ler
 * `.body` direto passa a mentir para todo corpo acima de
 * JASH_FN_BODY_INLINE (fica vazio), então quem varre a tabela sem passar por
 * `jash_function_lookup` tem que usar isto. */
static inline const char* jash_function_body_of(const jash_function* f) {
    return f->body_heap ? f->body_heap : f->body;
}

/* Initialize function table */
void jash_function_table_init(jash_function_table* table);

/* Register a function: name() { body } */
/* Returns: JASH_FUNC_OK, JASH_FUNC_EFULL (tabela cheia/argumento inválido) ou
 * JASH_FUNC_ENOMEM (JINV-086: sem memória para o corpo). Nas duas falhas a
 * tabela fica INALTERADA — nunca com um corpo pela metade. */
int jash_function_define(jash_function_table* table, const char* name, const char* body);

/* Look up a function by name */
/* Returns: function body as string, or NULL if not found */
const char* jash_function_lookup(jash_function_table* table, const char* name);

/* Check if name is a defined function */
int jash_function_exists(jash_function_table* table, const char* name);

/* List all functions */
void jash_function_list(jash_function_table* table);

/* Delete a function */
void jash_function_delete(jash_function_table* table, const char* name);

#endif
