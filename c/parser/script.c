// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/script — if/for/while/case statement evaluation.
//
// Phase A6: minimal if-then-else-fi support.
// Syntax: if <cmd>; then <cmd>; [else <cmd>;] fi
//
// Stores script state in jash_state.script_{lines,count,depth} to track
// multi-line constructs (nested if, for, etc).

#include "script.h"
#include "eval.h"
#include <sys/syscalls.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <string.h>

/* JINV-072 — palavra-chave e PALAVRA, nunca SUBSTRING.
 *
 * Este arquivo procurava os terminadores com `strstr(p, " fi")`. `" fi"` casa
 * DENTRO de `" first;"`, de `" file"`, de `" fine"`, de `" filter"`. O efeito
 * nao era um erro de sintaxe — era pior:
 *
 *     $ if true; then echo first; fi
 *     bash -> first
 *     jash -> (nada)  +  rst;: command not found
 *
 * O corpo NAO rodava e o rabo da propria palavra virava COMANDO. Tres agentes
 * diagnosticaram isso como "if de uma linha trava". E a mesma familia do
 * JINV-016/017/018/019: DADO virando COMANDO por leitura cega de texto.
 *
 * Duas coisas mudam aqui e as duas sao o conserto: a busca exige FRONTEIRA DE
 * PALAVRA dos dois lados, e ela e CONSCIENTE DE ASPAS — `echo 'fi'` deixa de
 * terminar o bloco. Devolve o inicio da palavra-chave (a lista antiga de
 * separadores devolvia o separador; quem chama agora recebe tambem, em
 * `after`, o ponto logo depois da palavra, para nao ter que recontar bytes).
 *
 * Devolve a occorrencia MAIS A ESQUERDA — a lista antiga percorria os
 * separadores em ordem (` `, `\t`, `\n`, `;`) e parava no primeiro TIPO que
 * casasse em qualquer lugar, entao um `\n`+then anterior perdia para um
 * ` then` posterior. */

/* ═══ JINV-083 — a MESMA leitura, agora tambem para as palavras de LAÇO ════
 *
 * O JINV-072 consertou `then`/`else`/`fi` aqui, e um agente seguinte auditou
 * `for_loop.c`/`while_loop.c`, viu `strncmp` COM checagem de fronteira, e
 * concluiu que estavam sãos. Estavam meio sãos: **fronteira de palavra não é
 * consciência de aspas.** Em `echo "ask for help"` o `for ` É uma palavra bem
 * delimitada — só que citada. Medido, com o bash de oráculo:
 *
 *     for i in 1; do echo "ask for help"; done
 *       bash -> ask for help      jash -> jash: ... (missing done)
 *     for i in 1; do echo "ask 4 help"; done       (controle)
 *       bash -> ask 4 help        jash -> ask 4 help
 *
 * E faltava a metade do outro lado: aquelas varreduras não exigiam fronteira
 * À ESQUERDA de `done` — `strncmp(p,"done",4)` casa DENTRO de `fordone`, de
 * `undone`, de `whiledone`, e o rabo da palavra virava comando:
 *
 *     for i in 1; do echo undone; done
 *       bash -> undone            jash -> `un` + `done: command not found`
 *
 * É a família do JINV-016/017/018/019/072: DADO virando ESTRUTURA por leitura
 * cega de texto. Impacto medido: 6 dos 19 `configure` de autoconf erravam o
 * `done` só por isto (`echo "$0 --help for help"` mata o do zlib; o padrão de
 * `case` do autoconf `| --program-transfor \` termina em `for `).
 *
 * O conserto NÃO é uma quarta varredura — é ESTA, generalizada para um
 * CONJUNTO de palavras, cada uma com o seu conjunto de terminadores: uma
 * palavra que exige argumento (`for`) nunca é palavra-chave em `for)`; um
 * fechador (`done`) pode terminar em `;`. `find_keyword` continua existindo,
 * agora como o caso de uma palavra só.
 *
 * ═══ A TERCEIRA PROPRIEDADE: POSIÇÃO DE COMANDO ══════════════════════════
 *
 * Palavra inteira + fora de aspas ainda deixava passar o caso mais banal de
 * todos, medido com o bash de oraculo:
 *
 *     for i in 1; do echo done; done
 *       bash -> done              jash -> (nada)
 *     for i in 1; do echo a done b; done
 *       bash -> a done b          jash -> `a`
 *
 * `done` ali e ARGUMENTO do echo, nao fechador. A regra do POSIX 2.9 e que
 * palavra reservada so e reservada em POSIÇÃO DE COMANDO — inicio da regiao,
 * ou depois de `;` `\n` `&` `|` `(` `)` `{` `}`, ou depois de uma palavra que
 * ABRE uma lista (`do`, `then`, `else`, `elif`). Quem pede `cmdpos` recebe
 * essa regra; quem nao pede (o `then`/`else`/`fi` do JINV-072) continua com o
 * comportamento de antes, byte por byte.
 *
 * Gate: tests/test_jinv083_keyword_in_quotes.sh */

