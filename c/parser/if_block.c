// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Module: parser/if_block — multi-line if/elif/else/fi support.
// REWRITE: Tokenizer + AST approach (compiler frontend style).

#include "script.h"
#include "eval.h"
#include "param_expand.h"
#include "quotes.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

// ── Token types ─────────────────────────────────────────────────────────────
typedef enum {
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_ELIF,
    TOK_FI,
    TOK_CMD,      // A command string (may contain semicolons/newlines)
    TOK_EOF
} token_type_t;

typedef struct token {
    token_type_t type;
    char* text;           // For TOK_CMD: the command text. For keywords: NULL or the keyword.
    const char* origin;  // Exact borrowed slice in the enclosing source.
    struct token* next;
} token_t;

// ── AST nodes ───────────────────────────────────────────────────────────────
typedef struct {
    char* text;
    const char* origin;
} stmt_t;

typedef struct {
    stmt_t* stmts;        // Command strings and their lexical origins
    int count;
    int cap;
} stmt_list_t;

typedef struct {
    stmt_list_t cond;     // Condition commands (between if/elif and then)
    stmt_list_t body;     // Body commands (between then and next elif/else/fi)
} elif_branch_t;

typedef struct {
    stmt_list_t cond;     // Initial if condition
    stmt_list_t then_body;
    elif_branch_t* elifs;
    int elif_count;
    int elif_cap;
    stmt_list_t else_body;
} if_node_t;

// ── Tokenizer ───────────────────────────────────────────────────────────────
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

static int at_keyword(const char* p, const char* kw, int kwlen) {
    if (strncmp(p, kw, kwlen) != 0) return 0;
    char after = p[kwlen];
    return !after || after == ' ' || after == '\t' || after == '\n' || after == ';';
}

static token_t* new_token(token_type_t type, const char* text, size_t len) {
    token_t* tok = calloc(1, sizeof(token_t));
    tok->type = type;
    tok->origin = text;
    if (text && len > 0) {
        tok->text = malloc(len + 1);
        if (tok->text) {
            memcpy(tok->text, text, len);
            tok->text[len] = '\0';
            // Trim trailing whitespace/semicolons
            size_t i = len;
            while (i > 0 && (tok->text[i-1] == ' ' || tok->text[i-1] == '\t' ||
                             tok->text[i-1] == '\n' || tok->text[i-1] == ';')) {
                tok->text[--i] = '\0';
            }
        }
    }
    return tok;
}

static void free_tokens(token_t* head) {
    while (head) {
        token_t* next = head->next;
        if (head->text) free(head->text);
        free(head);
        head = next;
    }
}

