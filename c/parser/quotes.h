// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/quotes — quote and escape handling

#ifndef JASH_PARSER_QUOTES_H
#define JASH_PARSER_QUOTES_H

#include <stddef.h>

/* Remove quotes from a string, respecting single/double quote semantics.
 * Single quotes: literal (no expansion, no escaping)
 * Double quotes: expansion + limited escaping (\$ \" \\ \n)
 * Backslash (outside): escape next character
 *
 * Returns: length of unquoted string, or -1 if unterminated quote
 */
int jash_unquote(const char* in, char* out, size_t out_size);

/* Check if a string contains any quotes or backslashes. */
int jash_has_quotes(const char* s);

/* Split a quoted string into tokens, respecting quotes.
 * Allocates memory for each token (caller must free).
 * Returns: number of tokens found
 */
int jash_quote_split(const char* line, char** out, int max_tokens);


/* Teto de uma palavra de comando. Um nome de comando é um caminho — o
 * pipeline usava 64 bytes e truncava caminhos absolutos em silêncio. */
#define JASH_CMDWORD_MAX 1024

/* JINV-019 — extrair a PRIMEIRA PALAVRA respeitando aspas.
 *
 * Existiam TRÊS cópias desta lógica (eval.c, pipeline.c, main.c) e nenhuma
 * rastreava estado de aspas: todas cortavam no primeiro espaço. Um nome de
 * comando legítimo com espaço — `'/opt/my tool/run' arg` — era truncado em
 * `'/opt/my`, e o jash_unquote seguinte batia no "aspa não terminada",
 * devolvia -1, e a guarda `if (len >= 0)` engolia a falha em silêncio. O
 * usuário recebia `'/opt/my: command not found`: uma mensagem que aponta para
 * um arquivo que ele nunca escreveu.
 *
 * Copia a palavra BRUTA (ainda com aspas) para `out` e devolve o ponteiro
 * para o resto da linha, já sem espaços à esquerda. O unquote continua sendo
 * responsabilidade do chamador — a ordem já estava certa, o que faltava era a
 * palavra inteira.
 *
 * `stop_pipe` != 0 também termina a palavra num '|' FORA de aspas (o que a
 * cópia do pipeline.c precisava).
 *
 * Devolve -1 em `*unterminated` quando a palavra acaba dentro de aspas, para
 * que o chamador possa diagnosticar em vez de adivinhar. NULL é aceito.
 * Gate: tests/test_jinv019_cmdword_quotes.sh */
const char* jash_first_word(const char* line, char* out, unsigned cap,
                            int stop_pipe, int* unterminated);

/* JINV-063 — espaco no FIM da linha e ESTRUTURA, nunca DADO.
 *
 *   $ echo x␣            bash: 'x\n'      jash: 'x \n'   <- vazava
 *   $ echo "x "          bash: 'x \n'     jash: 'x \n'   <- correto, e tem
 *                                                           que continuar
 *
 * O espaco final da LINHA DE COMANDO aparecia na SAIDA. Nao e cosmetico:
 * `[ "$(echo x )" = "x" ]` e verdadeiro no bash e era FALSO no jash, e a linha
 * gerada por programa (ou colada, ou montada por um agente) quase sempre
 * carrega um espaco final que ninguem ve.
 *
 * Mesma familia do JINV-020/026/028/038: a fronteira entre DADO e ESTRUTURA
 * sendo apagada em silencio. E, como no JINV-020, a unica leitura correta e
 * CONSCIENTE DE ASPAS — quando `args` chega ao builtin, `echo "x "` e
 * `echo x␣` ja sao a MESMA string `x `, e um rtrim cego em builtins/echo.c
 * quebraria o primeiro caso. Por isso o corte mora aqui, ANTES do unquote,
 * enquanto a informacao de aspas ainda existe.
 *
 * Devolve quantos bytes do inicio de `s` sao DADO: o comprimento apos cortar
 * o espaco em branco NAO-CITADO e NAO-ESCAPADO do fim. Aspa nao terminada
 * devolve strlen(s) — nao ha leitura confiavel, entao nao se corta nada.
 * Gate: tests/test_jinv063_trailing_space.sh */
size_t jash_rtrim_unquoted(const char* s);

