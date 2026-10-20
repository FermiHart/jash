// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/heredoc — here-document parsing (cat << EOF)
// Enhanced with bash parity: <<- indented terminator, pipe integration

#ifndef JASH_PARSER_HEREDOC_H
#define JASH_PARSER_HEREDOC_H

#include <stddef.h>

/* Parse here-document content until delimiter (standard << mode)
 * Returns: allocated string with heredoc content, or NULL on error.
 * Caller must free result. */
char* jash_parse_heredoc(const char* delimiter);

/* Extended heredoc parser with options
 * mode: 0 = << (standard), 1 = <<- (strip indentation)
 * Returns: allocated string with heredoc content, or NULL on error.
 * Caller must free result. */
char* jash_parse_heredoc_ex(const char* delimiter, int mode);

/* Check if token is a heredoc redirect (contains <<) */
int jash_is_heredoc_redirect(const char* token);

/* Check if token uses <<- (indented terminator) mode */
int jash_is_heredoc_indented(const char* token);

/* Extract delimiter from "<<EOF" → "EOF" or "<<-EOF" → "EOF" */
char* jash_extract_heredoc_delim(const char* token);

/* Validate heredoc delimiter syntax.
 * Returns: 0 if valid, -1 if invalid. */
int jash_validate_heredoc_delim(const char* delim);

/* ═══ JINV-092 — a ÚNICA leitura de palavra DELIMITADORA de heredoc ════════
 *
 * POSIX XCU 2.7.4 é uma frase só: *se QUALQUER caractere da palavra estiver
 * citado*, o delimitador é a palavra APÓS REMOÇÃO DE CITAÇÃO e o corpo NÃO
 * sofre expansão. Não é "começa e acaba com aspas" — é *qualquer* caractere,
 * por *qualquer* das três citações:
 *
 *     <<'EOF'    <<"EOF"    <<\EOF    <<E"OF"    <<EO\F
 *
 * As cinco valem, e as cinco fecham numa linha que diz exatamente `EOF`.
 *
 * Duas leituras independentes existiam — c/main.c (a que materializa o corpo)
 * e jash_protect_quoted_heredocs (a que blinda o corpo da expansão) — e as
 * DUAS só conheciam `'…'` e `"…"` inteiriças. `<<\EOF` errava nas duas, e as
 * duas de um jeito diferente:
 *
 *   - o blindador não reconhecia citação nenhuma, então `$HOME` no corpo era
 *     EXPANDIDO (o autor citou justamente para que não fosse);
 *   - o materializador guardava o delimitador como `\EOF` e depois procurava
 *     uma linha `\EOF` que nunca chega. O capturador então corria até o FIM
 *     DA ENTRADA, e todo o texto que o autor escreveu como DADO voltava ao
 *     parser e era EXECUTADO COMO COMANDO.
 *
 * O segundo é a família do JINV-016/020/022: dado virando comando. E `<<\EOF`
 * não é exótico — é o estilo da casa do autoconf (`<<\_ACEOF`): 15 a 20
 * ocorrências por `configure`, e a causa medida da classe `missing fi` que
 * matava 20 das 38 invocações do corpus.
 *
 * `p` aponta para o começo da palavra (depois de `<<` e do `-` opcional).
 * Copia o delimitador JÁ SEM CITAÇÃO em `out`; `*quoted` recebe 1 se qualquer
 * parte estava citada. Devolve o ponteiro logo DEPOIS da palavra.
 *
 * A varredura respeita aspas: um metacaractere CITADO (`<<'A;B'`) faz parte da
 * palavra, e só um NU a termina — a versão anterior parava no `;` sempre, o
 * que quebrava o delimitador pelo meio. Ambos os ponteiros de saída são
 * obrigatórios exceto `quoted`, que aceita NULL.
 * Gate: tests/test_jinv092_heredoc_delim.sh */
const char* jash_heredoc_delim(const char* p, char* out, size_t cap, int* quoted);

/* Protect heredoc bodies with quoted delimiters from expansion.
 * Replaces $ and ` with sentinels in the body so expansion passes
 * ignore them. Call before jash_param_expand(). */
void jash_protect_quoted_heredocs(char* line);

/* Get total size of heredoc content (for buffer pre-allocation) */
size_t jash_heredoc_content_size(const char* content);

/* Format heredoc error message */
const char* jash_heredoc_strerror(int errcode);

/* ─── Re-entrant heredoc body expansion (BUG-030) ─────────────────────────
 * A heredoc/here-string body materialized into `path` is registered with its
 * RAW text. jash_heredoc_expand_line() re-renders any registered body whose
 * path appears in `line` against the CURRENT shell state, just before that
 * command runs — so a body inside a loop sees the per-iteration loop variable.
 * The registry is stack-disciplined (mark/rewind) for nested run-multiline
 * (e.g. `source` within a script). */
struct jash_state;
void jash_heredoc_register(const char* path, const char* raw, int is_herestring);
int  jash_heredoc_expand_line(struct jash_state* state, const char* line);
int  jash_heredoc_registry_mark(void);
void jash_heredoc_registry_rewind(int mark);

#endif
