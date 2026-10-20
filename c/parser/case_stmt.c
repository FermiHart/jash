// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/case_stmt — case VAR in PATTERN) CMD ;; esac support.

#include "script.h"
#include "eval.h"
#include "param_expand.h"
#include "cmdsub.h"
#include "quotes.h"
#include <core/env.h>
#include <stdlib.h>   /* JINV-082: malloc/free do corpo do ramo sem teto */
#include <string.h>
#include <ctype.h>
#include <theme/io.h>
#include <theme/colors.h>

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

/* ═══ JINV-085 — nomes de classe POSIX dentro de `[ ]` ════════════════════
 *
 * `[[:digit:]]`, `[[:alpha:]]`, `[[:space:]]`… POSIX XCU 2.13.1 manda que a
 * expressão de colchete de um padrão de shell aceite as mesmas classes da
 * expressão regular. Nenhum dos quatro casadores de glob do jash as tinha:
 * `c/core/glob.c` (`match_class`, a porta do glob de arquivo) também não, e
 * por isso NÃO havia semântica pronta para copiar aqui — esta é a primeira.
 * A divergência é deliberada e fica registrada: enquanto `core/glob.c` não
 * ganhar o mesmo tratamento, `[[:digit:]]` vale em `case` e não vale no glob
 * de arquivo. (As três outras portas já divergem entre si por decisão — ver
 * o cabeçalho do JINV-065 — então unificar não é opção; estender é.)
 *
 * Devolve 1 casou, 0 não casou, -1 nome de classe desconhecido. O chamador
 * trata -1 como "não casou" (é o que o /bin/bash 3.2 faz — verificado contra
 * o oráculo no gate, asserção de classe desconhecida). */
static int case_posix_class(const char* name, size_t n, unsigned char c) {
#define JCLS(s) (n == sizeof(s) - 1 && memcmp(name, s, n) == 0)   /* JINV-085 */
    if (JCLS("alpha"))  return isalpha(c)  != 0;
    if (JCLS("digit"))  return isdigit(c)  != 0;
    if (JCLS("alnum"))  return isalnum(c)  != 0;
    if (JCLS("upper"))  return isupper(c)  != 0;
    if (JCLS("lower"))  return islower(c)  != 0;
    if (JCLS("space"))  return isspace(c)  != 0;
    if (JCLS("blank"))  return c == ' ' || c == '\t';
    if (JCLS("punct"))  return ispunct(c)  != 0;
    if (JCLS("print"))  return isprint(c)  != 0;
    if (JCLS("graph"))  return isgraph(c)  != 0;
    if (JCLS("cntrl"))  return iscntrl(c)  != 0;
    if (JCLS("xdigit")) return isxdigit(c) != 0;
    if (JCLS("ascii"))  return c < 128;
    if (JCLS("word"))   return isalnum(c) != 0 || c == '_';
#undef JCLS
    return -1;
}

static int match_bracket(const char** pattern, char c) {
    const char* p = *pattern + 1;
    int negate = 0;
    int matched = 0;
    /* JINV-085 — o PRIMEIRO caractere do conjunto pode ser `]` e aí ele é
     * MEMBRO LITERAL, não o fechamento (POSIX XBD 9.3.5). Sem esta bandeira,
     * `[]]` era lido como conjunto VAZIO seguido de nada: `case ']' in []])`
     * nunca casava, e `[!]]` (o complemento) casava tudo ao contrário.
     * Semântica copiada de `c/core/glob.c:match_class`, que já tinha o `first`
     * certo — esta porta era a que estava fora de compasso. */
    int first = 1;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }

    while (*p && (*p != ']' || first)) {   /* JINV-085: `]` inicial é membro */
        first = 0;
        /* JINV-085 — `[:nome:]` dentro do conjunto. Tem que vir ANTES do teste
         * de faixa: em `[[:digit:]]` o par `p[1] == '-'` nunca dispara, mas em
         * `[[:a-z:]]` (nome inválido com hífen) dispararia e comeria o `:]`. */
        if (p[0] == '[' && p[1] == ':') {
            const char* q = p + 2;
            while (*q && !(q[0] == ':' && q[1] == ']')) q++;
            if (q[0] == ':' && q[1] == ']') {
                if (case_posix_class(p + 2, (size_t)(q - (p + 2)),
                                     (unsigned char)c) > 0) matched = 1;
                p = q + 2;
                continue;
            }
            /* sem `:]` fechando: `[` é membro literal, cai no ramo comum */
        }
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char start = p[0];
            char end = p[2];
            if (start <= c && c <= end) matched = 1;
            p += 3;
        } else {
            if (*p == c) matched = 1;
            p++;
        }
    }

    if (*p == ']') {
        *pattern = p + 1;
        return negate ? !matched : matched;
    }

    /* Unterminated [: treat [ literally. */
    if (c == '[') {
        *pattern = *pattern + 1;
        return 1;
    }
    return 0;
}