// Tokenize the entire if block into a linked list.
// Context-aware: only the OUTER if/then/elif/else/fi are keywords.
// Nested if blocks are included as opaque CMD text.
static token_t* tokenize_if_block(const char* line) {
    token_t* head = NULL;
    token_t* tail = NULL;
    const char* p = skip_ws(line);
    int depth = 0;  // Nesting depth: 0 = outer level, >0 = inside nested if
    int in_body = 0;  // 1 = we've seen 'then' and are collecting body
    
    while (*p) {
        /* JINV-043: guarda de avanco — cada volta TEM que consumir um byte.
         * Se o coletor abaixo quebrar sem avancar e nenhum ramo de
         * palavra-chave consumir nada, o laco gira para sempre e o shell trava
         * sem mensagem. Foi o que aconteceu na primeira tentativa de corrigir
         * `fi` dentro de aspas (revertida). Com esta guarda o hang e
         * impossivel por construcao, seja qual for a logica do coletor. */
        const char* iter_start = p;

        p = skip_ws(p);
        if (!*p) break;

        // At depth 0, check for outer keywords
        if (depth == 0) {
            // 'if' is only a keyword at the very start (not in body)
            if (!in_body && at_keyword(p, "if", 2)) {
                token_t* tok = new_token(TOK_IF, NULL, 0);
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
                p += 2;
                continue;
            }
            if (at_keyword(p, "then", 4)) {
                token_t* tok = new_token(TOK_THEN, NULL, 0);
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
                p += 4;
                in_body = 1;  // Now we're in body mode
                continue;
            }
            if (at_keyword(p, "elif", 4)) {
                token_t* tok = new_token(TOK_ELIF, NULL, 0);
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
                p += 4;
                in_body = 0;  // Back to condition mode
                continue;
            }
            if (at_keyword(p, "else", 4)) {
                token_t* tok = new_token(TOK_ELSE, NULL, 0);
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
                p += 4;
                in_body = 1;  // Back to body mode
                continue;
            }
            if (at_keyword(p, "fi", 2)) {
                token_t* tok = new_token(TOK_FI, NULL, 0);
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
                p += 2;
                continue;
            }
        }

        // Collect command text (including nested if blocks) until next outer keyword
        const char* start = p;
        while (*p) {
            // Skip [[ ]] contents — they may contain keyword-like patterns (fi, then, etc.)
            // but the [[ ]] itself is part of the command text
            if (p[0] == '[' && p[1] == '[') {
                int bd = 1;
                p += 2;
                while (*p && bd > 0) {
                    if (p[0] == '[' && p[1] == '[') { bd++; p += 2; }
                    else if (p[0] == ']' && p[1] == ']') { bd--; p += 2; }
                    else { p++; }
                }
                continue;
            }

            /* JINV-083 — o coletor perguntava `at_keyword()` a CADA BYTE.
             *
             * `at_keyword` confere a fronteira a DIREITA e so ela: nao olha o
             * byte anterior e nao sabe o que e aspa. Entao, dentro do corpo de
             * um `if` multi-linha, o `fi` de `wififi` e o de `sci-fi x`
             * terminavam o bloco no meio da palavra, e o `echo fi` (o `fi` como
             * ARGUMENTO) virava fechador. Medido:
             *
             *     if true / then /   echo wififi / fi        bash: wififi
             *                                                jash: `wifi`
             *     if true / then /   echo sci-fi x / fi      bash: sci-fi x
             *                                                jash: `sci-`
             *     if true / then /   echo fi / fi            bash: `fi`
             *                                        jash: `fi: command not found`
             *
             * Agora quem procura e a leitura unica da casa: palavra inteira,
             * fora de aspas, em posicao de comando (parser/script.h). O ramo
             * do `[[ ]]` acima continua intacto, na frente, como estava. */
            static const jash_kw_t blk_kw[] = {
                { "if",   JASH_KW_END_OPEN,  1 },
                { "fi",   JASH_KW_END_CLOSE, 1 },
                { "then", JASH_KW_END_CLOSE, 1 },
                { "else", JASH_KW_END_CLOSE, 1 },
                { "elif", JASH_KW_END_CLOSE, 1 },
            };
            size_t which = 0;
            const char* after = 0;
            const char* hit = jash_find_keyword_set(p, blk_kw, 5, &which, &after);
            if (!hit) { while (*p) p++; break; }
            if (which == 0) {            /* if aninhado */
                depth++;
                p = after;
                continue;
            }
            if (which == 1) {            /* fi */
                if (depth > 0) {
                    depth--;
                    p = after;
                    continue;
                }
                // Outer fi: stop collecting (don't consume it)
                p = hit;
                break;
            }
            // At depth 0, stop at outer then/else/elif
            if (depth == 0) {
                p = hit;
                break;
            }
            p = after;
        }
        
        // Create CMD token with the collected text
        size_t len = p - start;
        if (len > 0) {
            token_t* tok = new_token(TOK_CMD, start, len);
            if (tok->text && tok->text[0]) {
                if (!head) head = tok; else tail->next = tok;
                tail = tok;
            } else {
                free(tok->text);
                free(tok);
            }
        }

        /* JINV-043: se a volta inteira nao consumiu nada, consome um byte
         * e segue — perder um caractere e ruim; travar o shell e
         * inaceitavel. Torna o hang IMPOSSIVEL por construcao. */
        if (p == iter_start && *p) p++;
    }
    
    return head;
}

