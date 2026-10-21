#include "source_location.h"
// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/quotes — handling ' " \ quote parsing

#include "quotes.h"
#include "arithmetic.h"
#include "param_expand.h"
#include <core/state.h>
#include <string.h>
#include <ctype.h>

/* Remove quotes from a string, respecting escaping.
 * Input: "hello \"world\"" or 'test $VAR'
 * Output: hello "world" or test $VAR
 *
 * Returns: number of chars written to out, or -1 if unterminated quote
 */
int jash_unquote(const char* in, char* out, size_t out_size) {
    if (!in || !out || out_size == 0) return -1;

    int out_pos = 0;
    int in_pos = 0;

    while (in[in_pos] && out_pos + 1 < (int)out_size) {
        char c = in[in_pos];

        /* Single quote: NO expansion, NO escaping until close quote */
        if (c == '\'') {
            in_pos++;
            while (in[in_pos] && in[in_pos] != '\'') {
                if (out_pos + 1 < (int)out_size) {
                    out[out_pos++] = in[in_pos];
                }
                in_pos++;
            }
            if (!in[in_pos]) return -1;  /* Unterminated single quote */
            in_pos++;  /* skip closing ' */
            continue;
        }

        /* Double quote: expansion + limited escaping (\$ \" \\ \n) */
        if (c == '"') {
            in_pos++;
            while (in[in_pos] && in[in_pos] != '"') {
                if (in[in_pos] == '\\' && in[in_pos + 1]) {
                    char next = in[in_pos + 1];
                    /* Only these escapes are special in double quotes */
                    if (next == '$' || next == '"' || next == '\\' || next == '\n') {
                        in_pos++;  /* skip backslash */
                        if (next == '\n') {
                            /* Backslash-newline: line continuation (skip both) */
                            in_pos++;
                            continue;
                        }
                        if (out_pos + 1 < (int)out_size) {
                            out[out_pos++] = next;
                        }
                        in_pos++;
                        continue;
                    }
                }
                if (out_pos + 1 < (int)out_size) {
                    out[out_pos++] = in[in_pos];
                }
                in_pos++;
            }
            if (!in[in_pos]) return -1;  /* Unterminated double quote */
            in_pos++;  /* skip closing " */
            continue;
        }

        /* Backslash (outside quotes): escape next char */
        if (c == '\\' && in[in_pos + 1]) {
            in_pos++;
            if (out_pos + 1 < (int)out_size) {
                out[out_pos++] = in[in_pos];
            }
            in_pos++;
            continue;
        }

        /* Regular character */
        out[out_pos++] = c;
        in_pos++;
    }

    /* JINV-027 — estouro devolve ERRO, nunca um resultado cortado.
     *
     * O laco acima para quando `out` enche (`out_pos + 1 < out_size`) e a
     * funcao devolvia `out_pos` — ou seja, SUCESSO com dado perdido. Os
     * chamadores fazem `if (len >= 0) usa_o_buffer; else mantem o texto cru`,
     * entao um truncamento silencioso era adotado como se fosse o valor certo.
     *
     * Medido: `echo "$(cat arquivo_de_60KB)"` entregava 16384 bytes (o
     * `char unq_args[16384]` de eval.c), enquanto `printf` — que esta na lista
     * skip_unquote e recebe args crus — entregava os 60000. A diferenca entre
     * os dois builtins era exatamente esta funcao.
     *
     * A guarda anterior (logo abaixo, preservada) so pegava uma FATIA do caso:
     * ela dispara apenas quando o proximo byte nao lido e uma aspa. O
     * comentario dela — "String too long for output buffer" — mostra que o
     * caso era conhecido; faltava cobri-lo por inteiro.
     *
     * Agora: se sobrou QUALQUER byte de entrada, o resultado nao cabe, e isso
     * e um erro. O chamador entao preserva o texto cru, que e completo — bem
     * melhor que meio texto passando por texto inteiro.
     * Gate: tests/test_jinv027_unquote_overflow.sh */
    if (in[in_pos]) {
        return -1;  /* nao coube: erro, nao meia-verdade */
    }

    if ((in[in_pos] && in[in_pos] == '\'') || in[in_pos] == '"') {
        return -1;  /* String too long for output buffer */
    }

    out[out_pos] = 0;
    {   /* BUG-096: restore shielded literal quotes in the token */
        extern void jash_quote_unshield(char* s);
        jash_quote_unshield(out);
    }
    return out_pos;
}