/* Palavras depois das quais COMEÇA um comando. Sem esta lista, o `for` de
 * `if …; then for j in …; do …; done; fi` deixaria de ser contado — a lista e
 * gramatica de shell, e por isso mora aqui dentro, no unico leitor. */
static size_t jash_kw_list_intro(const char* q) {
    static const char* const intro[] = { "do", "then", "else", "elif" };
    for (size_t i = 0; i < sizeof(intro) / sizeof(intro[0]); i++) {
        size_t l = strlen(intro[i]);
        if (strncmp(q, intro[i], l) != 0) continue;
        char nx = q[l];
        if (nx == ' ' || nx == '\t' || nx == '\n' || nx == ';') return l;
    }
    return 0;
}

/* A MESMA regra de posição de comando, lida para TRÁS (JINV-083).
 *
 * `jash_find_keyword_set` acompanha a posição enquanto anda; quem já tem a sua
 * própria varredura com estado (é o caso de `find_top_separator`, em eval.c,
 * que rastreia aspas, crase, parênteses, chaves e `[[ ]]` ao mesmo tempo) não
 * pode adotar o andar deste arquivo sem reescrever o dele. Então a regra mora
 * aqui, nas duas formas, LADO A LADO e com o mesmo conjunto de caracteres e a
 * mesma lista de palavras que abrem lista — para que não possam divergir sem
 * que quem edite uma veja a outra.
 *
 * `region` é o começo do texto lido: esta função nunca olha antes dele.
 *
 * Diferença conhecida e proposital: a forma de trás não enxerga a junção
 * `\`+nova-linha (que some do texto e, portanto, NÃO cria posição de comando).
 * Quem precisa desse detalhe usa a forma da frente. */
int jash_at_cmd_pos(const char* region, const char* p) {
    if (!region || !p || p <= region) return 1;
    /* Recua sobre brancos. NAO e um corte de espaco final (JINV-063): nao
     * altera texto nenhum, so procura o ultimo byte significativo antes de
     * `p`. Escrito em `for` de proposito — a catraca do JINV-063 conta a FORMA
     * textual do rtrim cego, e o numero dela so pode descer; um recuo que nao
     * corta nada nao deve gastar uma vaga daquele teto. */
    const char* q = p;
    for (;;) {
        if (q <= region) return 1;
        char b = q[-1];
        if (b != ' ' && b != '\t') break;
        q--;
    }
    if (q == region) return 1;
    char c = q[-1];
    if (c == ';' || c == '\n' || c == '&' || c == '|' ||
        c == '(' || c == ')' || c == '{' || c == '}') return 1;
    /* A palavra anterior abre uma lista (`do`/`then`/`else`/`elif`)? */
    const char* w = q;
    while (w > region && w[-1] != ' ' && w[-1] != '\t' && w[-1] != ';' &&
           w[-1] != '\n' && w[-1] != '&' && w[-1] != '|' &&
           w[-1] != '(' && w[-1] != ')') w--;
    return jash_kw_list_intro(w) == (size_t)(q - w);
}