// ── AST builder ─────────────────────────────────────────────────────────────
/* BUG-063: o `{` que abre um CORPO DE FUNCAO nao esta em posicao de comando.
 *
 * `jash_at_cmd_pos` responde certo para `{ echo a; }` — grupo de comandos — mas
 * em `function inner {` e em `inner() {` o `{` vem DEPOIS do nome, entao a
 * resposta e "nao", brace_depth nao sobe, e o `\n` seguinte quebra o enunciado
 * no meio da definicao. O corpo virava comandos soltos:
 *
 *     if true; then
 *       function inner {
 *         echo dentro          <- EXECUTAVA aqui, na definicao
 *       }
 *     fi
 *     inner                    <- inner: command not found
 *
 * Nao e "falha ao definir": o corpo RODA na hora errada, o que e pior — um
 * efeito colateral acontece e a funcao nao existe.
 *
 * Reconhece as quatro grafias do bash olhando o trecho ja percorrido do
 * enunciado: `function f {`, `function f() {`, `f() {`, `f () {`. */
static int brace_opens_function_body(const char* stmt_start, const char* brace) {
    const char* q = brace;
    while (q > stmt_start && (q[-1] == ' ' || q[-1] == '\t')) q--;   /* antes do { */

    /* forma `nome ( )` — recua pelo `)`, brancos, `(` */
    if (q > stmt_start && q[-1] == ')') {
        q--;
        while (q > stmt_start && (q[-1] == ' ' || q[-1] == '\t')) q--;
        if (q > stmt_start && q[-1] == '(') {
            q--;
            while (q > stmt_start && (q[-1] == ' ' || q[-1] == '\t')) q--;
        } else {
            return 0;                       /* `)` sem `(`: nao e cabecalho */
        }
    }

    /* agora tem que haver um NOME imediatamente a esquerda */
    const char* name_end = q;
    while (q > stmt_start &&
           (isalnum((unsigned char)q[-1]) || q[-1] == '_')) q--;
    if (q == name_end) return 0;            /* nenhum nome */

    /* `nome() {` ja basta; `function nome {` exige a palavra antes */
    if (name_end < brace) {
        const char* r = q;
        while (r > stmt_start && (r[-1] == ' ' || r[-1] == '\t')) r--;
        if (r - stmt_start >= 8 && !strncmp(r - 8, "function", 8)) {
            const char* before = r - 8;
            if (before == stmt_start || before[-1] == ' ' || before[-1] == '\t' ||
                before[-1] == '\n' || before[-1] == ';')
                return 1;
        }
    }
    /* `f() {` — o `)` recuado acima ja provou a forma */
    return (name_end < brace && *name_end != '\0' && brace[-1] == ')') ||
           (brace > stmt_start && (brace[-1] == ')' ||
            (brace[-1] == ' ' && brace - 2 > stmt_start && brace[-2] == ')')));
}

static void stmt_list_init(stmt_list_t* list) {
    list->stmts = NULL;
    list->count = 0;
    list->cap = 0;
}

static void stmt_list_add(stmt_list_t* list, const char* stmt, const char* origin) {
    if (list->count >= list->cap) {
        list->cap = list->cap ? list->cap * 2 : 4;
        list->stmts = realloc(list->stmts, list->cap * sizeof(stmt_t));
    }
    list->stmts[list->count].text = strdup(stmt);
    list->stmts[list->count++].origin = origin;
}

static void stmt_list_free(stmt_list_t* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->stmts[i].text);
    }
    free(list->stmts);
    list->stmts = NULL;
    list->count = list->cap = 0;
}