/* Check if a string contains unescaped quotes or needs quote processing.
 * Returns 1 if quoting/escaping is needed, 0 otherwise. */
int jash_has_quotes(const char* s) {
    if (!s) return 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\'' || s[i] == '"' || s[i] == '\\') {
            return 1;
        }
    }
    return 0;
}

/* Split a quoted string into tokens, respecting quotes and escapes.
 * NOTE: Returns pointers to static buffers (max 8 tokens of 256 chars each).
 * For production use, caller should reimplement with dynamic allocation.
 * Example: "hello world" 'test' plain → 3 tokens
 *
 * Returns: number of tokens found, tokens written to out[]
 */
int jash_quote_split(const char* line, char** out, int max_tokens) {
    if (!line || !out || max_tokens <= 0 || max_tokens > 8) return 0;

    static char token_storage[8][256];
    int token_count = 0;
    const char* p = line;
    char token_buf[512];

    while (*p && token_count < max_tokens) {
        /* Skip whitespace */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        if (!*p) break;

        /* Collect token: may span quoted sections */
        int token_len = 0;
        int in_single = 0, in_double = 0;

        while (*p && token_len + 1 < (int)sizeof(token_buf)) {
            char c = *p;

            /* Toggle quote state */
            if (c == '\'' && !in_double) {
                in_single = !in_single;
                p++;
                continue;
            }
            if (c == '"' && !in_single) {
                in_double = !in_double;
                p++;
                continue;
            }

            /* Inside quotes: accept everything until close */
            if (in_single || in_double) {
                token_buf[token_len++] = c;
                p++;
                continue;
            }

            /* Outside quotes: stop at whitespace */
            if (c == ' ' || c == '\t' || c == '\n') break;

            /* Backslash escape */
            if (c == '\\' && p[1]) {
                token_buf[token_len++] = '\\';
                token_buf[token_len++] = p[1];
                p += 2;
                continue;
            }

            token_buf[token_len++] = c;
            p++;
        }

        if (token_len > 0) {
            token_buf[token_len] = 0;

            /* Unquote the token */
            char unquoted[256];
            int unquoted_len = jash_unquote(token_buf, unquoted, sizeof(unquoted));
            if (unquoted_len >= 0) {
                /* Store in static buffer */
                if (unquoted_len >= (int)sizeof(token_storage[token_count])) {
                    unquoted_len = sizeof(token_storage[token_count]) - 1;
                }
                memcpy(token_storage[token_count], unquoted, unquoted_len + 1);
                out[token_count] = token_storage[token_count];
                token_count++;
            }
        }
    }

    return token_count;
}

/* JINV-019 — contrato em quotes.h. */
const char* jash_first_word(const char* line, char* out, unsigned cap,
                            int stop_pipe, int* unterminated) {
    if (unterminated) *unterminated = 0;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;

    unsigned i = 0;
    char q = 0;                       /* aspa aberta: 0, '\'' ou '"' */
    while (*p) {
        char c = *p;
        if (q) {
            if (c == q) q = 0;
            else if (q == '"' && c == '\\' && p[1]) {   /* \" dentro de "" */
                if (i + 1 < cap) out[i++] = c;
                p++; c = *p;
            }
        } else {
            if (c == ' ' || c == '\t') break;
            if (stop_pipe && c == '|') break;
            if (c == '\'' || c == '"') q = c;
            else if (c == '\\' && p[1]) {               /* \ fora de aspas */
                if (i + 1 < cap) out[i++] = c;
                p++; c = *p;
            }
        }
        if (i + 1 < cap) out[i++] = c;
        p++;
    }
    out[i] = '\0';
    if (q && unterminated) *unterminated = 1;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* JINV-063 — contrato em quotes.h. */
size_t jash_rtrim_unquoted(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    /* Caminho quente: TODO despacho de comando passa por aqui. Se o ultimo
     * byte nao e separador nao ha nada a cortar, e nenhuma varredura acontece.
     * A saida rapida mora AQUI, e nao em cada chamador, para que ninguem
     * precise lembrar de escrever a guarda — e para que ninguem a escreva
     * diferente (foi assim que nasceram as cinco copias que este arquivo
     * substituiu). */
    if (n == 0 || (s[n - 1] != ' ' && s[n - 1] != '\t' && s[n - 1] != '\n'))
        return n;

    const char* p = s;
    size_t keep = 0;                  /* bytes ate o fim do ultimo DADO visto */

    while (*p) {
        char c = *p;

        /* Barra invertida fora de aspas: ela e o proximo byte sao DADO.
         * `echo x\  ` -> o espaco escapado fica, o separador seguinte sai
         * (medido contra o bash). */
        if (c == '\\' && p[1]) { p += 2; keep = (size_t)(p - s); continue; }

        /* Aspa simples: literal ate a proxima aspa simples, sem escape. */
        if (c == '\'') {
            const char* q = p + 1;
            while (*q && *q != '\'') q++;
            if (!*q) return n;               /* aspa aberta: nao toca em nada */
            p = q + 1; keep = (size_t)(p - s); continue;
        }

        /* Aspa dupla: `\` ainda escapa dentro dela. */
        if (c == '"') {
            const char* q = p + 1;
            while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; }
            if (!*q) return n;               /* aspa aberta: nao toca em nada */
            p = q + 1; keep = (size_t)(p - s); continue;
        }

        /* Espaco/tab/nova-linha FORA de aspas sao ESTRUTURA — candidatos a
         * sumir. Nao mexem em keep: se nada de dado vier depois, o corte cai
         * aqui. (A nova-linha entra porque os separadores de segmento —
         * pipeline, `;`, `case` — tambem cortam por este mesmo contrato.) */
        if (c == ' ' || c == '\t' || c == '\n') { p++; continue; }

        p++; keep = (size_t)(p - s);
    }
    return keep;
}

