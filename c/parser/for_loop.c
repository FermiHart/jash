// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/for_loop — for VAR in WORDS; do BODY; done support.

#include "script.h"
#include "eval.h"
#include "arithmetic.h"
#include "cmdsub.h"
#include "param_expand.h"
#include "expand.h"  /* jash_expand_braces */
#include "quotes.h"  /* JINV-116: scanner único, ciente de aspas */
#include <core/env.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <string.h>
#include <stdlib.h>   /* JINV-080: malloc/free do corpo sem teto */
#include <ctype.h>
#include <errno.h>    /* certify overflow check on the loop-bound literal */

extern int jash_readonly_is_set(const char* name);
/* eval.c: shared `(( expr ))` command shape check + evaluator (loop fast paths) */
extern int jash_arith_cmd_shape(const char* line, int interactive);
extern int jash_eval_arith_command(jash_state_t* s, jash_writer_t w, void* ctx,
                                   const char* line);

/* Forward declaration for fast path optimization */
static int fast_arith_assign(jash_state_t* s, jash_writer_t w, void* ctx,
                              const char* body, int* exit_code, int dry_run);

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\x1E') p++;
    return p;
}

/* Loop-condition fast path: a condition of the exact shape
 * `VAR OP NUMBER` (e.g. `i<2000`) is certified once, then evaluated per
 * iteration as one env read + one integer compare instead of a full
 * recursive-descent parse. Anything else (assignments, ++/--, commas,
 * hex/octal-looking bounds, extra operators) declines and the generic
 * evaluator runs — identical results, just slower.
 *
 * Equivalence notes: the variable side reads exactly like arith_var_read
 * (strtol base 0, unset/empty is 0); the bound is decimal-only with no
 * leading zero, so base-0 parsing cannot disagree with the arith parser;
 * truth is nonzero, as the loop below tests `cond == 0`. */
typedef struct { char var[64]; int op; long bound; } for_cond_fast_t;

static int for_cond_certify(const char* e2, for_cond_fast_t* out) {
    if (!e2 || !out) return 0;
    const char* p = e2;
    while (*p == ' ' || *p == '\t') p++;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          *p == '_')) return 0;
    size_t vn = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        if (vn + 1 >= sizeof(out->var)) return 0;
        out->var[vn++] = *p++;
    }
    out->var[vn] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    int op = -1;
    if (p[0] == '<' && p[1] == '=') { op = 1; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { op = 3; p += 2; }
    else if (p[0] == '=' && p[1] == '=') { op = 4; p += 2; }
    else if (p[0] == '!' && p[1] == '=') { op = 5; p += 2; }
    else if (p[0] == '<') { op = 0; p += 1; }
    else if (p[0] == '>') { op = 2; p += 1; }
    else return 0;
    out->op = op;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = (*p++ == '-');
    if (*p < '0' || *p > '9') return 0;
    const char* digits = p;
    while (*p >= '0' && *p <= '9') p++;
    /* Leading zeros would make base-0 parsing octal: decline and let the
     * generic evaluator own those literals. */
    if ((size_t)(p - digits) > 1 && digits[0] == '0') return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') return 0;
    /* At most 18 digits always fit in a long; longer runs might
     * overflow strtol, so decline and let the generic evaluator (with
     * its saturating arithmetic) own them. */
    if ((size_t)(p - digits) > 18) return 0;
    errno = 0;
    long bound = strtol(digits, NULL, 10);
    if (errno == ERANGE) return 0;
    out->bound = neg ? -bound : bound;
    return 1;
}

static long parse_long_str(const char* s) {
    if (!s || !*s) return 0;
    long num = 0;
    int neg = 0;
    const char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') {
        num = num * 10 + (*p - '0');
        p++;
    }
    return neg ? -num : num;
}

static void long_to_str(long val, char* buf, unsigned bcap) {
    if (bcap == 0) return;
    int neg = 0;
    unsigned long u;
    if (val < 0) { neg = 1; u = (unsigned long)(-val); }
    else u = (unsigned long)val;
    char tmp[32]; int t = 0;
    if (u == 0) tmp[t++] = '0';
    else { while (u > 0) { tmp[t++] = '0' + (u % 10); u /= 10; } }
    unsigned o = 0;
    if (neg && o < bcap - 1) buf[o++] = '-';
    while (t > 0 && o < bcap - 1) buf[o++] = tmp[--t];
    buf[o] = '\0';
}