// Split a command string into individual statements (by ; and \n).
// Respects nested if/fi blocks - doesn't split inside them.
static void split_statements(stmt_list_t* list, const char* text, const char* origin) {
    if (!text || !*text) return;
    
    const char* p = text;
    while (*p) {
        /* JINV-064: guarda de avanco — a MESMA disciplina do JINV-043 que a
         * funcao irma `tokenize_if_block` (acima) ja tinha e esta aqui nao.
         * Toda volta do laco externo TEM que consumir pelo menos um byte: se o
         * coletor parar exatamente onde comecou, `end == start`, nada entra na
         * lista e a volta seguinte reproduz o mesmo estado byte a byte — o
         * shell gira a 100% de CPU, sem saida, sem mensagem, sem log, sem core,
         * para sempre. Com esta guarda o travamento e IMPOSSIVEL por
         * construcao, qualquer que seja a logica do coletor abaixo. */
        const char* iter_start = p;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') p++;
        if (!*p) break;

        const char* start = p;
        int if_depth = 0, case_depth = 0, loop_depth = 0;
        /* JINV-072 (segunda metade) — o divisor de enunciado tambem tem que saber o que e
         * ASPA e o que e PARENTESE. Ele so contava palavras-chave, entao um
         * `;` DENTRO de aspas ou dentro de um subshell cortava no meio:
         *
         *     if (FOO=FOO; unset FOO) >/dev/null 2>&1; then ...
         *     bash -> (roda o subshell)
         *     jash -> `(FOO=FOO: command not found`
         *
         * Essa linha e o cabecalho de TODO `configure` de autoconf (linha 62
         * do de ncurses) — e o mesmo dado-virando-comando do JINV-072, so que
         * pela outra fronteira. Idem `echo "a;b"` dentro de um bloco.
         *
         * A leitura e a MESMA das outras portas (aspa simples literal, `\`
         * escapando fora de aspa simples): nenhum analisador novo. */
        int paren_depth = 0;
        /* ═══ JINV-084 — o divisor conhecia TODO composto menos o de chaves ══
         *
         * Ele conta `if`/`case`/laço/parêntese/aspas justamente para não cortar
         * no meio de um enunciado — e não contava `{ }`. Então o `;` e a
         * nova-linha DENTRO de um grupo cortavam, e cada pedaço virava um
         * comando por conta própria. Medido, com o bash de oráculo:
         *
         *     if true; then
         *     { echo in; }
         *     fi
         *       bash -> in
         *       jash -> `{: command not found` + `}: command not found`
         *
         * (o `{ echo in` vira o comando `{` com dois argumentos e o `}` sobra
         * sozinho — o corpo do grupo NÃO roda.) Mesma classe dos JINV-064/072:
         * o divisor precisa saber onde o enunciado acaba, e um grupo de chaves
         * é UM enunciado.
         *
         * Sem varredura nova: as duas formas públicas de parser/script.h —
         * `jash_keyword_at` (fronteira à direita) e `jash_at_cmd_pos` (a regra
         * de posição de comando lida para TRÁS, que existe exatamente para quem,
         * como este laço, já tem estado próprio e não pode adotar o andar do
         * scanner). É o que faz `echo }` continuar sendo DADO aqui dentro. */
        int brace_depth = 0;
        char sq = 0, dq = 0;
        while (*p) {
            if (sq) { if (*p == '\'') sq = 0; p++; continue; }
            if (dq) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') dq = 0;
                p++; continue;
            }
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '\'') { sq = 1; p++; continue; }
            if (*p == '"')  { dq = 1; p++; continue; }
            if (*p == '(')  { paren_depth++; p++; continue; }
            if (*p == ')')  { if (paren_depth > 0) paren_depth--; p++; continue; }
            /* JINV-084 — `{`/`}` como palavras reservadas, não como bytes.
             * `{` só abre seguido de branco (senão `{a,b}` e `{echo` abririam
             * um grupo que ninguém fecha); `}` só fecha em posição de comando
             * (senão o `}` de `echo }` fecharia). */
            if (*p == '{' && jash_keyword_at(p, "{", JASH_KW_END_BRACE) &&
                (jash_at_cmd_pos(text, p) ||
                 brace_opens_function_body(start, p))) {   /* BUG-063 */
                brace_depth++; p++; continue;
            }
            if (*p == '}' && jash_at_cmd_pos(text, p) &&
                jash_keyword_at(p, "}", JASH_KW_END_ANY)) {
                if (brace_depth > 0) brace_depth--;
                p++; continue;   /* JINV-064: `}` órfã é palavra, não fim */
            }
            if (at_keyword(p, "if", 2)) { if_depth++; p += 2; continue; }
            if (at_keyword(p, "fi", 2)) {
                if (if_depth > 0) { if_depth--; p += 2; continue; }
                /* JINV-064: fechador SEM abertura correspondente nao termina
                 * enunciado nenhum — e uma palavra comum, exatamente como o
                 * bash a le (`echo done` imprime `done`). O `break` que estava
                 * aqui devolvia o ponteiro no ponto em que a volta comecou, e
                 * o laco externo girava eterno. Consome como texto e segue. */
                p += 2; continue;
            }
            if (at_keyword(p, "case", 4)) { case_depth++; p += 4; continue; }
            if (at_keyword(p, "esac", 4)) {
                if (case_depth > 0) { case_depth--; p += 4; continue; }
                p += 4; continue;   /* JINV-064: `esac` orfa e palavra, nao fim */
            }
            if (at_keyword(p, "for", 3) || at_keyword(p, "while", 5) ||
                at_keyword(p, "until", 5) || at_keyword(p, "select", 6)) {
                loop_depth++;
                p += (at_keyword(p, "for", 3) ? 3 :
                      at_keyword(p, "while", 5) ? 5 :
                      at_keyword(p, "until", 5) ? 5 : 6);
                continue;
            }
            if (at_keyword(p, "done", 4)) {
                if (loop_depth > 0) { loop_depth--; p += 4; continue; }
                p += 4; continue;   /* JINV-064: `done` orfao e palavra, nao fim */
            }
            if (if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                paren_depth == 0 &&                   /* JINV-072 */
                brace_depth == 0 &&                   /* JINV-084 */
                (*p == ';' || *p == '\n')) {
                break;
            }
            p++;
        }

        const char* end = p;

        if (end > start) {
            size_t len = end - start;
            char* stmt = malloc(len + 1);
            if (stmt) {
                memcpy(stmt, start, len);
                stmt[len] = '\0';
                /* JINV-063: o rtrim era CEGO e vinha ANTES da copia — comia
                 * tambem o espaco ESCAPADO, que e DADO (`if true; then
                 * echo x\  ; fi` imprimia `x\`). Copia primeiro, corta com a
                 * leitura consciente de aspas depois (parser/quotes.h). */
                stmt[jash_rtrim_unquoted(stmt)] = '\0';
                if (stmt[0]) stmt_list_add(list, stmt, origin + (start - text));
                free(stmt);
            }
        }

        /* JINV-064: se a volta inteira nao consumiu nada, consome um byte e
         * segue. Perder um caractere e ruim; travar o shell e inaceitavel.
         * Com os fechadores orfaos consumidos acima esta guarda nunca dispara
         * hoje — ela existe para que NENHUMA alteracao futura do coletor possa
         * reintroduzir o laco infinito sem ser pega antes de sair daqui. */
        if (p == iter_start && *p) p++;
    }
}