/* JINV-070 / JINV-071 — contrato em quotes.h.
 *
 * Esta funcao NASCEU do analisador que ja vivia inline em
 * `jash_eval_line_inner` (c/parser/eval.c) — a mesma maquina de estados de
 * aspas, com a mesma pilha de `$(`/`${` que os BUG-099/BUG-115/BUG-154
 * ensinaram. Ela nao e um TERCEIRO analisador: e aquele, promovido a publico
 * para que o rastreador de bloco pare de adivinhar. Duas diferencas, e as
 * duas sao o conserto:
 *
 *   1. o comentario acaba na NOVA-LINHA, nao no fim da string (JINV-070);
 *   2. a nova-linha conta como fronteira de palavra, entao uma LINHA INTEIRA
 *      de comentario dentro de um bloco tambem e reconhecida.
 *
 * A juncao `\`+nova-linha (JINV-071) anda na MESMA passada de proposito: as
 * duas regras se cruzam (`# nota \` NAO continua, porque o comentario acaba
 * na nova-linha; `echo a \`+`# x` continua ANTES de existir comentario) e so
 * um unico caminhante do texto pode respeitar as duas ao mesmo tempo. Foi
 * exatamente essa dependencia que dois analisadores separados erravam. */
int jash_strip_comments_and_joins_mapped(const char* in, char* out, size_t cap, unsigned* lines) {
    if (!in || !out || cap == 0) return 0;

    /* Saida rapida: sem `#` e sem `\` nao ha comentario nem continuacao —
     * nenhuma copia acontece. `jash_eval_line` passa por aqui a cada comando,
     * entao a guarda mora AQUI e nao em cada chamador (mesma disciplina do
     * jash_rtrim_unquoted acima). */
    {
        const char* q = in;
        int interesting = 0;
        for (; *q; q++) {
            if (*q == '#' || *q == '\\') { interesting = 1; break; }
        }
        if (!interesting) return 0;
    }
    if (strlen(in) >= cap) return -1;   /* nao cabe: NAO truncar (ver contrato) */

    const char* p = in;
    size_t o = 0;
    int changes = 0;

    char sq = 0, dq = 0;
    int cdep = 0; char cdq[64];
    int in_cmt = 0;
    /* O inicio da string conta como fronteira de palavra — um `#` na coluna 0
     * abre comentario. */
    int prev_boundary = 1;

    #define STRIP_PUT(ch, src) do { if (lines) lines[o] = jash_source_line_at(src); out[o++] = (ch); } while (0)
    while (*p) {
        char c = *p;

        /* ── dentro de comentario: tudo e inerte ate a nova-linha ──────────
         * inclusive o `\` — `# nota \` NAO continua a linha (bash). */
        if (in_cmt) {
            if (c == '\n') {
                in_cmt = 0;
                /* A nova-linha SEPARA enunciados: ela fica. */
                if (o + 1 < cap) STRIP_PUT('\n', p);
                prev_boundary = 1;
            }
            p++;
            continue;
        }

        /* ── aspas simples: literal, sem escape, sem comentario ───────────── */
        if (sq) {
            if (c == '\'') sq = 0;
            if (o + 1 < cap) STRIP_PUT(c, p);
            p++; prev_boundary = 0;
            continue;
        }

        /* ── aspas duplas ─────────────────────────────────────────────────── */
        if (dq) {
            if (c == '\\' && p[1] == '\n') {   /* JINV-071: junta dentro de "" */
                p += 2; changes++;
                continue;
            }
            if (c == '\\' && p[1]) {
                if (o + 1 < cap) STRIP_PUT(c, p);
                if (o + 1 < cap) STRIP_PUT(p[1], p + 1);
                p += 2; prev_boundary = 0;
                continue;
            }
            /* BUG-099/BUG-154: `$(` de dentro de "..." reinicia a citacao. */
            if (c == '$' && p[1] == '(' && p[2] != '(') {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 1;
                cdep++; dq = 0;
                if (o + 1 < cap) STRIP_PUT(c, p);
                if (o + 1 < cap) STRIP_PUT(p[1], p + 1);
                p += 2; prev_boundary = 1;
                continue;
            }
            /* BUG-115: `${` dentro de "..." tambem reinicia. */
            if (c == '$' && p[1] == '{') {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 1;
                cdep++; dq = 0;
                if (o + 1 < cap) STRIP_PUT(c, p);
                if (o + 1 < cap) STRIP_PUT(p[1], p + 1);
                p += 2; prev_boundary = 1;
                continue;
            }
            if (c == '"') dq = 0;
            if (o + 1 < cap) STRIP_PUT(c, p);
            p++; prev_boundary = 0;
            continue;
        }

        /* ── fora de aspas ────────────────────────────────────────────────── */
        if (c == '\\' && p[1] == '\n') {       /* JINV-071: juncao POSIX */
            p += 2; changes++;
            /* A linha seguinte continua a MESMA palavra: a fronteira nao
             * reabre (`echo a\`+`#b` nao vira comentario, como no bash). */
            prev_boundary = 0;
            continue;
        }
        if (c == '\\' && p[1]) {
            if (o + 1 < cap) STRIP_PUT(c, p);
            if (o + 1 < cap) STRIP_PUT(p[1], p + 1);
            p += 2; prev_boundary = 0;
            continue;
        }
        if (c == '\'') {
            sq = 1;
            if (o + 1 < cap) STRIP_PUT(c, p);
            p++; prev_boundary = 0;
            continue;
        }
        if (c == '"') {
            dq = 1;
            if (o + 1 < cap) STRIP_PUT(c, p);
            p++; prev_boundary = 0;
            continue;
        }
        if (c == '$' && (p[1] == '(' || p[1] == '{')) {
            if (cdep < (int)sizeof(cdq)) cdq[cdep] = 0;
            cdep++;
            if (o + 1 < cap) STRIP_PUT(c, p);
            if (o + 1 < cap) STRIP_PUT(p[1], p + 1);
            p += 2; prev_boundary = 1;
            continue;
        }
        if (cdep > 0) {
            /* BUG-100: dentro de `$( )` / `${ }` a passada e do cmdsub — aqui
             * o texto passa INTACTO, so a pilha de citacao e mantida. */
            if (c == '(' || c == '{') {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 0;
                cdep++;
            } else if (c == ')' || c == '}') {
                cdep--;
                if (cdep >= 0 && cdep < (int)sizeof(cdq)) dq = cdq[cdep];
            }
            prev_boundary = (c == ' ' || c == '\t' || c == '\n' || c == ';' ||
                             c == '&' || c == '|' || c == '(');
            if (o + 1 < cap) STRIP_PUT(c, p);
            p++;
            continue;
        }
        if (c == '#' && prev_boundary) {
            /* JINV-070: o comentario vai ate a NOVA-LINHA, nao ate o fim da
             * string. Corta o espaco em branco que ficou antes do `#` com a
             * leitura CONSCIENTE DE ASPAS (JINV-063) — um rtrim cego comeria
             * tambem o espaco ESCAPADO, que e dado. */
            out[o] = '\0';
            o = jash_rtrim_unquoted(out);
            out[o] = '\0';
            in_cmt = 1; changes++;
            p++;
            continue;
        }
        prev_boundary = (c == ' ' || c == '\t' || c == '\n' || c == ';' ||
                         c == '&' || c == '|' || c == '(' || c == ')');
        if (o + 1 < cap) STRIP_PUT(c, p);
        p++;
    }

    if (o >= cap) o = cap - 1;
    out[o] = '\0';
    if (lines) lines[o] = jash_source_line_at(p);
    #undef STRIP_PUT
    return changes;
}