/* Parse and execute: for (( expr1; expr2; expr3 )); do BODY; done */
int jash_eval_for_c_style(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* line) {
    const char* p = skip_ws(line);
    /* BUG-018: accept both `for ((` and `for((` (no space) — bash allows both. */
    if (strncmp(p, "for", 3) != 0 || !(isspace((unsigned char)p[3]) || p[3] == '(')) return -1;
    p = skip_ws(p + 3);

    /* Expect (( */
    if (p[0] != '(' || p[1] != '(') return -1;
    p = skip_ws(p + 2);

    /* Extract expr1; expr2; expr3 */
    const char* expr_start = p;
    int depth = 0;
    while (*p) {
        if (*p == '(') { depth++; p++; continue; }
        if (*p == ')') {
            if (depth == 0) break;  /* outer )) found */
            depth--;
            p++;
            continue;
        }
        p++;
    }
    /* Should be at )) */
    if (p[0] != ')' || p[1] != ')') return -1;

    /* Extract the three expressions */
    unsigned expr_len = (unsigned)(p - expr_start);

    /* JINV-100: no silent truncation in for ((expr)) */
    char expr_stack[1024];
    char* expr_buf = expr_len < sizeof(expr_stack) ? expr_stack : NULL;
    int expr_uses_heap = 0;
    if (!expr_buf) {
        expr_buf = malloc(expr_len + 1);
        if (!expr_buf) {
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: for((...)): sem memoria para expressao — recusado%s\n",
                        J_ROLE_ERR, J_RESET);
            return 1;
        }
        expr_uses_heap = 1;
    }
    for (unsigned i = 0; i < expr_len; i++) expr_buf[i] = expr_start[i];
    expr_buf[expr_len] = '\0';

    /* Split on ; to get init, cond, incr */
    char* e1 = expr_buf;
    char* e2 = NULL;
    char* e3 = NULL;
    int semi_count = 0;
    for (unsigned i = 0; i < expr_len; i++) {
        if (expr_buf[i] == ';') {
            expr_buf[i] = '\0';
            semi_count++;
            if (semi_count == 1) e2 = &expr_buf[i + 1];
            else if (semi_count == 2) e3 = &expr_buf[i + 1];
        }
    }
    if (semi_count < 2) return -1;

    /* Parse )) and find do */
    p = skip_ws(p + 2);
    /* Optional semicolon */
    if (*p == ';') p++;
    p = skip_ws(p);

    if (strncmp(p, "do", 2) != 0 || !isspace(p[2])) return 1;
    p = skip_ws(p + 2);

    const char* body_start = p;
    /* JINV-083 — o `done` que fecha, lido por PALAVRA e FORA DE ASPAS.
     *
     * Aqui morava uma das QUATRO copias da mesma varredura cega: ela casava
     * `for `/`while `/`until `/`done` em qualquer posicao do texto, inclusive
     * dentro de aspas (`echo "ask for help"` abria um laco fantasma), e nao
     * exigia fronteira a ESQUERDA de `done` (`echo undone` fechava o laco no
     * meio da palavra). Agora ha UMA leitura, em script.c. */
    const char* done_at = jash_find_loop_close(p);
    if (!done_at) return 1;
    p = done_at;

    size_t body_len = p - body_start;
    char body[2048];
    if (body_len >= sizeof(body)) body_len = sizeof(body) - 1;
    for (unsigned i = 0; i < body_len; i++) body[i] = body_start[i];
    body[body_len] = '\0';

    /* Execute init */
    if (e1 && *e1) {
        /* Handle assignment: var=expr */
        char* eq = strchr(e1, '=');
        if (eq) {
            *eq = '\0';
            const char* var = e1;
            while (*var == ' ' || *var == '\t') var++;
            const char* rhs = eq + 1;
            while (*rhs == ' ' || *rhs == '\t') rhs++;
            long val = jash_eval_arithmetic(rhs, s);
            char valbuf[32];
            long_to_str(val, valbuf, sizeof(valbuf));
            jash_env_set(s, var, valbuf);
        } else {
            jash_eval_arithmetic(e1, s);
        }
    }

    /* Loop */
    int exit_code = 0;
    /* FAST-PATH: a body that is a single `(( expr ))` command (certified by
     * jash_arith_cmd_shape — eval.c guarantees the skipped pre-phases of
     * jash_eval_line would be no-ops on this text) is re-evaluated through
     * the SAME function the normal dispatch reaches. The shape check is
     * side-effect-free; a declined call (-1, no side effects) falls back to
     * the full evaluator. */
    int body_is_arith = jash_arith_cmd_shape(body, s->interactive);
    /* Certified `VAR OP NUMBER` conditions skip the recursive-descent
     * parse per iteration (one env read + one compare instead). */
    for_cond_fast_t cond_fast;
    int cond_is_fast = for_cond_certify(e2 ? e2 : "", &cond_fast);
    for (;;) {
        /* Evaluate condition */
        if (e2 && *e2) {
            long cond;
            if (cond_is_fast) {
                const char* v = jash_env_get(s, cond_fast.var);
                long x = (v && *v) ? strtol(v, NULL, 0) : 0;
                switch (cond_fast.op) {
                    case 0: cond = (x < cond_fast.bound); break;
                    case 1: cond = (x <= cond_fast.bound); break;
                    case 2: cond = (x > cond_fast.bound); break;
                    case 3: cond = (x >= cond_fast.bound); break;
                    case 4: cond = (x == cond_fast.bound); break;
                    default: cond = (x != cond_fast.bound); break;
                }
            } else {
                cond = jash_eval_arithmetic(e2, s);
            }
            if (cond == 0) break;
        }

        /* Execute body */
        if (body_is_arith) {
            exit_code = jash_eval_arith_command(s, w, ctx, skip_ws(body));
            if (exit_code < 0)
                exit_code = jash_eval_line(s, w, ctx, body);
        } else {
            exit_code = jash_eval_line(s, w, ctx, body);
        }
        /* BUG-012: return/exit from inside the loop must STOP it and bubble up
         * (leave the flag set for the function boundary / driver to consume). */
        if (s->return_requested || s->exit_requested) break;
        /* Handle break (255) and continue (254) — BUG-166: N-level control
         * flow. Intermediate levels (>1) always fully exit this loop
         * (whether the original command was break or continue) and keep
         * propagating the unchanged sentinel + decremented level upward;
         * only the target (level<=1) loop performs the actual action.
         *
         * JINV-073: o teste exige agora o rc sentinela E a marca fora de banda
         * (state.h). Antes, um `(exit 255)` no corpo do laco era lido como
         * `break` — um codigo de saida legitimo de processo virando instrucao
         * de fluxo. */
        if (jash_loopctl_is_ctl(s, exit_code)) {
            if (s->loop_break_level > 1) { s->loop_break_level--; break; }
            s->loop_break_level = 0;
            int was_break = jash_loopctl_is_break(s, exit_code);
            jash_loopctl_clear(s);      /* JINV-073: consumido aqui */
            exit_code = 0;
            if (was_break) break;
            continue;
        }

        /* Execute increment */
        if (e3 && *e3) {
            /* Handle var++, var--, var+=N etc */
            char varname[64] = {0};
            const char* ep = e3;
            while (*ep == ' ' || *ep == '\t') ep++;
            /* Extract variable name */
            unsigned vi = 0;
            while (*ep && (isalnum(*ep) || *ep == '_') && vi < sizeof(varname) - 1)
                varname[vi++] = *ep++;
            varname[vi] = '\0';

            if (vi > 0) {
                /* Check for ++ / -- / +=N / -=N */
                if (ep[0] == '+' && ep[1] == '+') {
                    /* var++ */
                    const char* cur = jash_env_get(s, varname);
                    long val = cur ? parse_long_str(cur) : 0;
                    val++;
                    char vb[32]; long_to_str(val, vb, sizeof(vb));
                    jash_env_set(s, varname, vb);
                } else if (ep[0] == '-' && ep[1] == '-') {
                    /* var-- */
                    const char* cur = jash_env_get(s, varname);
                    long val = cur ? parse_long_str(cur) : 0;
                    val--;
                    char vb[32]; long_to_str(val, vb, sizeof(vb));
                    jash_env_set(s, varname, vb);
                } else if ((ep[0] == '+' && ep[1] == '=')) {
                    const char* cur = jash_env_get(s, varname);
                    long val = cur ? parse_long_str(cur) : 0;
                    val += jash_eval_arithmetic(ep + 2, s);
                    char vb[32]; long_to_str(val, vb, sizeof(vb));
                    jash_env_set(s, varname, vb);
                } else if ((ep[0] == '-' && ep[1] == '=')) {
                    const char* cur = jash_env_get(s, varname);
                    long val = cur ? parse_long_str(cur) : 0;
                    val -= jash_eval_arithmetic(ep + 2, s);
                    char vb[32]; long_to_str(val, vb, sizeof(vb));
                    jash_env_set(s, varname, vb);
                } else {
                    jash_eval_arithmetic(e3, s);
                }
            } else {
                jash_eval_arithmetic(e3, s);
            }
        }
    }

    /* BUG-018: run commands after `done` for the C-style loop too — e.g.
     * `for((i=0;i<N;i++)); do :; done && echo done`. Mirrors the list-form
     * for-loop (jash_eval_for_loop). `p` still points at "done" here. */
    if (s->return_requested || s->exit_requested) { if (expr_uses_heap) free(expr_buf); return exit_code; }
    /* JINV-075: um break/continue de nivel > 1 esta subindo — a cauda depois
     * do `done` NAO pode rodar, e o `return rest_rc` abaixo destruiria a
     * sentinela. Mesma guarda do irmao if_block.c:501. */
    if (jash_loopctl_pending(s)) { if (expr_uses_heap) free(expr_buf); return exit_code; }
    int for_loop_result = exit_code;
    if (*p) {
        p += 4; /* skip "done" */
        p = skip_ws(p);
        if (*p == ';' || *p == '\n') { p++; p = skip_ws(p); }
        if (*p) {
            for_loop_result = jash_eval_line(s, w, ctx, p);
        }
    }

    if (expr_uses_heap) free(expr_buf);
    return for_loop_result;
}