static void free_if_node(if_node_t* node) {
    stmt_list_free(&node->cond);
    stmt_list_free(&node->then_body);
    for (int i = 0; i < node->elif_count; i++) {
        stmt_list_free(&node->elifs[i].cond);
        stmt_list_free(&node->elifs[i].body);
    }
    free(node->elifs);
    stmt_list_free(&node->else_body);
}

// Build AST from token stream
static int build_if_ast(token_t* tokens, if_node_t* node) {
    memset(node, 0, sizeof(if_node_t));
    stmt_list_init(&node->cond);
    stmt_list_init(&node->then_body);
    stmt_list_init(&node->else_body);
    
    token_t* tok = tokens;
    
    // Expect IF token
    if (!tok || tok->type != TOK_IF) return -1;
    tok = tok->next;
    
    // Collect condition commands until THEN
    while (tok && tok->type == TOK_CMD) {
        split_statements(&node->cond, tok->text, tok->origin);
        tok = tok->next;
    }
    
    // Expect THEN
    if (!tok || tok->type != TOK_THEN) return -1;
    tok = tok->next;
    
    // Collect then-body until ELSE/ELIF/FI
    while (tok && tok->type == TOK_CMD) {
        split_statements(&node->then_body, tok->text, tok->origin);
        tok = tok->next;
    }
    
    // Process elif branches
    while (tok && tok->type == TOK_ELIF) {
        tok = tok->next;  // Skip ELIF
        
        // Grow elif array
        if (node->elif_count >= node->elif_cap) {
            node->elif_cap = node->elif_cap ? node->elif_cap * 2 : 4;
            node->elifs = realloc(node->elifs, node->elif_cap * sizeof(elif_branch_t));
        }
        elif_branch_t* branch = &node->elifs[node->elif_count++];
        stmt_list_init(&branch->cond);
        stmt_list_init(&branch->body);
        
        // Collect condition
        while (tok && tok->type == TOK_CMD) {
            split_statements(&branch->cond, tok->text, tok->origin);
            tok = tok->next;
        }
        
        // Expect THEN
        if (!tok || tok->type != TOK_THEN) return -1;
        tok = tok->next;
        
        // Collect body
        while (tok && tok->type == TOK_CMD) {
            split_statements(&branch->body, tok->text, tok->origin);
            tok = tok->next;
        }
    }
    
    // Process else
    if (tok && tok->type == TOK_ELSE) {
        tok = tok->next;  // Skip ELSE
        while (tok && tok->type == TOK_CMD) {
            split_statements(&node->else_body, tok->text, tok->origin);
            tok = tok->next;
        }
    }
    
    // Expect FI
    if (!tok || tok->type != TOK_FI) return -1;
    
    return 0;
}