/* JINV-094 — contrato em quotes.h.
 *
 * O caminhante é o MESMO de `jash_strip_comments_and_joins` logo acima (mesma
 * máquina de aspas, mesma pilha por nível de `$(`/`${` dos BUG-099/115/154,
 * mesma regra de fronteira de palavra para o `#`). O que muda é só a decisão
 * que ele toma na nova-linha — e é de propósito que as duas leituras não são a
 * mesma FUNÇÃO: elas rodam em MOMENTOS diferentes do texto (esta antes do
 * corte em linhas lógicas, aquela depois) e uma passada de comentário sobre o
 * programa inteiro invadiria o corpo de `$( )`, que tem passada própria
 * (BUG-100). Aqui o comentário é lido mas NÃO removido — só some quando ele
 * está no meio de uma junção, porque senão engoliria a linha continuada. */
int jash_join_operator_lines_mapped(char* buf, unsigned* lines, int* pending_eof) {
    if (pending_eof) *pending_eof = 0;
    if (!buf || !*buf) return 0;

    /* Saída rápida: sem `|` e sem `&` não há operador para continuar. O
     * pré-processador do script chama esta função uma vez por programa, e a
     * varredura de saída é uma passada só sobre o texto — a guarda mora AQUI
     * para que nenhum chamador precise lembrar de escrevê-la (mesma disciplina
     * de jash_rtrim_unquoted e de jash_strip_comments_and_joins). */
    {
        const char* q = buf;
        int interesting = 0;
        for (; *q; q++) {
            if (*q == '|' || *q == '&') { interesting = 1; break; }
        }
        if (!interesting) return 0;
    }

    char sq = 0, dq = 0;
    int cdep = 0; char cdq[64];
    int in_cmt = 0;
    size_t cmt_at = 0;          /* onde o comentário começou NA SAÍDA */
    int prev_boundary = 1;      /* o início do texto é fronteira de palavra */
    int pipe_run = 0, amp_run = 0;
    int op = 0;                 /* bytes do operador pendente: 0, 1 ou 2 */
    int joins = 0;

    const char* p = buf;
    size_t o = 0;

/* Qualquer byte de DADO apaga o operador pendente. */
#define JOIN_PUT(ch, src) do { if (lines) lines[o] = lines[(src) - buf]; buf[o++] = (ch); } while (0)
#define JOIN_DATA() do { pipe_run = 0; amp_run = 0; op = 0; } while (0)

    while (*p) {
        char c = *p;

        /* ── a nova-linha: a ÚNICA decisão desta passada ────────────────── */
        if (c == '\n' && !sq && !dq) {
            if (op > 0) {
                /* O comentário que separava o operador do fim da linha morre
                 * junto com a nova-linha que o terminava. Se ficasse, ele
                 * engoliria a linha continuada — `echo a | # nota` + `tr a A`
                 * viraria um comentário com `tr a A` dentro. */
                if (in_cmt) o = cmt_at;
                in_cmt = 0;
                /* O operador FICA; entra um branco para que ele continue tendo
                 * fronteira à direita (`|tr` e `| tr` são a mesma coisa para o
                 * tokenizador, mas `&&x` não é `&& x` em toda leitura). */
                JOIN_PUT(' ', p);
                joins++;
                prev_boundary = 1;
                p++;
                /* `op` NÃO zera: as linhas em branco e as linhas só-comentário
                 * que vierem em seguida também somem (bash). */
                continue;
            }
            in_cmt = 0;
            JOIN_PUT(c, p); p++;
            prev_boundary = 1; JOIN_DATA();
            continue;
        }

        /* ── dentro de comentário: tudo inerte até a nova-linha ─────────── */
        if (in_cmt) { JOIN_PUT(c, p); p++; continue; }

        /* ── aspas simples: literal, sem escape ─────────────────────────── */
        if (sq) {
            if (c == '\'') sq = 0;
            JOIN_PUT(c, p); p++; prev_boundary = 0; JOIN_DATA();
            continue;
        }

        /* ── aspas duplas ──────────────────────────────────────────────── */
        if (dq) {
            if (c == '\\' && p[1]) {
                JOIN_PUT(c, p); JOIN_PUT(p[1], p + 1); p += 2;
                prev_boundary = 0; JOIN_DATA();
                continue;
            }
            /* BUG-099/BUG-154: `$(` de dentro de "..." reinicia a citação. */
            if (c == '$' && p[1] == '(' && p[2] != '(') {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 1;
                cdep++; dq = 0;
                JOIN_PUT(c, p); JOIN_PUT(p[1], p + 1); p += 2;
                prev_boundary = 1; JOIN_DATA();
                continue;
            }
            /* BUG-115: `${` dentro de "..." também reinicia. */
            if (c == '$' && p[1] == '{') {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 1;
                cdep++; dq = 0;
                JOIN_PUT(c, p); JOIN_PUT(p[1], p + 1); p += 2;
                prev_boundary = 1; JOIN_DATA();
                continue;
            }
            if (c == '"') dq = 0;
            JOIN_PUT(c, p); p++; prev_boundary = 0; JOIN_DATA();
            continue;
        }

        /* ── fora de aspas ─────────────────────────────────────────────── */
        if (c == '\\' && p[1]) {
            /* A junção LÉXICA `\`+nova-linha (JINV-071) não mora aqui: o leitor
             * de linha lógica já a honra, e refazê-la neste ponto seria a
             * segunda implementação da mesma regra. Aqui a barra só precisa
             * NÃO ser confundida com estrutura — `\|` é dado. */
            JOIN_PUT(c, p); JOIN_PUT(p[1], p + 1); p += 2;
            prev_boundary = 0; JOIN_DATA();
            continue;
        }
        if (c == '\'') { sq = 1; JOIN_PUT(c, p); p++; prev_boundary = 0; JOIN_DATA(); continue; }
        if (c == '"')  { dq = 1; JOIN_PUT(c, p); p++; prev_boundary = 0; JOIN_DATA(); continue; }
        if (c == '$' && (p[1] == '(' || p[1] == '{')) {
            if (cdep < (int)sizeof(cdq)) cdq[cdep] = 0;
            cdep++;
            JOIN_PUT(c, p); JOIN_PUT(p[1], p + 1); p += 2;
            prev_boundary = 1; JOIN_DATA();
            continue;
        }
        if (c == '#' && prev_boundary && cdep == 0) {
            /* `cdep == 0` é obrigatório, e custou um teste para descobrir:
             * `${` empurra a pilha e abre fronteira de palavra, então em
             * `${#PATH}` o `#` tem fronteira à esquerda e viraria COMENTÁRIO —
             * o resto da linha ficaria inerte, o `}` não fecharia a pilha, e o
             * `|` que viesse depois não seria visto. É a mesma guarda que
             * `jash_strip_comments_and_joins` já aplica (BUG-100: dentro de
             * `$( )`/`${ }` o texto passa intacto, a passada é do cmdsub). */
            in_cmt = 1; cmt_at = o;
            /* `op` NÃO muda: `echo a | # nota` segue com o cano pendente. */
            JOIN_PUT(c, p); p++;
            continue;
        }
        if (c == '(' || c == '{') {
            if (cdep > 0) {
                if (cdep < (int)sizeof(cdq)) cdq[cdep] = 0;
                cdep++;
            }
            JOIN_PUT(c, p); p++; prev_boundary = 1; JOIN_DATA();
            continue;
        }
        if (c == ')' || c == '}') {
            if (cdep > 0) {
                cdep--;
                if (cdep < (int)sizeof(cdq)) dq = cdq[cdep];
            }
            JOIN_PUT(c, p); p++; prev_boundary = 1; JOIN_DATA();
            continue;
        }
        if (c == '|') {
            pipe_run++; amp_run = 0;
            /* `|` continua, `||` continua. `|||` não é operador nenhum no bash
             * (erro de sintaxe) — não juntamos, para que o erro apareça. */
            op = (pipe_run == 1) ? 1 : (pipe_run == 2 ? 2 : 0);
            JOIN_PUT(c, p); p++; prev_boundary = 1;
            continue;
        }
        if (c == '&') {
            amp_run++; pipe_run = 0;
            /* `&` sozinho é SEGUNDO PLANO e FECHA o enunciado (conferido com
             * /bin/bash: `echo a &` + `echo b` roda os dois). Só `&&` pede a
             * linha seguinte. */
            op = (amp_run == 2) ? 2 : 0;
            JOIN_PUT(c, p); p++; prev_boundary = 1;
            continue;
        }
        if (c == ' ' || c == '\t') {
            /* Branco não é dado: NÃO apaga o operador pendente. Mas quebra a
             * corrida — `a | | b` são dois canos, não um `||`. */
            pipe_run = 0; amp_run = 0;
            JOIN_PUT(c, p); p++; prev_boundary = 1;
            continue;
        }

        /* Qualquer outro byte é DADO. (`;` fecha o enunciado e é fronteira.) */
        prev_boundary = (c == ';');
        JOIN_PUT(c, p); p++; JOIN_DATA();
    }

#undef JOIN_PUT
#undef JOIN_DATA

    if (lines) lines[o] = lines[p - buf];
    buf[o] = '\0';
    if (pending_eof) *pending_eof = (op > 0);
    return joins;
}