/* ═══ JINV-087 — a varredura de PALAVRAS da lista do `for` ════════════════
 *
 * Recebe o texto JÁ EXPANDIDO, ainda com a marca JASH_EXP_MARK nas palavras
 * que passaram por expansão NÃO CITADA, e devolve o mesmo texto com:
 *
 *   - os caracteres de IFS não-brancos LITERAIS dessas palavras (e só delas,
 *     e só fora de aspas e de barra invertida) trocados por JASH_FIELD_SEP;
 *   - toda marca APAGADA.
 *
 * Devolve 0 quando não havia marca nenhuma — aí o chamador nem troca de
 * buffer, e o caminho comum (IFS branco) não paga nada.
 *
 * A leitura de `$IFS` não é feita aqui: `jash_ifs_is_hard_sep` responde pelo
 * mapa que `jash_ifs_field_begin` já carregou. Uma leitura só, como manda o
 * cabeçalho de script.h — este projeto já achou oito pares "duas
 * implementações da mesma coisa, uma empobrecida". */
static int jash_ifs_word_split(const char* in, char* out, size_t cap) {
    if (!in || !out) return 0;
    if (!strchr(in, JASH_EXP_MARK)) return 0;

    size_t o = 0;
    /* Passo 1: descobrir, por PALAVRA, se ela tem marca. A palavra termina em
     * branco FORA de aspas — o branco dentro de aspas ("a b") é dado e não
     * fecha palavra. */
    const char* p = in;
    while (*p && o + 1 < cap) {
        /* fronteira: copia o branco de separação como está */
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\x1E') {
            out[o++] = *p++;
            continue;
        }
        /* delimita a palavra corrente, com estado de aspas */
        const char* wstart = p;
        int sq = 0, dq = 0, marked = 0;
        const char* q = p;
        while (*q) {
            char c = *q;
            if (c == '\\' && !sq && q[1]) { q += 2; continue; }
            if (c == '\'' && !dq) { sq = !sq; q++; continue; }
            if (c == '"'  && !sq) { dq = !dq; q++; continue; }
            if (!sq && !dq && (c == ' ' || c == '\t' || c == '\n' || c == '\x1E')) break;
            if (c == JASH_EXP_MARK) marked = 1;
            q++;
        }
        /* Passo 2: reemite a palavra. Só a palavra MARCADA tem os separadores
         * literais promovidos — e, dentro dela, só o que está fora de aspas e
         * não foi escapado. É a diferença entre `$P:x` (corta) e `$P'a:b'`
         * (não corta), que o bash faz e a divisão cega não fazia. */
        sq = 0; dq = 0;
        for (const char* r = wstart; r < q && o + 1 < cap; r++) {
            char c = *r;
            if (c == JASH_EXP_MARK) continue;               /* a marca some aqui */
            if (c == '\\' && !sq && r[1]) {
                out[o++] = c;
                if (o + 1 < cap) out[o++] = *++r;
                continue;
            }
            if (c == '\'' && !dq) { sq = !sq; out[o++] = c; continue; }
            if (c == '"'  && !sq) { dq = !dq; out[o++] = c; continue; }
            if (marked && !sq && !dq && jash_ifs_is_hard_sep(c)) {
                out[o++] = JASH_FIELD_SEP;
                continue;
            }
            out[o++] = c;
        }
        p = q;
    }
    out[o] = 0;
    return 1;
}