const char* jash_find_keyword_set(const char* hay, const jash_kw_t* kws, size_t nk,
                                  size_t* which, const char** after) {
    if (!hay || !kws || nk == 0) return 0;
    char sq = 0, dq = 0;
    int boundary = 1;              /* o inicio da regiao e fronteira */
    int cmdpos  = 1;               /* ...e e posicao de comando */
    for (const char* q = hay; *q; ) {
        char c = *q;
        if (sq) { if (c == '\'') sq = 0; q++; boundary = 0; cmdpos = 0; continue; }
        if (dq) {
            if (c == '\\' && q[1]) { q += 2; boundary = 0; cmdpos = 0; continue; }
            if (c == '"') dq = 0;
            q++; boundary = 0; cmdpos = 0; continue;
        }
        /* `\` + nova-linha e uma juncao de linhas: some do texto (POSIX 2.2.1),
         * entao NAO muda fronteira nem posicao — um `done` na linha seguinte
         * continua sendo o fechador. */
        if (c == '\\' && q[1] == '\n') { q += 2; continue; }
        if (c == '\\' && q[1]) { q += 2; boundary = 0; cmdpos = 0; continue; }
        if (c == '\'') { sq = 1; q++; boundary = 0; cmdpos = 0; continue; }
        if (c == '"')  { dq = 1; q++; boundary = 0; cmdpos = 0; continue; }
        if (boundary) {
            for (size_t k = 0; k < nk; k++) {
                const char* kw = kws[k].word;
                if (!kw) continue;
                if (kws[k].cmdpos && !cmdpos) continue;
                size_t kl = strlen(kw);
                if (kl == 0 || strncmp(q, kw, kl) != 0) continue;
                char nx = q[kl];
                const char* endset = kws[k].endset ? kws[k].endset
                                                   : JASH_KW_END_ANY;
                if (!nx || strchr(endset, nx)) {
                    if (which) *which = k;
                    if (after) *after = q + kl;
                    return q;
                }
            }
            /* Nao era nenhuma das procuradas: e uma das que ABREM uma lista?
             * Entao o que vem depois dela esta em posicao de comando. */
            size_t il = jash_kw_list_intro(q);
            if (il) { q += il; boundary = 1; cmdpos = 1; continue; }
        }
        boundary = (c == ' ' || c == '\t' || c == '\n' || c == ';' ||
                    c == '&' || c == '|' || c == '(');
        if (c == ';' || c == '\n' || c == '&' || c == '|' ||
            c == '(' || c == ')' || c == '{' || c == '}') cmdpos = 1;
        else if (c != ' ' && c != '\t') cmdpos = 0;
        q++;
    }
    return 0;
}

/* Uma palavra so, com o conjunto historico de terminadores — exatamente o que
 * `find_keyword` sempre foi (JINV-072), agora publico. */
const char* jash_find_keyword(const char* hay, const char* kw,
                              const char** after) {
    if (!kw) return 0;
    jash_kw_t one;
    one.word = kw;
    one.endset = JASH_KW_END_ANY;
    one.cmdpos = 0;   /* JINV-072 nao pedia posicao; nao muda agora */
    return jash_find_keyword_set(hay, &one, 1, 0, after);
}

/* A palavra comeca EXATAMENTE aqui? (JINV-083)
 *
 * O scanner comeca com `boundary = 1`, entao "casou em `p`" ja significa
 * "palavra inteira em `p`". Quem so precisa da fronteira a DIREITA usa isto em
 * vez de escrever mais um `strncmp` nu — foi um `strncmp(p,"esac",4)` sem
 * fronteira que fazia `case esacx in esacx) ...` nunca casar. */
int jash_keyword_at(const char* p, const char* kw, const char* endset) {
    if (!p || !kw) return 0;
    jash_kw_t one;
    one.word = kw;
    one.endset = endset ? endset : JASH_KW_END_ANY;
    one.cmdpos = 0;   /* quem ja esta posicionado nao precisa da regra */
    return jash_find_keyword_set(p, &one, 1, 0, 0) == p;
}