/* JINV-116/117 — contrato em quotes.h. Uma varredura, uma regra. */
/* A regra da aspa, em UM lugar. Todo o resto do parser passa por aqui. */
const char* jash_quote_step(const char* p, char* quote) {
    if (*quote == '\'') {                   /* literal puro: só a ' fecha */
        if (*p == '\'') *quote = 0;
        return p + 1;
    }
    if (*quote == '"') {
        /* JINV-117: AQUI. Dentro de aspa dupla a barra escapa o próximo byte —
         * inclusive a própria aspa. Pular esse teste é o que fazia
         * `echo "a\">arquivo"` TRUNCAR o arquivo. */
        if (*p == '\\' && p[1]) return p + 2;
        if (*p == '"') *quote = 0;
        return p + 1;
    }
    /* Fora de aspas. */
    if (*p == '\\')                    return p[1] ? p + 2 : p + 1;
    if (*p == '\'' || *p == '"')     { *quote = *p; return p + 1; }
    return NULL;                        /* não é assunto de aspas */
}

/* Um passo de REMOÇÃO de aspas (quem lê um nome de arquivo quer o conteúdo,
 * não os delimitadores). Devolve em *out_c o byte que SOBREVIVE, ou 0 quando
 * o byte era só delimitador de aspa. NULL = não é assunto de aspas. */