/* JINV-070 / JINV-071 — UMA leitura de comentario e de continuacao, para o
 * topo E para dentro dos blocos.
 *
 * O defeito: o jash tinha DOIS analisadores sobre o mesmo texto. O de cima
 * (`jash_eval_line`) sabia o que e `#` fora de aspas; o rastreador de bloco
 * (o acumulador de `if`/`for`/`case` e os divisores de enunciado) nao sabia
 * NADA — e o de cima cortava a linha do `#` ate o FIM DA STRING, nao ate o
 * fim da LINHA. Num bloco de varias linhas isso engolia o `fi`:
 *
 *     if false; then            bash: (nada)         rc 0
 *     echo A # comentario       jash: syntax error: missing fi   rc 2
 *     fi                              e o ARQUIVO INTEIRO abortava
 *
 * Em `case` a mesma raiz nao emitia erro nenhum: rc 0, corpo do ramo mudo.
 * Em `f() { ... }` destruia a definicao. Medido em 2 176 scripts reais desta
 * maquina: 208 contem o gatilho, e ele sozinho zerava os 19 `configure` de
 * autoconf (audits/BENCHMARK-scripts-reais.md secao 4.1).
 *
 * O irmao: `\` no fim da linha DENTRO de um bloco nao era honrado de jeito
 * nenhum — `echo A \` + `B` imprimia `A \` e rodava `B` como comando (759
 * scripts, secao 4.2). Comentario e continuacao sao a MESMA passada porque
 * dependem um do outro: `# nota \` NAO continua (o comentario acaba na
 * nova-linha), e `echo a \` + `# x` continua ANTES de o `#` existir. Dois
 * analisadores separados nao tem como acertar os dois.
 *
 * Contrato: le `in` uma vez e escreve em `out` (cap bytes, sempre terminado
 * em NUL) o mesmo texto com (a) todo comentario NAO-CITADO removido ate a
 * nova-linha — a nova-linha FICA, e so ela separa os enunciados — e (b) todo
 * `\`+nova-linha fora de aspas simples removido (juncao POSIX). Dentro de
 * `'...'`, de `$( )` e de `${ }` nada e tocado (o `#` la e dado, e o cmdsub
 * tem a propria passada — BUG-100).
 *
 * Devolve quantas transformacoes fez (0 = nada a fazer, o chamador pode
 * seguir com o ponteiro original) ou -1 se `in` nao cabe em `cap` — nesse
 * caso `out` NAO e usavel e o chamador tem que manter o original: truncar
 * texto de programa e pior do que nao consertar comentario nenhum.
 * Gate: tests/test_jinv070_block_comment.sh, tests/test_jinv071_block_cont.sh */
int jash_strip_comments_and_joins(const char* in, char* out, size_t cap);

/* ═══ JINV-094 — o OPERADOR no fim da linha PEDE a linha seguinte ══════════
 *
 * O defeito, na forma mínima medida contra /bin/bash:
 *
 *     echo abc |          bash: `ABC`
 *     tr a A              jash: `abc`   ← O CANO SOME, em silêncio, rc 0
 *
 * Um cano é ESTRUTURA. Ele foi perdido e o shell disse que deu certo — a
 * mesma família de JINV-016/020/026/028/032/083/085/087/093, onde estrutura
 * desaparece calada. Um shell que perde um cano e devolve rc 0 é pior que um
 * que falha: a saída errada segue adiante como se fosse a certa.
 *
 * Também não é o mesmo que a junção `\`+nova-linha do JINV-071, e por isso
 * NÃO cabia naquela passada:
 *
 *   `\`+nova-linha  é junção LÉXICA    — os DOIS bytes somem da entrada;
 *   operador no fim é continuação SINTÁTICA — o operador FICA, some só a
 *                                             nova-linha.
 *
 * ── POR QUE ESTA PASSADA É SEPARADA (e roda ANTES do resto) ───────────────
 *
 * `jash_strip_comments_and_joins` roda em `jash_eval_line`, ou seja DEPOIS de
 * `jash_run_multiline` já ter partido o texto em linhas lógicas. Quando o
 * avaliador recebe `echo abc |` a nova-linha já não existe: não há o que
 * juntar. A decisão tem que acontecer ANTES do corte, e por isso mora numa
 * passada sobre o PROGRAMA INTEIRO (c/main.c, logo depois do
 * pré-processamento de heredoc, cujos corpos já saíram do texto para arquivos
 * temporários). Uma passada só, no texto todo: topo, corpo de bloco, corpo de
 * `$( )` e de crase saem todos consertados pelo mesmo caminhante — nenhum
 * segundo analisador.
 *
 * ── CONTRATO ──────────────────────────────────────────────────────────────
 *
 * Reescreve `buf` NO LUGAR. É seguro porque a transformação nunca cresce: uma
 * nova-linha consumida vira UM espaço, e um comentário engolido pela junção só
 * encurta. A saída nunca ultrapassa a posição já lida da entrada.
 *
 * Some a nova-linha que vem depois de `|`, `||` ou `&&` NÃO CITADOS (com os
 * brancos e o comentário que os separavam do fim da linha, e com as linhas em
 * branco ou só-comentário que vierem em seguida). NÃO some depois de `;` nem
 * de `&` sozinho — no bash esses dois FECHAM o enunciado (conferido).
 * Operador dentro de aspas, escapado, ou dentro de comentário é DADO e não
 * continua nada.
 *
 * `*pending_eof` recebe 1 quando o texto ACABA com um operador pendente
 * (`echo abc |` e mais nada). No bash isso é `syntax error: unexpected end of
 * file`, rc 2, e NADA roda — quem chama tem que recusar, não executar o
 * pedaço. NULL é aceito.
 *
 * Devolve quantas junções fez (0 = nada a fazer).
 * Gate: tests/test_jinv094_operator_continuation.sh */
