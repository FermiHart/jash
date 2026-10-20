// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: parser/cmdsub — command substitution $(cmd) and `cmd`
//
// Implementation strategy: run the inner command with a writer that
// captures output into a buffer (instead of stdout), trim trailing
// newlines (bash semantics), then splice the captured text into the
// outer command line before dispatch.

#ifndef JASH_PARSER_CMDSUB_H
#define JASH_PARSER_CMDSUB_H

#include <core/state.h>

/* Returns 1 if `line` contains $( or ` substring. */
int jash_has_cmdsub(const char* line);

/* Expansion-presence bitmask: ONE quote-aware pass answering for cmdsub,
 * arithmetic, parameter expansion and quoted-heredoc markers together.
 * Each bit replicates its standalone probe exactly (same quote rules,
 * same trigger bytes), so gating on bits is identical to calling the
 * three probes in sequence — with a third of the scans on the common
 * no-expansion line. After any stage rewrites the line, rescan: an
 * expansion can introduce new triggers (e.g. cmdsub emitting `$HOME`). */
#define JASH_EXP_CMDSUB   0x01u
#define JASH_EXP_ARITH  0x02u
#define JASH_EXP_PARAM  0x04u
#define JASH_EXP_HEREDOCQ 0x08u
#define JASH_EXP_ALL    0x0Fu
unsigned jash_expansion_mask(const char* line);

/* Expand all $(cmd) and `cmd` substrings in `line`, replacing each
 * with the trimmed stdout of running `cmd`. Returns pointer to a
 * static buffer with the rewritten string, or `line` if no work was
 * done. */
const char* jash_expand_cmdsub(jash_state_t* s, const char* line);

/* Same expansion when the caller just proved the unchanged input contains a
 * substitution with jash_has_cmdsub(). Avoids repeating that linear scan. */
const char* jash_expand_cmdsub_known(jash_state_t* s, const char* line);

/* ═══ JINV-088 — a expansão que NÃO conseguiu montar o enunciado inteiro ═══
 *
 * O texto do comando dentro de `$( )` / ` … ` era cortado em 511 bytes por
 * três buffers fixos EM SÉRIE. Um enunciado cortado não é o enunciado
 * escrito — some o `|`, some o `> arquivo`, some o `&&` — então o shell
 * executava um programa que ninguém digitou, calado e com rc 0.
 *
 * O corte acabou (pilha até 512, heap do tamanho exato acima). O que resta é
 * a falta de memória, e ela NÃO pode virar truncamento: é RECUSA ALTA. Mesmo
 * contrato de jash_param_expand_error() — quem expande consulta antes de
 * executar, e aborta a linha inteira. */
int jash_cmdsub_error(void);
const char* jash_cmdsub_error_msg(void);
void jash_cmdsub_clear_error(void);

/* SECURITY #15: after param/arith expansion has run on a line that went
 * through jash_expand_cmdsub, call this in-place to restore the literal '$'
 * and '`' bytes that were shielded in the substitution output (so they reach
 * the command as inert data, never re-expanded). Safe to call unconditionally
 * (the sentinels never occur in shell source). */
void jash_cmdsub_unprotect(char* s);

/* JINV-090: o inverso — blinda o `$`/crase RESIDUAL de uma linha JA totalmente
 * expandida que ainda vai passar pelo motor de canos (que expande de novo, por
 * segmento). Um `$` que sobrou a uma passada completa e dado, por definição.
 * Chamar só depois da última expansão e só sobre buffer mutável; o restauro é
 * em pipeline.c, depois da expansão de cada segmento.
 * Gate: tests/test_jinv090_eval_no_reexpand.sh */
void jash_cmdsub_protect_residual(char* s);