/* Parse and execute: for VAR in WORDS; do BODY; done */
int jash_eval_for_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* line) {

    const char* p = skip_ws(line);
    if (strncmp(p, "for", 3) != 0 || !isspace(p[3])) return -1;

    p = skip_ws(p + 3);
    const char* var_start = p;
    /* JINV-078: o nome da variável termina no `;` também. Sem isto,
     * `for x; do …` produzia varname="x;" e a mensagem de erro saía
     * "for x; in ..." — o parser nem conseguia NOMEAR o que tinha lido. */
    while (*p && !isspace((unsigned char)*p) && *p != ';') p++;
    size_t var_len = p - var_start;
    if (var_len == 0 || var_len > 63) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: for requires a variable name. Did you mean: for i in ...; do ...; done?%s\n",
                    J_ROLE_ERR, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    char varname[64];
    memcpy(varname, var_start, var_len);
    varname[var_len] = 0;

    p = skip_ws(p);

    char words_inline[65536];
    char* words = words_inline;
    char* words_heap = NULL;

    /* ═══ JINV-078 — `for VAR; do … done` (sem `in`) ═══════════════════════
     *
     * POSIX (XCU 2.9.4.2): omitir a lista `in …` é EQUIVALENTE a
     * `for VAR in "$@"` — o laço percorre os parâmetros posicionais. É como
     * toda função de shell itera sobre os próprios argumentos, e autoconf
     * usa isto o tempo todo. O jash devolvia
     * `for requires 'in' keyword` e, num script (não-interativo), pedia
     * `exit_requested` — o arquivo INTEIRO morria na primeira ocorrência.
     *
     * Este gatilho NÃO estava na auditoria original: ele só apareceu ao
     * medir de novo DEPOIS que o `export: usage` deixou de ser o primeiro
     * erro. Um bloqueador dominante esconde o próximo — é a razão de a
     * medição ter que ser refeita a cada onda, não só no fim.
     *
     * A implementação delega ao caminho que já existe e já está coberto:
     * a lista de palavras vira o texto literal `"$@"`. Não é um atalho — é
     * literalmente o que o POSIX define, e garante que as duas formas não
     * possam divergir no futuro (aspas, elementos com espaço, lista vazia).
     *
     * Formas aceitas: `for x; do`, `for x do`, `for x` + newline + `do`,
     * e `for x;do` (sem espaço). Se o que vem depois do nome não for `do`,
     * o erro `requires 'in'` de antes continua valendo — um `for x y` segue
     * sendo erro de sintaxe, como no bash.
     *
     * Gate: tests/test_jinv078_for_positional.sh */
    int positional_form = 0;
    {
        const char* q = p;
        if (*q == ';') q = skip_ws(q + 1);
        if (strncmp(q, "do", 2) == 0 &&
            (isspace((unsigned char)q[2]) || q[2] == ';' || q[2] == '\0'))
            positional_form = 1;
    }

    if (positional_form) {
        memcpy(words, "\"$@\"", 5);       /* JINV-078 */
        if (*p == ';') p++;
        p = skip_ws(p);
    } else {
    /* Accept `in` followed by whitespace, ';' (empty word list — bash:
     * `for i in; do ...; done` is legal and the body never runs), or
     * end-of-string. Without these alternates the parser rejects the
     * valid empty-list idiom. */
    if (strncmp(p, "in", 2) != 0 ||
        !(isspace(p[2]) || p[2] == ';' || p[2] == '\0')) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: for requires 'in' keyword. Did you mean: for %s in ...; do ...; done?%s\n",
                    J_ROLE_ERR, varname, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    p = skip_ws(p + 2);
    const char* words_start = p;
    /* BUG-059/bash parity: the word list ends at ';' OR at a newline —
     * `for i in 1 2<newline>do ...<newline>done` is the canonical multi-line
     * form every agent harness emits. skip_ws() below already eats the
     * newline(s) before `do`.
     * JINV-116: a lista de palavras é texto de shell, então o `;` só separa se
     * estiver fora de aspas — `for f in "a;b" c; do` mantinha o `;` citado como
     * fim da lista e perdia o resto do comando. */
    p = jash_scan_unquoted(p, ";\n");
    if (!*p) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: unexpected end of for statement (missing ;). Did you mean: for %s in ...; do ...; done?%s\n",
                    J_ROLE_ERR, varname, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    size_t words_len = p - words_start;
    if (words_len >= sizeof(words_inline)) {
        words_heap = (char*)malloc(words_len + 1);
        if (!words_heap) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: for: sem memoria para lista literal — recusando\n");
            return 2;
        }
        words = words_heap;
    }
    memcpy(words, words_start, words_len);
    words[words_len] = 0;

    p = skip_ws(p + 1);
    }   /* JINV-078: fim do ramo com `in` */

    if (strncmp(p, "do", 2) != 0 || !isspace(p[2])) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: for requires 'do' keyword. Did you mean: for %s in ...; do ...; done?%s\n",
                    J_ROLE_ERR, varname, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; free(words_heap); return 2; }
    }

    p = skip_ws(p + 2);
    const char* body_start = p;
    /* JINV-083 — o `done` que fecha, lido por PALAVRA e FORA DE ASPAS.
     *
     * ESTE e o site que os 19 `configure` batiam. A varredura de antes casava
     * `for `/`while `/`until `/`done` em qualquer posicao do texto, inclusive
     * DENTRO DE ASPAS:
     *
     *     for i in 1; do echo "ask for help"; done
     *       bash -> ask for help      jash -> syntax error: missing done
     *
     * O `for ` citado subia a profundidade e o `done` que o autor escreveu
     * deixava de fechar o laco. Dois gatilhos reais medidos: o
     * `echo "$0 --help for help"` do `configure` do zlib, e o padrao de `case`
     * do autoconf `| --program-transfor \` (termina em `for `). Faltava
     * tambem a fronteira a ESQUERDA de `done` — `undone` fechava o laco.
     *
     * Uma leitura so, em script.c: `jash_find_loop_close`. */
    const char* done_at = jash_find_loop_close(p);
    if (!done_at) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: unexpected end of for statement (missing done). Did you mean: for %s in ...; do ...; done?%s\n",
                    J_ROLE_ERR, varname, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; free(words_heap); return 2; }
    }
    p = done_at;

    /* ═══ JINV-080 — o corpo do laço não pode ser truncado em silêncio ═════
     *
     * `char body[1024]` com clamp mudo: um corpo de laço com mais de 1023
     * bytes tinha a CAUDA APAGADA e o laço rodava assim mesmo, com rc 0.
     * Nada no stderr, nada no rc — o programa executado passava a ser outro,
     * e quem chamou não tinha como saber. É a "morte silenciosa" que a
     * auditoria nomeia, dentro do próprio arquivo deste conserto.
     *
     * O corte cai em 1024 bytes de CORPO — ~15 linhas de comando reais. Todo
     * `for ac_option / do … done` de autoconf tem ~7 KB; todo laço de
     * verdade passa disso.
     *
     * Achado ao MEDIR os 19 `configure` depois do JINV-077/078: com o
     * `export` e o `for` nus consertados, o erro seguinte deixou de ser de
     * sintaxe e virou ausência de saída. Um bloqueador dominante esconde o
     * próximo.
     *
     * Conserto: sem teto. Corpo até 1023 bytes segue na pilha (caso comum,
     * zero malloc); acima disso, heap do tamanho exato. Falta de memória é
     * recusa ALTA — o laço não roda — e nunca truncamento.
     *
     * Gate: tests/test_jinv079_loop_body_size.sh */
    size_t body_len = p - body_start;
    char  body_inline[1024];
    char* body_heap = NULL;
    char* body = body_inline;
    if (body_len >= sizeof(body_inline)) {
        body_heap = (char*)malloc(body_len + 1);
        if (!body_heap) {
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: for: sem memoria para o corpo do laco — o laco NAO foi executado%s\n",
                        J_ROLE_ERR, J_RESET);
            { if (s && !s->interactive) s->exit_requested = 1; free(words_heap); return 2; }
        }
        body = body_heap;
    }
    memcpy(body, body_start, body_len);
    body[body_len] = 0;

    /* Split words and iterate */
    int exit_code = 0;
    int ret_code  = 0;   /* JINV-080: uma só porta de saída, para liberar o heap */

    /* Expand the word list first: cmdsub -> arithmetic -> param/array
     * -> brace. This makes `for x in ${arr[@]}`, `for f in $(ls)`,
     * `for i in $LIST` AND `for i in {1..3}` / `for x in a{1,2,3}b`
     * behave like bash (word-split on the expansion result). */
    const char* expanded = words;
    jash_expand_ctx_t ectx;   /* ADR-0036 W4: scoped to cover every use of `expanded` below */
    char* word_copy = NULL;
    size_t word_cap = 0;
    /* JINV-087: carrega $IFS UMA vez para toda a sequência de expansão, e
     * liga a marca de palavra (JASH_EXP_MARK). Com IFS branco (o padrão)
     * `jash_ifs_word_mark_wanted()` já responde 0 e nenhum produtor marca
     * nada — o texto expandido sai idêntico ao de antes deste conserto. */
    jash_ifs_field_begin(s, 1);
    jash_ifs_word_mark_enable(1);
    if (jash_has_cmdsub(expanded)) {
        /* JINV-087: a divisão por IFS acontece DENTRO da expansão, onde ainda
         * se sabe se o `$(…)` estava entre aspas. Ligado só em volta desta
         * chamada; restaurado logo abaixo. */
        extern int g_jash_cmdsub_field_split;
        int fld_saved = g_jash_cmdsub_field_split;
        g_jash_cmdsub_field_split = 1;
        const char* x = jash_expand_cmdsub_known(s, expanded);
        g_jash_cmdsub_field_split = fld_saved;
        if (x) expanded = x;
        if (jash_cmdsub_error()) {
            jash_writes(jash_stderr_writer, 0, jash_cmdsub_error_msg());
            jash_writes(jash_stderr_writer, 0, "\n");
            ret_code = 1;
            goto for_body_done;
        }
        /* JINV-087: o comando de dentro do `$( )` pode ter sido outro `for` —
         * e ele reentra nestes mesmos globais e pode até ter MUDADO o $IFS.
         * Rearma antes de seguir, em vez de confiar em estado que outra
         * avaliação inteira teve a chance de mexer. */
        jash_ifs_field_begin(s, 1);
        jash_ifs_word_mark_enable(1);
    }
    if (jash_has_arithmetic(expanded)) {
        jash_arith_clear_error();
        const char* x = jash_expand_arithmetic(expanded, s);
        if (x) expanded = x;
        if (jash_arith_error()) {
            jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
            ret_code = 1;
            goto for_body_done;
        }
    }
    if (jash_has_param_expansion(expanded)) {
        /* JINV-087: variante de CAMPOS — só o resultado de expansão NÃO
         * CITADA sai marcado. Antes aqui vinha jash_param_expand (que não
         * divide nada) e a divisão era feita depois, na cadeia inteira e às
         * cegas: literal e citado eram cortados junto. */
        const char* x = jash_param_expand_split_fields(expanded, s);
        if (x) expanded = x;
        if (jash_param_expand_error()) {
            jash_writes(jash_stderr_writer, 0, jash_param_expand_error_msg());
            jash_writes(jash_stderr_writer, 0, "\n");
            ret_code = 1;
            goto for_body_done;
        }
    }
    jash_ifs_word_mark_enable(0);

    /* ═══ JINV-087 — a PALAVRA que passou por expansão ════════════════════
     *
     * O oráculo divide por palavra: se a palavra contém uma expansão NÃO
     * CITADA, os caracteres de IFS LITERAIS dela também cortam.
     *
     *   IFS=:; for d in a:b     → [a:b]      nenhuma expansão: nada corta
     *   IFS=:; for d in $P:x    → [0] [x]    o `:` literal corta
     *   IFS=:; for d in "$P":x  → [0:x]      a expansão era citada
     *   IFS=:; for d in $P'a:b' → [0a:b]     o `:` está em aspas
     *
     * As duas informações moram em lugares diferentes e é por isso que o
     * conserto tem duas metades: o produtor sabe se HOUVE expansão citada
     * (emite ou não a marca); só aqui se sabe onde a palavra começa e
     * termina, e quais bytes dela estão dentro de aspas.
     *
     * Roda ANTES de chave/glob de propósito: a marca é apagada aqui, e um
     * byte estranho dentro de um padrão de glob não casaria arquivo nenhum.
     *
     * Escreve em `word_copy` (declarado logo acima, e reusado no fim como
     * buffer do tokenizador) para não somar mais 64 KB de pilha a um quadro
     * que já é grande e que RECURSA em laço aninhado. */
    word_cap = strlen(expanded) + 1;
    word_copy = (char*)malloc(word_cap);
    if (!word_copy) {
        jash_writes(jash_stderr_writer, 0,
                    "jash: for: sem memoria para lista de palavras — recusando\n");
        ret_code = 1;
        goto for_body_done;
    }
    if (jash_ifs_word_split(expanded, word_copy, word_cap))
        expanded = word_copy;

    /* Brace expansion runs after $A/$B become literals so `{$A..$B}` works.
     * Without it, `for i in {1..3}` would iterate ONCE over the literal
     * token `{1..3}` and the body would echo nothing. */
    {
        const char* x = jash_expand_braces(expanded, &ectx);
        if (x) expanded = x;
    }
    /* BUG-108: pathname expansion — `for f in *.txt` must iterate the
     * matching files (jash already globbed in command position; word lists
     * were skipped). BUG-167: honor shopt nullglob instead of hardcoding
     * "unmatched patterns stay literal". */
    if (!s->shell_opts.noglob) {
        ectx.nullglob = s->shell_opts.nullglob;
        const char* x = jash_expand_glob(expanded, &ectx);
        if (jash_glob_error()) {
            jash_writes(jash_stderr_writer, 0, jash_glob_error_msg());
            jash_writes(jash_stderr_writer, 0, "\n");
            ret_code = 1;
            goto for_body_done;
        }
        if (x) expanded = x;
    }

    /* ═══ JINV-087 — o DADO CITADO deixou de ser dividido ═════════════════
     *
     * Aqui morava a divisão CEGA: um mapa de `$IFS` aplicado à cadeia INTEIRA,
     * depois de toda a expansão. Nesse ponto não existe mais nenhuma diferença
     * entre um `:` que veio de `$P` sem aspas (separador, POSIX manda cortar)
     * e um `:` que o autor escreveu na mão ou dentro de aspas (dado, POSIX
     * PROÍBE cortar). O mapa cortava os três:
     *
     *   IFS=:; for d in /bin:/usr/bin        bash: 1 item · jash: 2   ← literal
     *   IFS=:; for d in "$P"                 bash: 1 item · jash: 2   ← citado
     *   IFS=:; for d in $P                   bash: 2 itens · jash: 2  ← já certo
     *
     * É EXATAMENTE o idioma `as_dir` de varredura de PATH do autoconf, que
     * está nos 19 `configure` desta máquina.
     *
     * A marca "veio de expansão NÃO CITADA" não estava perdida: ela existe no
     * ponto da expansão, e é lá que a divisão passou a ser feita
     * (jash_param_expand_split_fields / g_jash_cmdsub_field_split, acima). O
     * que chega aqui já vem com JASH_FIELD_SEP nos cortes legítimos e com o
     * branco-que-é-dado blindado. Texto literal e resultado citado chegam
     * intactos — e é essa ausência de marca que é o conserto.
     *
     * Mesma família de JINV-016 (argv[0] injetado), JINV-020 (separador em
     * aspas aninhadas), JINV-028 (`>` perdido), JINV-032 (`;;` em aspas) e
     * JINV-083 (palavra-chave em aspas): dado virando estrutura porque a
     * informação de citação foi descartada antes do ponto que precisava dela.
     * O conserto nunca é um filtro depois — é preservar a marca até lá.
     *
     * Gate: tests/test_jinv087_ifs_quoted.sh */
    {
        size_t need = strlen(expanded) + 1;
        if (need > word_cap) {
            char* grown = (char*)realloc(word_copy, need);
            if (!grown) {
                jash_writes(jash_stderr_writer, 0,
                            "jash: for: sem memoria para lista expandida — recusando\n");
                ret_code = 1;
                goto for_body_done;
            }
            word_copy = grown;
            word_cap = need;
        }
        memcpy(word_copy, expanded, need);
    }

    /* LOOP UNROLLING OPTIMIZATION: Pre-tokenize all words into a fixed array.
     * For small constant lists (<=64 items), this eliminates incremental
     * parsing overhead during iteration — effectively "unrolling" the
     * token-discovery step. For larger lists, fall back to streaming parse. */
    #define JASH_FOR_UNROLL_MAX 256
    char unroll_tokens[JASH_FOR_UNROLL_MAX][256];
    int unroll_count = 0;
    int use_unroll = 1;

    {
        const char* wp = word_copy;
        /* BUG-094: quote state PERSISTS across \x1E element boundaries —
         * "${a[@]}" elements share one quote pair. Resetting per token
         * re-split multi-word elements and dropped empty ones. */
        char quote = 0;
        while (*wp && unroll_count < JASH_FOR_UNROLL_MAX) {
            if (!quote) wp = skip_ws(wp);
            if (!*wp) break;

            char token[256];
            size_t token_len = 0;
            /* BUG-117a: a QUOTED empty token ("" or an empty "$@" element) is
             * a real word — track whether this token saw quoting. */
            int tok_had_quote = (quote != 0);
            while (*wp && token_len + 1 < sizeof(token)) {
                char c = *wp;
                if (c == '\x1E') break;  /* Array word boundary (RS) — splits even inside "..." */
                if (c == JASH_FIELD_SEP) break;  /* JINV-087: fronteira de campo do IFS */
                if (quote) {
                    if (c == quote) { quote = 0; wp++; continue; }
                    /* BUG-116b: \" inside "..." is a literal quote (bash dq
                     * escapes) — without this the escaped quote CLOSED the
                     * quote and mangled the rest ("x\"y" tail collapsed). */
                    if (quote == '"' && c == '\\' && wp[1]) {
                        wp++; token[token_len++] = *wp++; continue;
                    }
                } else {
                    if (isspace((unsigned char)c)) break;
                    if (c == '"' || c == '\'') { quote = c; tok_had_quote = 1; wp++; continue; }
                }
                if (c == '\\' && !quote && wp[1]) { wp++; c = *wp; }
                token[token_len++] = c;
                wp++;
            }
            if (*wp && token_len + 1 == sizeof(token) &&
                *wp != '\x1E' && *wp != JASH_FIELD_SEP &&
                (quote || !isspace((unsigned char)*wp))) {
                use_unroll = 0;
                break;
            }
            token[token_len] = 0;
            jash_quote_unshield(token);   /* BUG-096 */
            /* ═══ JINV-087 — o campo VAZIO é um campo ═══════════════════════
             * Um caractere de IFS não-branco é delimitador EXATO: `a::b` com
             * IFS=':' são TRÊS campos, o do meio vazio (é assim que o `as_dir`
             * do autoconf lê `PATH=/bin::/usr/bin` como "o diretório corrente
             * está no meio"). Só o campo vazio FINAL — o que vem depois de um
             * delimitador que fecha a cadeia — não existe: `a:` é um campo. */
            int fs_boundary = (*wp == JASH_FIELD_SEP);
            if (fs_boundary) {
                wp++;
                memcpy(unroll_tokens[unroll_count], token, token_len + 1);
                unroll_count++;
                continue;
            }
            int rs_boundary = (*wp == '\x1E');
            if (rs_boundary && quote) {
                /* quoted empty element (consecutive \x1E) is a real word */
                if (token_len == 0) token[0] = 0;
                memcpy(unroll_tokens[unroll_count], token, token_len + 1);
                unroll_count++;
                wp++;
                continue;
            }
            if (rs_boundary) wp++;

            if (token_len > 0 || tok_had_quote) {
                memcpy(unroll_tokens[unroll_count], token, token_len + 1);
                unroll_count++;
            }
        }
        /* If there are remaining words beyond our limit, disable unrolling */
        if (*wp) {
            wp = skip_ws(wp);
            if (*wp) use_unroll = 0;
        }
    }

    if (use_unroll && unroll_count > 0) {
        /* Same simple-arithmetic body probe as the streaming path below
         * (BUG-111: dry_run=1 — the probe must not execute the assignment). */
        int use_fast_arith = 0;
        int dummy_exit = 0;
        if (fast_arith_assign(s, w, ctx, body, &dummy_exit, 1))
            use_fast_arith = 1;
        for (int ti = 0; ti < unroll_count; ti++) {
            /* Set loop var */
            jash_env_set(s, varname, unroll_tokens[ti]);

            /* Execute body */
            if (use_fast_arith) {
                /* Same rule as the streaming path: if the real call bails,
                 * fall back to the full evaluator. */
                if (!fast_arith_assign(s, w, ctx, body, &exit_code, 0))
                    exit_code = jash_eval_line(s, w, ctx, body);
            } else {
                exit_code = jash_eval_line(s, w, ctx, body);
            }
            /* BUG-012: return/exit bubbles up, stops the loop. */
            if (s->return_requested || s->exit_requested) break;
            /* Handle break (255) and continue (254) — BUG-166: N-level.
             * JINV-073: rc sentinela + marca fora de banda (state.h). */
            if (jash_loopctl_is_ctl(s, exit_code)) {
                if (s->loop_break_level > 1) { s->loop_break_level--; break; }
                s->loop_break_level = 0;
                int was_break = jash_loopctl_is_break(s, exit_code);
                jash_loopctl_clear(s);      /* JINV-073: consumido aqui */
                exit_code = 0;
                if (was_break) break;
                continue;
            }
            if (exit_code < 0) break;
        }
    } else {
        /* SLOW PATH: streaming tokenization for large lists */
        size_t token_cap = strlen(word_copy) + 1;
        char* token = (char*)malloc(token_cap);
        if (!token) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: for: sem memoria para token — recusando\n");
            ret_code = 1;
            goto for_body_done;
        }
        const char* wp = word_copy;
        
        /* OPTIMIZATION: Check if body is simple arithmetic assignment.
         * BUG-111: dry_run=1 — the probe must not execute the assignment. */
        int use_fast_arith = 0;
        int dummy_exit = 0;
        if (fast_arith_assign(s, w, ctx, body, &dummy_exit, 1)) {
            use_fast_arith = 1;
        }
        
        /* BUG-094: quote state persists across \x1E (see unroll path). */
        char quote = 0;
        while (*wp) {
            if (!quote) wp = skip_ws(wp);
            if (!*wp) break;

            size_t token_len = 0;
            int tok_had_quote = (quote != 0);   /* BUG-117a */
            while (*wp && token_len + 1 < token_cap) {
                char c = *wp;
                if (c == '\x1E') break;  /* array word boundary — splits even inside "..." */
                if (c == JASH_FIELD_SEP) break;  /* JINV-087: fronteira de campo do IFS */
                if (quote) {
                    if (c == quote) { quote = 0; wp++; continue; }
                    /* BUG-116b: \" inside "..." is a literal quote */
                    if (quote == '"' && c == '\\' && wp[1]) {
                        wp++; token[token_len++] = *wp++; continue;
                    }
                } else {
                    if (isspace((unsigned char)c)) break;
                    if (c == '"' || c == '\'') { quote = c; tok_had_quote = 1; wp++; continue; }
                }
                if (c == '\\' && !quote && wp[1]) { wp++; c = *wp; }
                token[token_len++] = c;
                wp++;
            }
            token[token_len] = 0;
            jash_quote_unshield(token);   /* BUG-096 */
            /* JINV-087: mesma regra do caminho desenrolado — campo vazio entre
             * dois delimitadores exatos é um campo. */
            int fs_boundary = (*wp == JASH_FIELD_SEP);
            if (fs_boundary) wp++;
            int rs_boundary = (*wp == '\x1E');
            int keep_empty = (rs_boundary && quote) || tok_had_quote || fs_boundary;
            if (rs_boundary) wp++;

            if (token_len > 0 || keep_empty) {
                jash_env_set(s, varname, token);

                if (use_fast_arith) {
                    /* BUG-111: dry-run detection no longer pre-screens the
                     * evaluation, so fall back to the full evaluator if the
                     * real call bails (e.g. empty arithmetic result). */
                    if (!fast_arith_assign(s, w, ctx, body, &exit_code, 0))
                        exit_code = jash_eval_line(s, w, ctx, body);
                } else {
                    exit_code = jash_eval_line(s, w, ctx, body);
                }

                /* BUG-012: return/exit bubbles up, stops the loop. */
                if (s->return_requested || s->exit_requested) break;
                /* BUG-166: N-level break/continue.
                 * JINV-073: rc sentinela + marca fora de banda (state.h). */
                if (jash_loopctl_is_ctl(s, exit_code)) {
                    if (s->loop_break_level > 1) { s->loop_break_level--; break; }
                    s->loop_break_level = 0;
                    int was_break = jash_loopctl_is_break(s, exit_code);
                    jash_loopctl_clear(s);  /* JINV-073: consumido aqui */
                    exit_code = 0;
                    if (was_break) break;
                    continue;
                }
                if (exit_code < 0) break;
            } else {
                /* Empty token — stop (trailing whitespace) */
                break;
            }
        }
        free(token);
    }

    /* BUG-012: if a return/exit fired inside the loop, do NOT run the text
     * after `done` — bubble straight up so the function/driver consumes it.
     * JINV-080: daqui para baixo toda saída passa por `for_body_done`, senão
     * o corpo em heap vaza. */
    if (s->return_requested || s->exit_requested) { ret_code = exit_code; goto for_body_done; }

    /* ═══ JINV-075 — a cauda depois do `done` respeita break/continue ═════
     *
     * O irmao if_block.c:501 ja tinha esta guarda; este sitio e o do
     * `case` (eval.c) nao tinham. Mesma disciplina aplicada num lugar e
     * esquecida nos outros dois. Sem ela:
     *
     *   for i in 1 2; do for j in a b; do break 2; done; echo never; done
     *
     * o `break 2` sai do laco interno, o `echo never` DEPOIS do `done`
     * interno executa (imprime), e o rc 0 dele substitui a sentinela 255 no
     * `return rest_rc` logo abaixo — o pedido de sair do laco externo e
     * APAGADO e o externo segue rodando. Saida: `never never done`;
     * o bash imprime so `done`.
     *
     * Gate: tests/test_jinv075_loop_tail_break.sh */
    if (jash_loopctl_pending(s)) { ret_code = exit_code; goto for_body_done; }

    /* Check if there are commands after "done" */
    if (*p) {
        p += 4; /* skip "done" */
        p = skip_ws(p);
        /* Skip optional semicolon */
        if (*p == ';' || *p == '\n') {
            p++;
            p = skip_ws(p);
        }
        /* If there are more commands, execute them */
        if (*p) {
            /* JINV-080: a cauda pode ser um laço aninhado que também aloca —
             * libera ANTES de recursar, não depois. */
            int rest_rc;
            if (body_heap) { free(body_heap); body_heap = NULL; }
            free(word_copy); word_copy = NULL;
            free(words_heap); words_heap = NULL;
            rest_rc = jash_eval_line(s, w, ctx, p);
            return rest_rc;
        }
    }

    ret_code = exit_code;

