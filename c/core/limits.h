// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/limits — os tetos de capacidade de linha, num lugar só.
//
// JINV-030. Antes deste arquivo, cada estágio escolhia o seu número, e a
// incoerência entre eles é o que produzia os cortes silenciosos documentados
// em `audits/ACHADO-buffers-fixos.md`:
//
//   - a substituição de comando declarava 131072 bytes de capacidade;
//   - a expansão de parâmetro declarava 131072;
//   - mas o SEGMENTO de pipeline que recebia os dois aceitava 8192.
//
// A capacidade dos dois primeiros era, portanto, ficção — o excedente sumia
// sem aviso. O mesmo valor aparecia escrito de quatro maneiras (`131072`,
// `128 * 1024`, `131073`, `8192`), o que tornava a incoerência invisível numa
// leitura casual.
//
// A regra que este arquivo existe para tornar verificável:
//
//     um estágio nunca pode aceitar MENOS do que quem o alimenta produz.
//
// Onde um teto TEM que ser menor, a razão está escrita ao lado — nunca um
// número solto. Hoje há exatamente duas razões legítimas para ser menor, e as
// duas são sobre memória de PILHA, não sobre o dado.
//
// F E R M I ∞ H A R T

#ifndef JASH_CORE_LIMITS_H
#define JASH_CORE_LIMITS_H

/* ── O teto canônico ──────────────────────────────────────────────────────
 * Quanto texto uma linha lógica pode carregar depois de todas as expansões.
 * Tudo que vive no HEAP deriva daqui, sem exceção. */
#define JASH_LINE_CAP            (256 * 1024)  /* JINV-103: aumentado de 128KB */

/* ── Tetos menores, cada um com sua razão ────────────────────────────────
 *
 * PILHA: `jash_expand_ctx_t` carrega três buffers deste tamanho e é alocado
 * na pilha a cada expansão (~104 KB por chamada com o valor atual). Elevá-lo
 * a JASH_LINE_CAP daria ~400 KB de pilha por nível de recursão do avaliador.
 * Acima deste teto a linha passa INTACTA e sem expansão de til/chaves/glob,
 * com aviso — nunca cortada (JINV-026), porque o que se perde num corte é o
 * FINAL da linha, onde moram o `>`, o `|` e o `&&`. */
#define JASH_LINE_CAP_STACK      (256 * 1024)  /* JINV-103: aumentado de 32KB */

/* AVALIAÇÃO: profundidade máxima de recursão do avaliador (eval.c).
 * Cada nível consome ~1KB de pilha para slots de expansão/comentário/PS/trim/unq.
 * 200 níveis = ~200KB de pilha, seguro mesmo com guard pages. */
#define JASH_EVAL_MAX_DEPTH      200

/* VALOR DE VARIÁVEL: `JASH_ENV_VAL_LEN` é usado como tamanho de buffer de
 * pilha em ~10 pontos, vários deles na MESMA função (três `char[…]` seguidos
 * em `builtin_redir.c`). Elevá-lo multiplica o consumo de pilha por sítio.
 * Enquanto isso, o armazenamento em si já é heap (`heap_val` em state.h), então
 * este número é POLÍTICA, não layout — herdado de quando a tabela de env era
 * um array fixo. Reduzi-lo a um teto honesto é dívida declarada. */
#define JASH_LINE_CAP_ENVVAL     (8 * 1024)

/* ── A relação que o gate verifica ───────────────────────────────────────
 * Falha em tempo de compilação se alguém inverter a ordem — um teto de pilha
 * maior que o canônico significa que a derivação foi trocada por um número
 * solto outra vez. */
/* Compila em qualquer C11, inclusive no bear (onde #if com macros conversa
 * diferente). O contrato continua obrigatório em tempo de compilação. */
_Static_assert(JASH_LINE_CAP_STACK <= JASH_LINE_CAP,
    "JASH_LINE_CAP_STACK nao pode exceder JASH_LINE_CAP (ver core/limits.h)");
/* Checagens de compilação em forma de _Static_assert: funcionam em C11 comum
 * e no bear (onde #if com macros não avalia comparações corretamente). */
_Static_assert(JASH_LINE_CAP_ENVVAL <= JASH_LINE_CAP_STACK,
    "JASH_LINE_CAP_ENVVAL nao pode exceder JASH_LINE_CAP_STACK");
_Static_assert((JASH_LINE_CAP & (JASH_LINE_CAP - 1)) == 0,
    "JASH_LINE_CAP must be a power of 2");
_Static_assert(JASH_EVAL_MAX_DEPTH > 0 && JASH_EVAL_MAX_DEPTH <= 1000,
    "JASH_EVAL_MAX_DEPTH must be in range 1..1000");
_Static_assert(JASH_LINE_CAP >= JASH_LINE_CAP_STACK,
    "JASH_LINE_CAP must be >= JASH_LINE_CAP_STACK");

#endif /* JASH_CORE_LIMITS_H */