const char* jash_unquote_step(const char* p, char* quote, char* out_c) {
    const char  before = *quote;
    const char* q = jash_quote_step(p, quote);
    if (!q) { *out_c = 0; return NULL; }
    if (q == p + 2) { *out_c = p[1]; return q; }      /* \X  →  X          */
    *out_c = (before == *quote) ? *p : 0;             /* delimitador some  */
    return q;
}

const char* jash_scan_unquoted(const char* s, const char* stops) {
    /* Fast reject: if no stop byte occurs in the string at all, the
     * quote/depth tracking below cannot find a hit — one SIMD strpbrk
     * instead of a full state-machine pass. Exact: absence of every
     * stop byte implies the loop below reaches the NUL it returns. */
    if (!strpbrk(s, stops)) return s + strlen(s);
    char quote = 0;   /* ' ou " quando dentro */
    int  depth = 0;   /* aninhamento de $(...) $((...)) ${...} */
    int  back  = 0;   /* dentro de `...` */

    for (const char* p = s; *p; p++) {
        if (!back) {
            const char* q = jash_quote_step(p, &quote);
            if (q) { p = q - 1; continue; }   /* -1: o for faz o p++ */
        }
        if (back) { if (*p == '`') back = 0; continue; }

        if (*p == '`') { back = 1; continue; }
        if (*p == '$' && (p[1] == '(' || p[1] == '{')) { depth++; p++; continue; }
        if (depth > 0) {
            /* Dentro de uma expansão tudo é inerte para quem chama: um `;` em
             * `$(a; b)` é do sub-shell, não do laço de fora. */
            if (*p == '(' || *p == '{') depth++;
            else if (*p == ')' || *p == '}') depth--;
            continue;
        }

        if (strchr(stops, *p)) return p;
    }
    /* Nunca NULL: o chamador testa *p. */
    return s + strlen(s);
}

int jash_join_operator_lines(char* buf, int* pending_eof) {
    return jash_join_operator_lines_mapped(buf, NULL, pending_eof);
}

int jash_strip_comments_and_joins(const char* in, char* out, size_t cap) {
    return jash_strip_comments_and_joins_mapped(in, out, cap, NULL);
}