/* O `done` que fecha o laco ja aberto — a UNICA copia desta contagem.
 *
 * JINV-083: os abridores exigem branco (ou `(`, do `for((`) logo depois, entao
 * `$(echo for)` segue sendo DADO, como sempre foi, e um `for((` aninhado sem
 * espaco passa a ser contado (as versoes antigas so viam `for `/`for\t` e
 * perdiam esse — medido: `for i in 1; do for((j=0;j<1;j++)); do echo x; done;
 * done` imprimia `done: command not found`). */
const char* jash_find_loop_close(const char* body) {
    static const jash_kw_t loop_kw[] = {
        { "for",    JASH_KW_END_OPEN,  1 },
        { "while",  JASH_KW_END_OPEN,  1 },
        { "until",  JASH_KW_END_OPEN,  1 },
        { "select", JASH_KW_END_OPEN,  1 },
        { "done",   JASH_KW_END_CLOSE, 1 },
    };
    const size_t nkw = sizeof(loop_kw) / sizeof(loop_kw[0]);
    const size_t closer = nkw - 1;

    if (!body) return 0;
    int depth = 1;
    const char* p = body;
    while (*p) {
        size_t which = 0;
        const char* after = 0;
        const char* hit = jash_find_keyword_set(p, loop_kw, nkw, &which, &after);
        if (!hit) return 0;
        if (which == closer) {
            if (--depth == 0) return hit;
        } else {
            depth++;
        }
        /* Retomar logo depois da palavra: a busca so casa FORA de aspas, entao
         * o estado de citacao em `after` e sempre "fora" — reiniciar o scanner
         * ali e correto. */
        p = after;
    }
    return 0;
}

/* ═══ JINV-084 — o `}` que fecha o grupo, pela MESMA leitura ══════════════
 *
 * O grupo de chaves era o único composto sem dono: `eval.c` contava `{` e `}`
 * byte a byte, sem aspas, sem fronteira e sem posição de comando —
 *
 *     const char* p = line + 1; int depth = 1;
 *     while (*p && depth > 0) { if (*p=='{') depth++; else if (*p=='}') depth--; ... }
 *
 * — e `main.c` nem sabia que `{` abria alguma coisa, então `{` sozinho numa
 * linha era executado como COMANDO. Medido, com o bash de oráculo:
 *
 *     { echo hi; }              bash -> hi        jash -> hi           (ok)
 *     {\n echo hi\n }           bash -> hi        jash -> `{: command not found`
 *                                                       + `}: command not found`
 *     { echo a; } && echo b     bash -> a + b     jash -> a     (a cauda SUMIA)
 *
 * Impacto medido: 5 dos 10 `configure` travados imprimiam `}: command not
 * found` imediatamente antes de morrer — é o `as_fn_*` de varredura de PATH do
 * autoconf, que escreve o corpo da função como grupo multi-linha.
 *
 * Não há varredura nova aqui: é `jash_find_keyword_set`, a mesma que serve
 * `for`/`while`/`until`/`select`/`done` logo acima, com o par `{`/`}` e os
 * terminadores que cada um exige. A auditoria da casa já achou SETE pares
 * "duas implementações da mesma coisa, uma empobrecida"; a contagem cega de
 * eval.c era a oitava, e some agora em favor desta.
 *
 * Gate: tests/test_jinv084_brace_group.sh */
const char* jash_find_brace_close(const char* body) {
    static const jash_kw_t brace_kw[] = {
        /* `{` exige branco depois — `{a,b}` e `{echo` NÃO abrem grupo. */
        { "{", JASH_KW_END_BRACE, 1 },
        /* `}` fecha com qualquer terminador (` \t\n;&|)`), mas SÓ em posição
         * de comando: o `}` de `echo }` e o do padrão `}) …` de um `case`
         * continuam sendo DADO. */
        { "}", JASH_KW_END_ANY,   1 },
    };
    const size_t nkw = sizeof(brace_kw) / sizeof(brace_kw[0]);
    const size_t closer = nkw - 1;

    if (!body) return 0;
    int depth = 1;
    const char* p = body;
    while (*p) {
        size_t which = 0;
        const char* after = 0;
        const char* hit = jash_find_keyword_set(p, brace_kw, nkw, &which, &after);
        if (!hit) return 0;
        if (which == closer) {
            if (--depth == 0) return hit;
        } else {
            depth++;
        }
        /* Mesma retomada de `jash_find_loop_close`: a busca só casa FORA de
         * aspas, então o estado de citação em `after` é sempre "fora". */
        p = after;
    }
    return 0;
}