/* ═══ JINV-097 — O COMPOSTO EXPANDIDO UMA VEZ, ANTES DE HAVER ESTADO ═══════
 *
 * `eval_stmt_list` expandia TODO enunciado do corpo do `if` antes de rodá-lo.
 * Para um comando simples isso é inofensivo — ele roda uma vez, com o estado
 * que existe naquele instante. Para um COMPOSTO (laço, `case`, `if` aninhado,
 * grupo de chaves, subshell, definição de função) é fatal: o texto INTEIRO —
 * corpo, condição, incremento — é expandido UMA vez, contra o estado ANTERIOR
 * ao laço começar. A variável de iteração ainda não existe, então some; a
 * condição do laço é congelada no valor que tinha antes da primeira volta.
 *
 * Medido, com o /bin/bash de oráculo:
 *
 *     x=OUTER / if true; then / for i in 1 2; do / echo "[$i][$x]" / done / fi
 *       bash -> [1][OUTER] [2][OUTER]      jash -> [][OUTER] [][OUTER]
 *
 *     if true; then / for i in 1 2 3; do / test "$i" = 2 && continue
 *       echo "c$i" / done / fi
 *       bash -> c1 c3                      jash -> c c c   (o `continue` some)
 *
 *     if true; then / i=0 / while [ $i -lt 3 ]; do / i=$((i+1)) / done / fi
 *       bash -> termina                    jash -> NÃO TERMINA
 *
 * O terceiro é o pior: `[ $i -lt 3 ]` vira `[ 0 -lt 3 ]` para sempre e o laço
 * gira eterno — sem mensagem, sem rc, sem rastro. `$x` (setada ANTES) expande
 * certo; `$i` (setada a cada volta) expande vazio. É a assinatura exata de
 * "expandir num ponto onde o estado ainda não era o certo": a mesma família do
 * JINV-074 (cauda com dois donos) e do JINV-090 (restauro do escudo antes do
 * cano, cedo demais para servir de blindagem).
 *
 * ═══ Por que a expansão prematura existia ═══
 *
 * O comentário original alegava: "o tokenizador extrai o texto do corpo da
 * função ANTES de as variáveis locais serem setadas; sem isto, `if [[ -n "$x" ]]`
 * dentro de uma função veria o literal $x". Essa alegação foi MEDIDA e não se
 * sustenta mais: as 13 formas dela (local + `[[ ]]`, local + `[ ]`, `$1` na
 * condição, `$1` no corpo, `elif` com `$var`, aritmética, `$( )`, cifrão em
 * aspas simples, …) dão o MESMO resultado do bash com e sem a expansão — hoje
 * quem expande é `jash_eval_line`, e ele já roda em cima do estado certo. Duas
 * provas independentes: o caminho de UMA LINHA (`jash_eval_if_statement`, em
 * script.c) NUNCA pré-expandiu e sempre acertou, e o mesmo laço no TOPO do
 * script também.
 *
 * ═══ O conserto ═══
 *
 * A expansão prematura CONTINUA, intacta, para o enunciado simples — é ela que
 * honra a alegação acima caso algum caminho ainda dependa dela. O que muda é
 * que o COMPOSTO passa cru: quem sabe expandir o corpo de um laço a cada volta
 * é o avaliador do laço, e ele já faz isso quando o laço está no topo. Não há
 * varredura nova — a pergunta "isto abre um composto?" é UMA chamada ao
 * scanner único da casa (`jash_find_keyword_set`, parser/script.h), com
 * `cmdpos=1`, que é o que faz `echo for` continuar sendo DADO.
 *
 * Gate: tests/test_jinv097_if_body_late_expand.sh                          */