int jash_join_operator_lines(char* buf, int* pending_eof);

/* JINV-116/117 — THE scanner. One quote state machine, not N.
 *
 * Devolve o ponteiro para a primeira ocorrência de qualquer byte de `stops`
 * que esteja FORA de aspas, de escape e de expansão. Se nenhum aparecer,
 * devolve o ponteiro para o '\0' final — nunca NULL, para que o chamador possa
 * fazer `p = jash_scan_unquoted(p, ";\n"); if (!*p) erro;` sem um teste extra.
 *
 * ── O QUE É "FORA" ────────────────────────────────────────────────────────
 *   'aspa simples'   tudo literal até a próxima ' — nem escape existe aqui
 *   "aspa dupla"     \ escapa o PRÓXIMO byte (inclusive a própria ")
 *   \x  solto        escapa x
 *   `crase`          inerte até a próxima crase
 *   $(...) ${...}    inerte, com aninhamento contado
 *
 * ── POR QUE ESTE ARQUIVO ──────────────────────────────────────────────────
 *
 * Havia OITO cópias desta varredura, cada uma com um subconjunto diferente da
 * regra, e as duas famílias erravam de jeitos distintos:
 *
 *   - for_loop.c:416, while_loop.c:56/59, select_loop.c:75 varriam
 *     `while (*p && *p != ';' && *p != '\n') p++;` — ZERO consciência de
 *     aspas. Um `;` citado no cabeçalho do laço encerrava a lista cedo e o
 *     `do` seguinte sumia: `for c in "a; b"; do` virava erro de sintaxe
 *     (JINV-116). Forma comuníssima em script gerado por agente.
 *
 *   - pipeline.c:line_has_op() rastreava aspas mas fechava a aspa dupla num
 *     \" ESCAPADO. `echo "a\">x"` era lido como tendo um `>` fora de aspas:
 *     o shell CRIAVA O ARQUIVO `x` em silêncio (JINV-117). Não é cosmético —
 *     é escrita em disco que o usuário não pediu.
 *
 * A regra da aspa dupla é a que mais se erra ao recopiar, e é exatamente a que
 * tem consequência destrutiva. Por isso ela mora aqui, uma vez.
 *
 * Gates: tests/test_jinv116_loop_quoted_semi.sh
 *        tests/test_jinv117_dq_escape_operator.sh */
const char* jash_scan_unquoted(const char* s, const char* stops);

/* JINV-116: a MESMA regra de aspas, para quem não pode usar
 * jash_scan_unquoted() porque precisa intercalar outro estado (o splitter de
 * pipeline conta extglob, subshell, brace group e [[ ]] no mesmo laço).
 *
 * Avança UM passo a partir de `p`, atualizando `*quote` (0, '\'' ou '"').
 * Retorna o próximo ponteiro se o byte era assunto de aspas/escape; retorna
 * NULL se não era — e aí quem chamou decide o que fazer com ele.
 *
 * Quem varre texto de shell usa isto ou jash_scan_unquoted(). Não existe uma
 * terceira forma: foi a terceira forma que truncou o arquivo do usuário. */
const char* jash_quote_step(const char* p, char* quote);

/* Idem, mas para quem faz REMOÇÃO de aspas ao ler (nome de arquivo, palavra).
 * *out_c recebe o byte que sobrevive, ou 0 se era só delimitador. */
const char* jash_unquote_step(const char* p, char* quote, char* out_c);

#endif /* JASH_PARSER_QUOTES_H */