/* JINV-065 — orçamento de passos, segunda linha de defesa.
 * O colapso da corrida de `*` (abaixo) mata a forma `*****b`, que é a única em
 * que um casador correto é linear. Ele NÃO torna o backtracking linear em geral
 * (`*a*a*a…b` continua exponencial — e o /bin/bash também trava nele, então não
 * é defeito do jash). O orçamento existe para que a PRÓXIMA forma patológica
 * seja RECUSADA em vez de pendurar o shell. Mesmo teto de `c/core/glob.c`
 * (JASH_GLOB_MAX_STEPS), a única porta que já tinha essa defesa.
 *
 * DIFERENÇA deliberada em relação a `core/glob.c`: lá o estouro devolve "não
 * casou" em SILÊNCIO. Aqui ele GRITA no stderr antes de devolver — devolver
 * falso calado é exatamente a família de defeito que esta auditoria persegue
 * (o shell responderia rápido e ERRADO, e ninguém saberia). */
#define JASH_CASE_GLOB_MAX_STEPS 4000000L
static long g_case_glob_steps;
static int  g_case_glob_over;

/* Simple glob match: * for any, ? for one char, [...] for set/range. */
static int case_glob_match_rec(const char* pattern, const char* text) {
    if (++g_case_glob_steps > JASH_CASE_GLOB_MAX_STEPS) {  /* JINV-065 */
        g_case_glob_over = 1;
        return 0;
    }
    while (*pattern && *text) {
        if (*pattern == '*') {
            /* JINV-065 — colapsa a CORRIDA inteira de `*`, não um só.
             * `*****` É `*`: dar a cada asterisco da corrida uma varredura
             * própria de todo o resto do texto multiplica as varreduras entre
             * si (C(n,k) chamadas). E a forma globstar que agente escreve todo
             * dia — dois asteriscos, barra, repetido três vezes, ".c" no fim
             * (não dá para escrever aqui: barra-asterisco fecha o comentário) —
             * pendura o shell com um caminho de 118 caracteres, sem nada de
             * patológico à vista. O idioma já estava escrito no rabo
             * desta função (onde só roda com o texto esgotado, protegendo
             * nada) e no ramo quente de `c/parser/conditional.c:98`, que por
             * isso era imune. Sem isto o `while (*text)` abaixo recursa uma vez
             * por asterisco da corrida. */
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;  /* * at end matches rest */
            while (*text) {
                if (case_glob_match_rec(pattern, text)) return 1;
                if (g_case_glob_over) return 0;  /* JINV-065: desenrola já */
                text++;
            }
            /* text exhausted: try matching remaining pattern against empty string */
            return case_glob_match_rec(pattern, text);
        } else if (*pattern == '?') {
            pattern++;
            text++;
        } else if (*pattern == '[') {
            if (!match_bracket(&pattern, *text)) return 0;
            text++;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else {
            return 0;
        }
    }
    /* Consume trailing wildcards that can match empty string */
    while (*pattern == '*') pattern++;
    return !*pattern && !*text;
}

/* JINV-065 — porta única de entrada do casador: zera o orçamento e, se ele
 * estourar, RECUSA o padrão em voz alta. O contrato para quem chama é: se
 * `g_case_glob_over` ficou de pé, o 0 devolvido significa "não sei", não
 * "não casou" — pare de tentar as outras alternativas do `|`. */
static int glob_match(const char* pattern, const char* text) {
    g_case_glob_steps = 0;
    g_case_glob_over  = 0;
    int r = case_glob_match_rec(pattern, text);
    if (g_case_glob_over) {
        jash_writef(jash_stderr_writer, 0,
                    "jash: case: padrao recusado por custo — o casamento "
                    "estourou o orcamento de %ld passos: '%s'\n",
                    (long)JASH_CASE_GLOB_MAX_STEPS, pattern);
        return 0;
    }
    return r;
}

/* ═══ JINV-085 — a alternação do `case` ═══════════════════════════════════
 *
 * ── O defeito 1: o `|` era separador MESMO DENTRO DE ASPAS ──────────────
 *
 * A varredura era `while (*p && *p != '|') p++;` — nua, sem nenhum rastreio
 * de citação. Um `|` citado é DADO, e virava estrutura:
 *
 *     case 'a|b' in 'a|b') echo SIM;; *) echo NAO;; esac
 *       bash -> SIM        jash -> NAO
 *
 * É exatamente a família JINV-016 / JINV-020 / JINV-028 / JINV-032 — um
 * caractere que era dado virou sintaxe — na última varredura de separador do
 * repositório que ainda estava nua.
 *
 * ── O defeito 2: o branco em volta do `|` entrava no padrão ─────────────
 *
 * Este é o que paga a conta. POSIX XCU 2.9.4 diz que o padrão é uma PALAVRA;
 * o branco em volta do `|` é separador de token e não pertence a ela. A
 * varredura antiga entregava as alternativas com o branco colado:
 *
 *     case $ac_val in
 *       [\\/$]* | ?:[\\/]* )  continue;;
 *
 *   alternativa 1 = "[\\/$]* "   (espaço no fim)
 *   alternativa 2 = " ?:[\\/]* " (espaço nos dois lados)
 *
 * Nenhuma das duas casa NADA, porque o texto comparado não termina em
 * espaço. **Toda cláusula de `case` com espaço em volta do `|` era morta** —
 * e essa é a forma que o autoconf escreve, em todo `configure` que existe.
 *
 * MEDIDO: das 38 invocações de `configure --help` / `--version` desta
 * máquina, 12 morriam com
 *
 *     configure: error: expected an absolute path for --exec_prefix: NONE
 *
 * que sai deste bloco do autoconf (a checagem de caminho absoluto do prefixo).
 * O bloco reúne TRÊS defeitos desta porta de uma vez: o branco em volta do
 * `|` (aqui), a alternativa CITADA `''` (aqui também), e o `case` ANINHADO no
 * corpo do ramo (JINV-085 no leitor de `;;`, mais abaixo).
 *
 * ── O que é aparado, e o que NÃO é ─────────────────────────────────────
 *
 * Só o branco da BORDA, e só quando ele é nu. Branco citado (`" a"`) ou
 * escapado (`a\ `) é dado e sobrevive — a mesma régua do `jash_rtrim_unquoted`
 * do JINV-063, que existe porque um rtrim cego comia o espaço escapado.
 * O `\|` também deixa de separar: é literal, como no bash. */
static int case_pattern_match(const char* pattern, const char* text) {
    const char* p = pattern;
    while (*p) {
        /* branco da borda esquerda: separador de token, não faz parte */
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;   /* JINV-085 */

        const char* start = p;
        /* `endtrim` acompanha o fim do último caractere SIGNIFICATIVO — assim
         * o branco da borda direita cai fora sem precisar de uma segunda
         * passada, e o branco citado/escapado nunca é confundido com ele. */
        const char* endtrim = p;                             /* JINV-085 */
        int sq = 0, dq = 0;
        while (*p) {
            char c = *p;
            if (c == '\\' && p[1]) { p += 2; endtrim = p; continue; }
            if (sq) { if (c == '\'') sq = 0; p++; endtrim = p; continue; }
            if (dq) { if (c == '"')  dq = 0; p++; endtrim = p; continue; }
            if (c == '\'') { sq = 1; p++; endtrim = p; continue; }
            if (c == '"')  { dq = 1; p++; endtrim = p; continue; }
            if (c == '|') break;   /* só AQUI o `|` é estrutura */
            p++;
            if (c != ' ' && c != '\t' && c != '\n') endtrim = p;
        }

        /* `part` acompanha `pattern[256]` do chamador: uma alternativa nunca
         * é maior que a lista inteira de onde saiu, e a aparadura só encolhe.
         * O clamp fica como cinto de segurança, inalcançável por construção. */
        char part[256];
        unsigned len = (unsigned)(endtrim - start);
        if (len >= sizeof(part)) len = sizeof(part) - 1;
        memcpy(part, start, len);
        part[len] = '\0';

        /* BUG-096 adjunct: patterns undergo QUOTE REMOVAL — `case "$v" in
         * '"')` must compare the literal ", not the quoted text. Quoted
         * pattern chars also lose glob meaning; jash_unquote strips the
         * quotes (glob chars inside quotes staying special is a pre-existing
         * limitation of this matcher). */
        {
            extern int jash_unquote(const char* in, char* out, size_t out_size);
            extern int jash_has_quotes(const char* str);
            if (jash_has_quotes(part)) {
                char unq[256];
                if (jash_unquote(part, unq, sizeof(unq)) >= 0) {
                    memcpy(part, unq, sizeof(part) - 1);
                    part[sizeof(part) - 1] = '\0';
                }
            }
            /* BUG-119/096: expanded VALUES keep their literal quotes —
             * unshield AFTER quote removal so they are data, not syntax. */
            jash_quote_unshield(part);
        }

        if (glob_match(part, text)) return 1;
        /* JINV-065: padrão recusado por custo — não gaste o mesmo orçamento
         * de novo em cada alternativa do `|`, e não deixe a recusa virar um
         * "não casou" mudo repetido N vezes. */
        if (g_case_glob_over) break;
        if (*p == '|') p++;
    }
    return 0;
}

/* Parse and execute: case EXPR in PATTERN) CMD ;; ... esac */
int jash_eval_case_stmt(jash_state_t* s, jash_writer_t w, void* ctx,
                        const char* line) {

    const char* p = skip_ws(line);
    if (strncmp(p, "case", 4) != 0 || !isspace(p[4])) return -1;

    p = skip_ws(p + 4);
    const char* expr_start = p;

    /* Find the 'in' keyword: scan for whitespace-delimited tokens,
     * checking if the next token is 'in'. We need to handle:
     *   case $VAR in ...    case "literal" in ...    case word in ...
     * The expression is everything up to (but not including) the 'in' keyword. */
    char expr[256];
    const char* in_pos = NULL;
    {
        const char* scan = p;
        while (*scan) {
            /* Skip current token */
            if (*scan == '"') {
                scan++;
                while (*scan && *scan != '"') {
                    if (*scan == '\\' && scan[1]) scan++;
                    scan++;
                }
                if (*scan == '"') scan++;
            } else if (*scan == '\'') {
                scan++;
                while (*scan && *scan != '\'') scan++;
                if (*scan == '\'') scan++;
            } else {
                while (*scan && *scan != ' ' && *scan != '\t') scan++;
            }
            /* After token, skip whitespace and check if next token is 'in' */
            const char* after_tok = scan;
            while (*after_tok == ' ' || *after_tok == '\t') after_tok++;
            if (strncmp(after_tok, "in", 2) == 0 &&
                (after_tok[2] == '\0' || isspace((unsigned char)after_tok[2]))) {
                /* Found 'in' keyword. Expression is from expr_start to end of current token. */
                in_pos = after_tok;
                /* Trim trailing whitespace from expression */
                const char* expr_end = scan;
                while (expr_end > expr_start && (expr_end[-1] == ' ' || expr_end[-1] == '\t'))
                    expr_end--;
                size_t expr_len = expr_end - expr_start;
                if (expr_len >= sizeof(expr)) expr_len = sizeof(expr) - 1;
                memcpy(expr, expr_start, expr_len);
                expr[expr_len] = 0;
                p = in_pos;
                break;
            }
            /* Not 'in' yet — include this token in expression and continue */
            if (!*scan) {
                /* Reached end without finding 'in' */
                break;
            }
            /* Skip whitespace between tokens */
            while (*scan == ' ' || *scan == '\t') scan++;
        }
    }

    if (!in_pos) {
        /* Fallback: no 'in' found */
        size_t expr_len = strlen(expr_start);
        if (expr_len >= sizeof(expr)) expr_len = sizeof(expr) - 1;
        memcpy(expr, expr_start, expr_len);
        expr[expr_len] = 0;
        p = expr_start + expr_len;
    }

    p = skip_ws(p);
    if (strncmp(p, "in", 2) != 0 || (p[2] != '\0' && !isspace((unsigned char)p[2]) && p[2] != ';')) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: case requires 'in' keyword. Did you mean: case %s in ... ;; esac?%s\n",
                    J_ROLE_ERR, expr, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    p = skip_ws(p + 2);

    /* Get expr value — handle $VAR, "$VAR", 'literal', "literal", ${var}, $(cmd) */
    char expr_value[512];
    {
        const char* src = expr;
        size_t len = strlen(expr);
        int is_single_quoted = 0;

        /* Strip surrounding quotes if present */
        if (len >= 2 && expr[0] == '\'' && expr[len-1] == '\'') {
            /* Single quotes: literal, no expansion */
            src = expr + 1;
            len -= 2;
            is_single_quoted = 1;
        } else if (len >= 2 && expr[0] == '"' && expr[len-1] == '"') {
            /* Double quotes: strip quotes, then expand */
            src = expr + 1;
            len -= 2;
        }

        char tmp[512];
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, src, len);
        tmp[len] = 0;

        if (is_single_quoted) {
            /* No expansion for single-quoted */
            memcpy(expr_value, tmp, len + 1);
        } else {
            /* First: command substitution $(cmd) and `cmd` */
            const char* cs_expanded = jash_expand_cmdsub(s, tmp);
            if (cs_expanded && cs_expanded != tmp) {
                size_t cslen = strlen(cs_expanded);
                if (cslen >= sizeof(tmp)) cslen = sizeof(tmp) - 1;
                memcpy(tmp, cs_expanded, cslen);
                tmp[cslen] = 0;
                /* JINV-089 — o restauro do `$` blindado MUDOU DE LUGAR, não
                 * sumiu: ele acontece logo abaixo, em `expr_value`, DEPOIS da
                 * expansão de parâmetros (a chamada que já existia lá e cujo
                 * próprio comentário diz "the earlier unshield ran before
                 * param_expand").
                 *
                 * Aqui em cima ele era PREMATURO e desfazia a blindagem do
                 * SECURITY #15 exatamente no ponto que ela existe para
                 * proteger: um `$` vindo da SAÍDA da substituição voltava a
                 * ser `$` vivo e a passada seguinte o expandia —
                 *
                 *   V=VAL; case $`echo V` in '$V') echo LIT;; *) echo OTRO;; esac
                 *     bash -> LIT        jash -> OTRO   (expandiu $V de novo)
                 *
                 * O `jash_quote_unshield` CONTINUA aqui: aspas blindadas
                 * (BUG-096) têm que voltar ANTES da expansão, senão o
                 * `param_expand` não reconhece a citação. São duas marcas com
                 * tempos de vida diferentes — de propósito. */
                jash_quote_unshield(tmp);        /* BUG-096 */
            }

            /* Then: variable/parameter expansion */
            if (*tmp == '$' || jash_has_param_expansion(tmp)) {
                const char* expanded = jash_param_expand(tmp, s);
                if (expanded) {
                    size_t elen = strlen(expanded);
                    if (elen >= sizeof(expr_value)) elen = sizeof(expr_value) - 1;
                    memcpy(expr_value, expanded, elen);
                    expr_value[elen] = 0;
                } else {
                    /* Fallback: simple $VAR */
                    const char* val = jash_env_get(s, tmp + 1);
                    size_t vlen = val ? strlen(val) : 0;
                    if (vlen >= sizeof(expr_value)) vlen = sizeof(expr_value) - 1;
                    if (val) memcpy(expr_value, val, vlen);
                    expr_value[vlen] = 0;
                }
            } else {
                /* Literal value (after cmdsub) */
                size_t tlen = strlen(tmp);
                if (tlen >= sizeof(expr_value)) tlen = sizeof(expr_value) - 1;
                memcpy(expr_value, tmp, tlen);
                expr_value[tlen] = 0;
            }
        }
    }
    /* BUG-096: param expansion above emits quote sentinels — restore them in
     * the final compare value (the earlier unshield ran before param_expand). */
    jash_cmdsub_unprotect(expr_value);
    jash_quote_unshield(expr_value);

    /* Process patterns */
    int exit_code = 0;
    int matched = 0;
    int fall_into = 0;   /* set by `;&`: run the next clause without testing */

    /* JINV-083 — `esac` fecha o `case` so quando e PALAVRA.
     *
     * As duas checagens abaixo eram `strncmp(p, "esac", 4)` nu: sem fronteira
     * a direita, qualquer padrao que COMECE com essas quatro letras fechava o
     * enunciado antes da hora e a clausula nunca casava —
     *
     *     case esacx in esacx) echo hit;; *) echo miss;; esac
     *       bash -> hit        jash -> (nada)
     *     case esacX in esac*) echo hit;; *) echo miss;; esac
     *       bash -> hit        jash -> (nada)
     *
     * `)` fica no conjunto de terminadores de proposito: um `esac)` nu e erro
     * de sintaxe no proprio bash, e a forma legal (`(esac)`) chega aqui ja com
     * o parentese consumido logo abaixo. */
#define JASH_CASE_ESAC_END " \t\n;)"   /* JINV-083 */
    while (*p && !jash_keyword_at(p, "esac", JASH_CASE_ESAC_END)) {
        p = skip_ws(p);
        if (jash_keyword_at(p, "esac", JASH_CASE_ESAC_END)) break;
        if (!*p) break;

        /* BUG-088: POSIX allows an optional open paren before the pattern —
         * `case x in (x) …;; esac`. Without skipping it, the paren became
         * part of the pattern text and the clause silently never matched. */
        if (*p == '(') p = skip_ws(p + 1);
        const char* pattern_start = p;
        while (*p && *p != ')') p++;
        if (!*p) return 1;

        size_t pattern_len = p - pattern_start;
        char pattern[256];
        if (pattern_len >= sizeof(pattern)) pattern_len = sizeof(pattern) - 1;
        memcpy(pattern, pattern_start, pattern_len);
        pattern[pattern_len] = 0;

        /* BUG-119: patterns undergo parameter/command expansion — `case x in
         * $pat)` must match the VALUE of $pat. Quote sentinels from the
         * expansion stay shielded here; case_pattern_match unshields AFTER
         * its quote removal so literal quotes in values are data. */
        if (strchr(pattern, '$') || strchr(pattern, '`')) {
            const char* px = pattern;
            if (jash_has_cmdsub(px)) {
                const char* x = jash_expand_cmdsub_known(s, px);
                if (x) px = x;
            }
            if (jash_has_param_expansion(px)) {
                const char* x = jash_param_expand(px, (void*)s);
                if (x) px = x;
            }
            if (px != pattern) {
                size_t pl = strlen(px);
                if (pl >= sizeof(pattern)) pl = sizeof(pattern) - 1;
                memcpy(pattern, px, pl);
                pattern[pl] = 0;
            }
            jash_cmdsub_unprotect(pattern);
        }

        p++;  /* skip ) */
        p = skip_ws(p);

        const char* cmd_start = p;
        /* JINV-032 — o `;;` que fecha o ramo tem que estar FORA de aspas.
         *
         * Este laco era `while (*p && strncmp(p, ";;", 2) != 0) p++;` — sem
         * nenhum rastreio de citacao. Um `;;` dentro de aspas terminava o ramo
         * no meio do texto:
         *
         *   case x in x) echo "a;;b";; esac
         *     bash -> a;;b        jash -> "a
         *
         * Mesma classe do JINV-016, do JINV-020 e do JINV-028: um caractere que
         * era DADO virava estrutura. Aqui o estrago e duplo — o comando executa
         * truncado E o resto do texto vira o proximo ramo do case.
         *
         * Das nove varreduras de separador do repositorio, esta era a ULTIMA
         * que ainda divergia do bash numa tabela de 17 casos-armadilha (as
         * outras oito ja estavam corretas, algumas por ordem de execucao). */
        /* ═══ JINV-085 — um `case` DENTRO do ramo tem os `;;` dele ═════════
         *
         * O leitor abaixo já sabia que um `;;` citado ou dentro de `$( )` não
         * fecha o ramo (JINV-032). Faltava o terceiro esconderijo: o `;;` de
         * um `case` ANINHADO. Sem contar o aninhamento, o PRIMEIRO `;;` do
         * filho fechava o ramo do PAI —
         *
         *     case a in a) case b in b) echo S;; esac;; esac
         *       bash -> S        jash -> (nada, rc 1)
         *
         * — o corpo virava o pedaço `case b in b) echo S`, sem `esac`, que
         * morre calado com rc 1, e o resto (`esac;; esac`) era relido como
         * continuação do pai. Aninhar `case` não é exótico: é a forma que o
         * autoconf usa na checagem de caminho absoluto do prefixo,
         *
         *     NONE | '' ) case $ac_var in *prefix ) continue;; esac;;
         *
         * que é exatamente a linha que produzia `--exec_prefix: NONE` em 12
         * das 38 invocações de `configure` medidas.
         *
         * ── Por que POSIÇÃO DE COMANDO, e não só a palavra ──────────────
         *
         * `case` e `esac` são PALAVRAS RESERVADAS: só valem como sintaxe onde
         * um comando pode começar. Contar toda ocorrência da palavra faria
         *
         *     case a in a) case b in b) echo esac;; esac;; esac
         *
         * fechar o filho no `echo esac` — um argumento viraria estrutura, que
         * é a mesma família de defeito que este arquivo inteiro persegue, só
         * que na direção contrária. `cmdpos` reproduz a régua do shell: começo
         * do corpo, ou logo depois de `; & | ( ) { }` / nova linha, ou depois
         * de `then else elif do if while until !`.
         *
         * `in` está FORA dessa lista de propósito: senão `for i in case; do`
         * abriria um aninhamento que nunca fecha. Depois de `case X in` quem
         * devolve a posição de comando é o `)` do padrão, que já está na
         * lista. */
        {
            int sq = 0, dq = 0, back = 0, pdepth = 0;
            int casedepth = 0, cmdpos = 1;      /* JINV-085 */
            char dqsave[32] = {0};
            while (*p) {
                char c = *p;
                if (sq) { if (c == '\'') sq = 0; p++; continue; }
                if (dq) {
                    if (c == '\\' && p[1]) { p += 2; continue; }
                    /* POSIX 2.2.3: `$(` e crase reiniciam a citacao. */
                    if (c == '$' && p[1] == '(' && p[2] != '(') {
                        if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 1;
                        pdepth++; dq = 0; p += 2; continue;
                    }
                    if (c == '`') { back = 1; dq = 0; p++; continue; }
                    if (c == '"') dq = 0;
                    p++; continue;
                }
                if (back) { if (c == '`') back = 0; p++; continue; }
                if (c == '\\' && p[1]) { p += 2; cmdpos = 0; continue; }
                if (c == '\'') { sq = 1; p++; cmdpos = 0; continue; }
                if (c == '"')  { dq = 1; p++; cmdpos = 0; continue; }
                if (c == '`')  { back = 1; p++; cmdpos = 0; continue; }
                /* JINV-085 — palavra reservada em posição de comando. Só entra
                 * aqui em texto NU: nenhuma das aberturas de citação acima é
                 * letra, então a ordem dos testes fica preservada. */
                if (cmdpos && (isalpha((unsigned char)c) || c == '_')) {
                    const char* wend = p;
                    while (isalnum((unsigned char)*wend) || *wend == '_') wend++;
                    size_t wlen = (size_t)(wend - p);
                    if (wlen == 4 && memcmp(p, "case", 4) == 0) {
                        casedepth++;
                    } else if (wlen == 4 && memcmp(p, "esac", 4) == 0 &&
                               casedepth > 0) {
                        casedepth--;
                    }
                    /* palavras que mantêm a posição de comando aberta */
                    cmdpos = (wlen == 4 && memcmp(p, "then", 4) == 0) ||
                             (wlen == 4 && memcmp(p, "else", 4) == 0) ||
                             (wlen == 4 && memcmp(p, "elif", 4) == 0) ||
                             (wlen == 2 && memcmp(p, "do",   2) == 0) ||
                             (wlen == 2 && memcmp(p, "if",   2) == 0) ||
                             (wlen == 5 && memcmp(p, "while", 5) == 0) ||
                             (wlen == 5 && memcmp(p, "until", 5) == 0);
                    p = wend;
                    continue;
                }
                if (c == '$' && p[1] == '(') {
                    if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 0;
                    pdepth++; p += 2; cmdpos = 1; continue;
                }
                if (c == '(') {
                    if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 0;
                    pdepth++; p++; cmdpos = 1; continue;
                }
                if (c == ')') {
                    if (pdepth > 0) {
                        pdepth--;
                        if (pdepth < (int)sizeof(dqsave) && dqsave[pdepth]) {
                            dqsave[pdepth] = 0; dq = 1;
                        }
                    }
                    p++; cmdpos = 1; continue;
                }
                /* So aqui o `;;` e estrutura: fora de aspas, fora de $( ) —
                 * e fora de um `case` aninhado (JINV-085). */
                /* Three terminators, not one (bash 4.0+):
                 *   ;;   run this clause, leave the case
                 *   ;&   run this clause, then run the NEXT one unconditionally
                 *   ;;&  run this clause, then keep TESTING the ones after it
                 * All three start with ';' followed by ';' or '&'. */
                if (pdepth == 0 && c == ';' && (p[1] == ';' || p[1] == '&')) {
                    if (casedepth == 0) break;
                    p += (p[1] == ';' && p[2] == '&') ? 3 : 2;
                    cmdpos = 1; continue;           /* terminador do filho */
                }
                if (c == ';' || c == '&' || c == '|' ||
                    c == '\n' || c == '{' || c == '}' || c == '!') {
                    cmdpos = 1;                      /* JINV-085 */
                } else if (c != ' ' && c != '\t') {
                    cmdpos = 0;
                }
                p++;
            }
        }
        if (!*p) return 1;

        size_t cmd_len = p - cmd_start;
        /* Trim trailing whitespace so the dispatched command doesn't carry
         * a trailing space into builtins (echo would print "match " instead
         * of "match").
         * JINV-063: o corte mudou de lugar — acontece APOS a copia, onde o
         * corpo ja e string terminada em NUL, e usa a leitura CONSCIENTE DE
         * ASPAS de parser/quotes.h. O rtrim cego daqui tambem comia o espaco
         * ESCAPADO (`case y in y) echo x\  ;; esac` imprimia `x\`). */
        /* ═══ JINV-082 — o corpo de um ramo do `case` não pode ser truncado ══
         *
         * O terceiro gêmeo do `char body[1024]` (JINV-080 no `for`, JINV-082
         * no `while`), com o MESMO clamp mudo:
         *
         *     char cmd[1024];
         *     if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
         *
         * A partir de ~2044 bytes de arquivo o ramo perdia a cauda e rodava
         * assim mesmo: rc 0, stderr vazio, saída diferente da do bash. Um
         * `case` é justamente onde um script grande concentra corpo —
         * o `case $ac_option in … esac` de qualquer `configure` de autoconf
         * tem ramos de vários KB.
         *
         * Correção idêntica: pilha até 1023 bytes (caso comum, zero malloc),
         * heap do tamanho exato acima, falta de memória = recusa ALTA (o ramo
         * NÃO roda). Aqui o buffer é POR RAMO, então a liberação acontece no
         * fim de cada volta — nada acumula ao varrer um `case` de 50 ramos.
         *
         * Gate: tests/test_jinv082_loop_body_heap.sh */
        char  cmd_inline[1024];
        char* cmd_heap = NULL;
        char* cmd = cmd_inline;
        if (cmd_len >= sizeof(cmd_inline)) {
            cmd_heap = (char*)malloc(cmd_len + 1);
            if (!cmd_heap) {
                jash_writef(jash_stderr_writer, 0,
                            "%sjash: case: sem memoria para o corpo do ramo — o ramo NAO foi executado%s\n",
                            J_ROLE_ERR, J_RESET);
                if (s && !s->interactive) s->exit_requested = 1;
                return 2;
            }
            cmd = cmd_heap;
        }
        memcpy(cmd, cmd_start, cmd_len);
        cmd[cmd_len] = 0;
        cmd_len = jash_rtrim_unquoted(cmd);
        cmd[cmd_len] = 0;

        /* `;&` on the PREVIOUS clause means this one runs without being
         * tested at all; otherwise it runs only if it matches and nothing has
         * settled the case yet. */
        int run_this = fall_into;
        if (!run_this && !matched && case_pattern_match(pattern, expr_value))
            run_this = 1;
        fall_into = 0;

        if (run_this) {
            /* JINV-082: o corpo pode ser um composto que reentra neste
             * avaliador — libera ANTES de despachar não dá (o texto ainda é
             * lido), então guarda o rc e libera logo depois, sempre. */
            exit_code = jash_eval_line(s, w, ctx, cmd);

            if (p[0] == ';' && p[1] == '&') {
                fall_into = 1;              /* ;&  → next clause runs blind */
            } else if (p[0] == ';' && p[1] == ';' && p[2] == '&') {
                /* ;;& → keep testing: leave `matched` clear on purpose. */
            } else {
                matched = 1;                /* ;;  → the case is settled */
            }
        }
        if (cmd_heap) free(cmd_heap);   /* JINV-082: por ramo, nada acumula */

        /* Skip the terminator we just classified: `;;&` is three bytes. */
        p += (p[0] == ';' && p[1] == ';' && p[2] == '&') ? 3 : 2;
    }

    return exit_code;
}