static int stmt_opens_compound(const char* stmt) {   /* JINV-097 */
    if (!stmt || !*stmt) return 0;

    /* 1. Palavra reservada que ABRE um composto, em POSIÇÃO DE COMANDO.
     *    Os terminadores são os mesmos que `jash_find_loop_close` usa para os
     *    laços (JASH_KW_END_OPEN exige argumento, e cobre o `for((` colado);
     *    `case`/`if`/`function` aceitam também a nova-linha porque, dentro de
     *    um bloco, o enunciado guardado pode ter a quebra logo depois. */
    static const jash_kw_t open_kw[] = {
        { "for",      JASH_KW_END_OPEN, 1 },
        { "while",    JASH_KW_END_OPEN, 1 },
        { "until",    JASH_KW_END_OPEN, 1 },
        { "select",   JASH_KW_END_OPEN, 1 },
        { "case",     " \t\n",          1 },
        { "if",       " \t\n(",         1 },
        { "function", " \t\n",          1 },
    };
    if (jash_find_keyword_set(stmt, open_kw,
                              sizeof(open_kw) / sizeof(open_kw[0]), 0, 0)) {
        return 1;
    }

    /* 2. As três formas que não são palavra e por isso o scanner não procura.
     *    Ancoradas no COMEÇO do enunciado — não é varredura, é uma olhada no
     *    primeiro byte significativo. */
    const char* p = stmt;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;

    /* subshell `( … )` — e também `(( … ))`, cujo corpo é lido pelo avaliador
     * de aritmética, que consulta a variável ele mesmo. */
    if (*p == '(') return 1;

    /* grupo de chaves `{ … }` — a MESMA regra do divisor de enunciado
     * (JINV-084): `{` exige branco depois, senão `{a,b}` e `{echo` abririam um
     * grupo que ninguém fecha. */
    if (*p == '{' && jash_keyword_at(p, "{", JASH_KW_END_BRACE)) return 1;

    /* definição de função `nome() { … }` / `nome () { … }`. O corpo é GUARDADO
     * para rodar depois, então expandi-lo aqui é o mesmo erro em estado puro:
     * medido, `f() { echo "fn:$1"; }` definido dentro de um `if` imprimia
     * `fn:` — o `$1` foi expandido na DEFINIÇÃO, quando não havia argumento. */
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        const char* q = p + 1;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
               (*q >= '0' && *q <= '9') || *q == '_') q++;
        const char* r = q;
        while (*r == ' ' || *r == '\t') r++;
        if (r[0] == '(') {
            const char* t = r + 1;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == ')') return 1;
        }
    }

    return 0;
}

// ── AST executor ────────────────────────────────────────────────────────────
static int eval_stmt_list(jash_state_t* s, jash_writer_t w, void* ctx,
                          stmt_list_t* list) {
    int last_rc = 0;
    for (int i = 0; i < list->count; i++) {
        const char* stmt = list->stmts[i].text;
        unsigned* map = jash_source_map_slice(list->stmts[i].origin, strlen(stmt));
        if (!map) {
            jash_writes(jash_stderr_writer, 0, "jash: source location allocation failed\n");
            s->exit_requested = 1;
            s->last_status.code = 2;
            return 2;
        }
        jash_source_span span;
        jash_source_push(&span, stmt, strlen(stmt), map, map[0]);
        /* Expand $VAR in condition/body text JUST BEFORE evaluation.
         * This is needed because the if-block tokenizer extracts text from
         * the function body BEFORE local variables are set. Without this,
         * `if [[ -n "$x" ]]` inside a function would see literal "$x".
         *
         * JINV-097: … mas SÓ para o enunciado simples. Um composto expandido
         * aqui é expandido UMA vez, contra o estado anterior ao laço começar —
         * a variável de iteração some e a condição do laço congela (laço
         * eterno). Quem expande o corpo do composto a cada volta é o avaliador
         * dele, exatamente como já acontece quando o laço está no topo. */
        if (!stmt_opens_compound(stmt) &&               /* JINV-097 */
            jash_has_param_expansion(stmt)) {
            const char* expanded = jash_param_expand(stmt, s);
            if (expanded && expanded != stmt) {
                stmt = expanded;
            }
        }
        /* An expanded simple command belongs to this statement's source
         * line; its replacement bytes are not offsets in the raw text. */
        last_rc = (stmt == list->stmts[i].text)
                ? jash_eval_line(s, w, ctx, stmt)
                : jash_eval_line_mapped(s, w, ctx, stmt, NULL, map[0]);
        jash_source_pop(&span);
        free(map);
        /* Propagate break (255) and continue (254) immediately */
        if (s->exit_requested || last_rc == 255 || last_rc == 254) break;
    }
    return last_rc;
}