/* ═══ JINV-087 — os bytes que levam a MARCA DE CITAÇÃO adiante ═════════════
 *
 * A separação de campos por `$IFS` só se aplica ao resultado de expansão NÃO
 * CITADA (POSIX XCU 2.6.5). O jash expande reescrevendo texto: quando o
 * tokenizador a jusante vê a cadeia pronta, a informação "este byte veio de
 * `$P` sem aspas" já morreu — e era ela, e só ela, que autorizava o corte.
 *
 * Estes bytes SÃO essa marca: emitidos NO PONTO da expansão (onde as aspas
 * ainda são conhecidas) e consumidos no tokenizador.
 *
 *   JASH_FIELD_SEP  fronteira de campo produzida por um caractere de IFS
 *                   NÃO-BRANCO num resultado não citado. Delimitador EXATO:
 *                   `a::b` com IFS=':' são três campos, o do meio vazio.
 *                   Irmão do 0x1E (RS), que marca fronteira de ELEMENTO DE
 *                   ARRAY — coisas diferentes, bytes diferentes, de
 *                   propósito: reusar o 0x1E faria `"${a[@]}"` e a divisão
 *                   por IFS mentirem uma sobre a outra.
 *
 *   JASH_PROT_TAB   tabulação / nova-linha que NÃO está em `$IFS` e portanto
 *   JASH_PROT_NL    é DADO, não separador (com `IFS=:`, o valor `a<TAB>b:c`
 *                   são DOIS campos e o primeiro contém a tabulação). Mesmo
 *                   mecanismo do JASH_PROT_SPACE (0x06, JINV-009/033), que
 *                   já existia para o espaço vindo de dentro de um nome de
 *                   arquivo. Restaurados por jash_quote_unshield, abaixo.
 *
 * Só aparecem em texto TRANSITÓRIO de expansão; nenhum sobrevive a um token
 * entregue ao corpo do laço ou a um argv. */
#define JASH_FIELD_SEP  ((char)0x1F)
#define JASH_PROT_TAB   ((char)0x0E)
#define JASH_PROT_NL    ((char)0x0F)

/*   JASH_EXP_MARK   "esta PALAVRA contém uma expansão não citada". A régua do
 *                   bash (o oráculo) não é o byte, é a palavra: `IFS=:` com
 *                   `$P:x` corta no `:` LITERAL, porque a palavra passou por
 *                   expansão; já `a:b` sozinho não corta, e `"$P":x` também
 *                   não — a expansão estava entre aspas. Sem esta marca a
 *                   correção erraria para o outro lado: preservaria dado que
 *                   o bash divide.
 *
 *                   Só é emitida quando `$IFS` tem algum caractere NÃO-BRANCO;
 *                   com IFS padrão (o caso de 99% dos scripts) o texto sai
 *                   byte a byte idêntico ao de antes. Consumida e APAGADA
 *                   pela varredura do for_loop, antes de chave/glob — um byte
 *                   estranho dentro de um padrão de glob não casaria nada. */
#define JASH_EXP_MARK   ((char)0x11)
/* O irmão mais velho da família (JINV-009) mora em cmdsub.c, junto do texto
 * que explica por que ele existe; repetido aqui — mesmo valor, guardado —
 * para que quem emite fora de cmdsub.c não precise inventar um segundo nome
 * para o mesmo byte. */
#ifndef JASH_PROT_SPACE
#define JASH_PROT_SPACE ((char)0x06)
#endif