/* Os `then`/`else`/`fi` do `if` de uma linha.
 *
 * JINV-083: exigem POSIÇÃO DE COMANDO. Sem isso, o `fi` que e ARGUMENTO
 * fechava o bloco antes da hora e o resto virava comando:
 *
 *     if true; then echo fi; fi
 *       bash -> fi        jash -> (vazio) + `fi: command not found`
 *
 * (o `echo fi` sozinho sempre funcionou — o estrago so aparecia dentro do
 * bloco, que e onde o texto e lido a procura do fechador.) */
static const char* find_keyword(const char* hay, const char* kw,
                                const char** after) {
    if (!kw) return 0;
    jash_kw_t one;
    one.word = kw;
    one.endset = JASH_KW_END_ANY;
    one.cmdpos = 1;                            /* JINV-083 */
    return jash_find_keyword_set(hay, &one, 1, 0, after);
}

/* Check if a line starts with a keyword (ignoring leading whitespace) */
static int starts_with_keyword(const char* line, const char* kw) {
    if (!line || !kw) return 0;
    while (*line == ' ' || *line == '\t') line++;
    size_t kw_len = strlen(kw);
    if (strncmp(line, kw, kw_len) != 0) return 0;
    /* Ensure word boundary (next char is space, tab, or end) */
    char next = line[kw_len];
    return !next || next == ' ' || next == '\t' || next == ';';
}

/* Trim leading/trailing whitespace */
static const char* trim_ws(const char* s) {
    if (!s) return s;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n')) s++;
    return s;
}

/* Check if line is a single command that succeeded/failed (for if condition) */
int jash_eval_if_condition(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* condition_line) {
    /* Evaluate the condition (a normal command line).
     * If it returns 0, the condition is TRUE; otherwise FALSE.
     * This matches POSIX sh semantics. */
    int ret = jash_eval_line(s, w, ctx, condition_line);
    return (ret == 0) ? 1 : 0;  /* 1 = true, 0 = false */
}

/* Parse and execute "if <cond> then <body> [else <else_body>] fi"
 * All on one line OR collected from multi-line input (handled by caller).
 * Returns exit code of the executed branch. */