static int eval_condition(jash_state_t* s, jash_writer_t w, void* ctx,
                          stmt_list_t* cond) {
    // Condition is true if last command exits with 0
    int rc = eval_stmt_list(s, w, ctx, cond);
    return (rc == 0) ? 1 : 0;
}

// ── Main entry point ────────────────────────────────────────────────────────
int jash_eval_if_block(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* line) {
    // Tokenize
    token_t* tokens = tokenize_if_block(line);
    if (!tokens) return -1;
    
    // Build AST
    if_node_t node;
    if (build_if_ast(tokens, &node) != 0) {
        free_tokens(tokens);
        return -1;
    }
    
    // Execute
    int last_rc = 0;
    int executed = 0;
    
    // Check initial if condition
    int cond_true = eval_condition(s, w, ctx, &node.cond);
    if (s->exit_requested) {
        last_rc = s->last_status.code;
        goto if_done;
    }
    if (cond_true) {
        last_rc = eval_stmt_list(s, w, ctx, &node.then_body);
        executed = 1;
    }
    
    // Check elif branches
    if (!executed) {
        for (int i = 0; i < node.elif_count; i++) {
            cond_true = eval_condition(s, w, ctx, &node.elifs[i].cond);
            if (s->exit_requested) {
                last_rc = s->last_status.code;
                goto if_done;
            }
            if (cond_true) {
                last_rc = eval_stmt_list(s, w, ctx, &node.elifs[i].body);
                executed = 1;
                break;
            }
        }
    }
    
    // Execute else if nothing matched
    if (!executed && node.else_body.count > 0) {
        last_rc = eval_stmt_list(s, w, ctx, &node.else_body);
    }
    
if_done:
    // Cleanup
    free_if_node(&node);
    free_tokens(tokens);
    if (s->exit_requested) return last_rc;
    
    /* Find position after 'fi' and execute remaining commands.
     *
     * JINV-083 — a irma que o JINV-072 nao alcancou. Esta varredura casava
     * `if`/`fi` em QUALQUER posicao do texto: sem fronteira a ESQUERDA, sem
     * nocao de aspas, sem posicao de comando. Medido, com o bash de oraculo,
     * em bloco `if` multi-linha:
     *
     *     if true / then /   echo fi / fi / echo cauda
     *       bash -> `fi` + `cauda`   jash -> `fi: command not found`
     *     ...  echo wififi  ...
     *       bash -> `wififi`         jash -> `wifi`
     *     ...  echo "a fi b"  ...
     *       bash -> `a fi b`         jash -> `echo: unterminated quote` + `b"`
     *
     * O `fi` de dentro da palavra fechava o bloco e o RABO virava comando — o
     * mesmo estrago do JINV-072, no arquivo ao lado, sobrevivendo aqui. Agora
     * usa a leitura unica da casa (parser/script.h). */
    static const jash_kw_t if_kw[] = {          /* JINV-083 */
        { "if", JASH_KW_END_OPEN,  1 },
        { "fi", JASH_KW_END_CLOSE, 1 },
    };
    const char* p = line;
    int depth = 0;
    while (*p) {
        size_t which = 0;
        const char* after = 0;
        const char* hit = jash_find_keyword_set(p, if_kw, 2, &which, &after);
        if (!hit) break;
        if (which == 0) {
            depth++;
        } else {
            depth--;
            if (depth == 0) {
                p = after;   /* logo depois do `fi` — sem `+2` a adivinhar */
                // Skip whitespace and semicolons
                while (*p == ' ' || *p == '\t' || *p == ';' || *p == '\n') p++;
                // Execute remaining commands ONLY if no break/continue
                if (*p && last_rc != 255 && last_rc != 254) {
                    int rc2 = jash_eval_line(s, w, ctx, p);
                    if (rc2 != 0) last_rc = rc2;
                }
                break;
            }
        }
        p = after;
    }
    
    return last_rc;
}