for_body_done:                       /* JINV-080: porta única de saída */
    free(word_copy);
    free(words_heap);
    if (body_heap) free(body_heap);
    return ret_code;
}

/* OPTIMIZATION: Fast path for simple arithmetic assignment in loop body
 * Detects pattern: VAR=$((EXPR)) and evaluates directly without full parsing
 * Returns 1 if optimized, 0 if not applicable */
static int fast_arith_assign(jash_state_t* s, jash_writer_t w, void* ctx,
                              const char* body, int* exit_code, int dry_run) {
    /* Skip leading whitespace */
    const char* p = body;
    while (*p && isspace(*p)) p++;
    
    /* Check for VAR= pattern */
    const char* var_start = p;
    while (*p && *p != '=' && !isspace(*p)) p++;
    if (*p != '=') return 0;
    
    size_t var_len = p - var_start;
    if (var_len == 0 || var_len > 64) return 0;
    if (!((var_start[0] >= 'a' && var_start[0] <= 'z') ||
          (var_start[0] >= 'A' && var_start[0] <= 'Z') ||
          var_start[0] == '_')) return 0;
    for (size_t i = 1; i < var_len; i++)
        if (!((var_start[i] >= 'a' && var_start[i] <= 'z') ||
              (var_start[i] >= 'A' && var_start[i] <= 'Z') ||
              (var_start[i] >= '0' && var_start[i] <= '9') ||
              var_start[i] == '_')) return 0;
    
    char var_name[65];
    memcpy(var_name, var_start, var_len);
    var_name[var_len] = 0;
    if (jash_readonly_is_set(var_name)) return 0;
    
    p++; /* skip = */
    while (*p && isspace(*p)) p++;
    
    /* Check for $(( pattern */
    if (strncmp(p, "$((", 3) != 0) return 0;
    p += 3;
    
    /* Find matching )) */
    const char* expr_start = p;
    int depth = 1;
    while (*p && depth > 0) {
        if (p[0] == '(' && p[1] == '(') { depth++; p += 2; continue; }
        if (p[0] == ')' && p[1] == ')') { depth--; if (depth == 0) break; p += 2; continue; }
        p++;
    }
    if (depth != 0) return 0;
    
    size_t expr_len = p - expr_start;
    if (expr_len == 0 || expr_len > 1024) return 0;
    
    /* Build full arithmetic expression: $((expr)) */
    char full_expr[1032];
    full_expr[0] = '$';
    full_expr[1] = '(';
    full_expr[2] = '(';
    memcpy(full_expr + 3, expr_start, expr_len);
    full_expr[3 + expr_len] = ')';
    full_expr[4 + expr_len] = ')';
    full_expr[5 + expr_len] = 0;
    
    p += 2; /* skip )) */
    while (*p && isspace(*p)) p++;
    
    /* Only an optional trailing separator is part of this fast path. Never
     * swallow a command after `;`/newline. */
    if (*p == ';') {
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (*p != 0) return 0;

    /* BUG-111: the DETECTION call must be side-effect-free — evaluating and
     * committing here made the pre-loop probe phantom-execute the assignment
     * once (empty word list → n=1 instead of 0; >unroll-limit lists → N+1). */
    if (dry_run) return 1;

    /* Evaluate arithmetic expression */
    jash_arith_clear_error();
    const char* result_str = jash_expand_arithmetic(full_expr, s);
    if (jash_arith_error()) {
        jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
        *exit_code = 1;
        return 1;
    }
    if (!result_str || *result_str == 0) return 0;
    
    /* Set variable directly (result_str already contains the number) */
    if (jash_env_set(s, var_name, result_str) != 0) {
        *exit_code = 1;
        return 1;
    }
    
    *exit_code = 0;
    return 1;
}