/* ═══ JINV-096 — VALOR não é SINTAXE ══════════════════════════════════════
 *
 * POSIX XCU 2.6: sobre o RESULTADO de uma expansão não citada aplicam-se
 * APENAS a divisão de campos (`$IFS`) e a expansão de nome de arquivo
 * (glob). NUNCA reanálise sintática. Um `|` que veio de um VALOR é texto;
 * um `|` que o autor DIGITOU é estrutura. São o mesmo byte e origens
 * opostas.
 *
 * O jash decide "esta linha tem cano?" (`jash_line_has_pipe`, pipeline.c) e
 * "esta linha redireciona?" (`jash_line_has_redir`) DEPOIS de reescrever a
 * linha com as expansões — quando as duas origens já viraram o mesmo byte.
 * Medido no HEAD, contra `/bin/bash` como oráculo:
 *
 *     P='dado | tee /tmp/X'; echo $P
 *       bash: imprime o texto     jash: imprime `dado` e CRIA /tmp/X
 *     V='dado > /tmp/Y'; echo $V
 *       bash: imprime o texto     jash: saída vazia e CRIA /tmp/Y
 *
 * Com `|`, um comando nomeado apenas em DADO — conteúdo de arquivo, de
 * rede, de argumento, de variável de ambiente — EXECUTA. É execução de
 * comando a partir de dado não confiável, não incompatibilidade.
 *
 * ═══ Por que este não é o JINV-093 ═══
 *
 * O JINV-093 consertou o caso ESCAPADO (`\|`): ali a marca de citação (a
 * barra) existia e morria na passada de parâmetros; o conserto foi
 * PRESERVÁ-LA. Aqui não havia marca nenhuma — o byte sai do valor idêntico
 * ao byte que o autor digitaria. Então a marca tem que ser CRIADA no ponto
 * da emissão, que é o único lugar onde ainda se sabe "isto veio de dentro
 * de um valor".
 *
 * ═══ Por que sentinelas 1:1, e não um prefixo de escape ═══
 *
 * Trocar o byte (e não prefixá-lo) faz com que NENHUMA varredura precise
 * aprender uma regra nova: `line_has_op`, o divisor de segmentos de cano, o
 * analisador de redirecionamento e a camada de til/chave/glob simplesmente
 * não encontram mais o byte que procuram. O restauro pega carona em
 * `jash_quote_unshield`, que TODO consumidor de token já chama (core/argv.c,
 * exec.c, pipeline.c, for_loop.c, case_stmt.c, heredoc.c, quotes.c). Zero
 * varredura nova — era a exigência: este repositório já encontrou onze vezes
 * o padrão "duas implementações da mesma coisa, uma empobrecida".
 *
 * O conjunto é o do `jash_pe_is_operator_meta` (param_expand.c, JINV-093) —
 * a definição que o repositório JÁ tem de "byte que decide estrutura" — mais
 * o til, que é a mesma regra na camada de palavra (`~` num VALOR não é
 * diretório do usuário; medido: `V='~'; echo $V` dava `/Users/…`).
 * O `*` NÃO entra: glob É uma das duas expansões que POSIX manda aplicar.
 *
 * A porta de emissão é ÚNICA (`jash_meta_shield`) e é chamada em TODOS os
 * pontos que emitem resultado de expansão — param_expand.c (valor, array,
 * posicionais), cmdsub.c (saída de comando) e expand.c (nome de arquivo
 * vindo de glob). Um ponto esquecido é um buraco inteiro: foi assim que
 * `echo *` com um arquivo chamado `a|tee X` executava o `tee`.
 *
 * Gate: tests/test_jinv096_value_not_syntax.sh */
#define JASH_PROT_PIPE   ((char)0x12)
#define JASH_PROT_LT     ((char)0x13)
#define JASH_PROT_GT     ((char)0x14)
#define JASH_PROT_AMP    ((char)0x15)
#define JASH_PROT_SEMI   ((char)0x16)
#define JASH_PROT_LPAREN ((char)0x17)
#define JASH_PROT_RPAREN ((char)0x18)
#define JASH_PROT_TILDE  ((char)0x19)

/* JINV-096: a porta ÚNICA. Recebe um byte de DADO (já vindo de uma expansão)
 * e devolve o byte blindado, ou ele mesmo se não decide estrutura. Idempotente
 * sobre bytes já blindados — os sentinelas não são metacaracteres. */
char jash_meta_shield(char c);

/* JINV-096: 1 se `s` contém algum sentinela desta família. Para o resgate de
 * eval.c, que precisa saber se há blindagem a desfazer quando NENHUMA aspa
 * sintática disparou a remoção de aspas (era exatamente o furo do JINV-033
 * com o 0x06). */
int jash_meta_shield_present(const char* s);

/* BUG-096: restore literal quote chars (0x03/0x04 sentinels used to shield
 * quote characters inside expanded VALUES) in a token AFTER tokenization.
 * Safe to call unconditionally — the sentinels never occur in shell source.
 * JINV-087: restaura também a tabulação/nova-linha protegidas.
 * JINV-096: restaura também os metacaracteres de estrutura blindados. */
void jash_quote_unshield(char* s);

#endif