int jash_eval_if_statement(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* if_line) {
    /* For now, only support single-line if:
     *   if [ -f /tmp/x ]; then echo found; fi
     * Future: multi-line with readline collecting until fi. */

    if (!starts_with_keyword(if_line, "if")) {
        return -1;  /* Not an if statement */
    }

    const char* p = trim_ws(if_line);
    p += 2;  /* skip "if" */
    p = trim_ws(p);

    /* Handle [[ ]] syntax — find matching ]] before looking for then */
    const char* then_search_start = p;
    if (p[0] == '[' && p[1] == '[') {
        /* Find matching ]] */
        int depth = 1;
        const char* sp = p + 2;
        while (*sp && depth > 0) {
            if (sp[0] == '[' && sp[1] == '[') { depth++; sp += 2; }
            else if (sp[0] == ']' && sp[1] == ']') { depth--; sp += 2; }
            else { sp++; }
        }
        /* Now search for then after ]] */
        then_search_start = sp;
    }

    /* Find "then" keyword (multi-line: \n before keyword).
     * JINV-072: por PALAVRA e consciente de aspas — a lista de separadores
     * (" then"/"\tthen"/"\nthen"/";then") era uma busca por SUBSTRING. */
    const char* after_then = 0;
    const char* then_pos = find_keyword(then_search_start, "then", &after_then);
    if (!then_pos) {
        /* BUG-022: syntax errors go to stderr (bash parity), not stdout. */
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: if requires 'then' keyword. Did you mean: if COND; then ...?%s\n",
                    J_ROLE_ERR, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    /* Extract condition (from after "if" to before "then") */
    size_t cond_len = then_pos - p;
    char cond_buf[256];
    if (cond_len >= sizeof(cond_buf)) cond_len = sizeof(cond_buf) - 1;
    memcpy(cond_buf, p, cond_len);
    cond_buf[cond_len] = 0;

    /* Trim condition.
     * JINV-072: a regiao agora termina na PALAVRA `then`, entao o separador
     * (` `, `\t`, `\n` ou `;`) ficou dentro dela — o `\n` tambem sai. O `;`
     * fica de proposito: `if true; then` sempre avaliou a condicao como
     * `true;` e o avaliador de linha ja trata isso. */
    size_t i = cond_len;
    while (i > 0 && (cond_buf[i-1] == ' ' || cond_buf[i-1] == '\t' ||
                     cond_buf[i-1] == '\n')) i--;
    cond_buf[i] = 0;

    /* Evaluate condition */
    int cond_true = jash_eval_if_condition(s, w, ctx, cond_buf);

    /* Parse "then" body and optional "else" body */
    p = trim_ws(after_then);   /* JINV-072: sem aritmetica de `+5` a adivinhar */

    /* Find "else" or "fi" — por PALAVRA e consciente de aspas (JINV-072).
     * A lista antiga (" else "/"\telse "/"\nelse " e " fi"/"\tfi"/"\nfi"/";fi")
     * era busca por SUBSTRING: `" fi"` casava dentro de `" first;"` e o
     * terminador do bloco caia no meio de uma palavra do corpo. */
    const char* after_else = 0;
    const char* else_pos = find_keyword(p, "else", &after_else);
    const char* after_fi = 0;
    const char* fi_pos = find_keyword(p, "fi", &after_fi);
    if (!fi_pos) {
        jash_writef(w, ctx, "%sjash: syntax error: unexpected end of if statement (missing fi)%s\n",
                    J_ROLE_ERR, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    /* Extract then-body */
    size_t then_len;
    const char* else_body = NULL;
    if (else_pos && else_pos < fi_pos) {
        then_len = else_pos - p;
        else_body = trim_ws(after_else);   /* JINV-072: sem `+6` a adivinhar */
    } else {
        then_len = fi_pos - p;
    }

    char then_buf[512];
    if (then_len >= sizeof(then_buf)) then_len = sizeof(then_buf) - 1;
    memcpy(then_buf, p, then_len);
    then_buf[then_len] = 0;

    /* Trim then-body (JINV-072: `\n` tambem, ver a condicao acima) */
    i = then_len;
    while (i > 0 && (then_buf[i-1] == ' ' || then_buf[i-1] == '\t' ||
                     then_buf[i-1] == '\n' || then_buf[i-1] == ';')) i--;
    then_buf[i] = 0;

    /* Extract else-body (if any) */
    char else_buf[512] = {0};
    if (else_body) {
        const char* else_end = fi_pos;
        size_t else_len = (else_end > else_body) ? (size_t)(else_end - else_body) : 0;
        if (else_len >= sizeof(else_buf)) else_len = sizeof(else_buf) - 1;
        memcpy(else_buf, else_body, else_len);
        else_buf[else_len] = 0;
        i = else_len;
        while (i > 0 && (else_buf[i-1] == ' ' || else_buf[i-1] == '\t' ||
                         else_buf[i-1] == '\n' || else_buf[i-1] == ';')) i--;
        else_buf[i] = 0;
    }

    /* JINV-072: `after_fi` ja aponta logo depois da PALAVRA `fi`. O codigo
     * antigo pulava o separador a mao e somava 2 — quando o `fi_pos` casado
     * era o `fi` de `first`, esse `+2` largava o ponteiro dentro da palavra e
     * `rst;` era executado como comando. */
    const char* rest = trim_ws(after_fi);
    if (*rest == ';') rest = trim_ws(rest + 1);

    /* Execute appropriate branch */
    int rc = 0;
    if (cond_true) {
        rc = jash_eval_line(s, w, ctx, then_buf);
    } else if (else_buf[0]) {
        rc = jash_eval_line(s, w, ctx, else_buf);
    }
    if (rc == 255 || rc == 254 || !*rest) return rc;
    return jash_eval_line(s, w, ctx, rest);
}
