// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/while_loop — while COND; do BODY; done support.

#include "script.h"
#include "eval.h"
#include "arithmetic.h"
#include "param_expand.h"
#include "quotes.h"   /* JINV-116: scanner único, ciente de aspas */
#include <core/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <theme/io.h>
#include <theme/colors.h>
#include <sys/syscalls.h>

/* eval.c: shared `(( expr ))` command shape check + evaluator (loop fast paths) */
extern int jash_arith_cmd_shape(const char* line, int interactive);
extern int jash_eval_arith_command(jash_state_t* s, jash_writer_t w, void* ctx,
                                   const char* line);

static const char* skip_ws(const char* p) {
    /* BUG-059: also eat newlines — between COND/do/body the multi-line form
     * uses them where the one-liner uses ';' (bash `linebreak` grammar). */
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

/* Loop-body shape shared by the fast paths below: a chain of NAME=$((EXPR))
 * statements separated by ';'/newline (the last optionally bare).
 * On match, *chain_start points at the first statement. */
static int body_is_arith_chain(const char* body, const char** chain_start) {
    const char* bp = body;
    while (*bp == ' ' || *bp == '\t' || *bp == ';') bp++;
    int body_ok = (*bp != '\0');
    const char* b_scan = bp;
    while (*b_scan && body_ok) {
        if (!(isalpha((unsigned char)*b_scan) || *b_scan == '_')) { body_ok = 0; break; }
        while (isalnum((unsigned char)*b_scan) || *b_scan == '_') b_scan++;
        if (*b_scan != '=') { body_ok = 0; break; }
        b_scan++;
        if (b_scan[0] != '$' || b_scan[1] != '(' || b_scan[2] != '(') { body_ok = 0; break; }
        b_scan += 3;
        int pdepth = 2;
        while (*b_scan && pdepth > 0) {
            if (*b_scan == '(') pdepth++;
            else if (*b_scan == ')') pdepth--;
            if (pdepth == 0) break;
            b_scan++;
        }
        if (pdepth != 0) { body_ok = 0; break; }
        b_scan++; /* second ) */
        while (*b_scan == ' ' || *b_scan == '\t') b_scan++;
        if (*b_scan == ';' || *b_scan == '\n') {
            while (*b_scan == ';' || *b_scan == '\n' || *b_scan == ' ' || *b_scan == '\t') b_scan++;
        } else if (*b_scan != '\0') { body_ok = 0; break; }
    }
    if (body_ok && chain_start) *chain_start = bp;
    return body_ok;
}

/* Execute a NAME=$((EXPR)) chain validated by body_is_arith_chain(). */
static void run_arith_chain(jash_state_t* s, const char* xp) {
    while (*xp) {
        char name[64]; unsigned ni = 0;
        while ((isalnum((unsigned char)*xp) || *xp == '_') && ni + 1 < sizeof(name))
            name[ni++] = *xp++;
        name[ni] = 0;
        xp++; /* = */
        xp += 3; /* $(( */
        char expr[256]; unsigned ei = 0;
        int d = 2;
        while (*xp && d > 0 && ei + 1 < sizeof(expr)) {
            if (*xp == '(') d++;
            else if (*xp == ')') d--;
            if (d == 0) break;
            expr[ei++] = *xp++;
        }
        expr[ei] = 0;
        long val = jash_eval_arithmetic(expr, s);
        char buf[32]; snprintf(buf, sizeof(buf), "%ld", val);
        jash_env_set(s, name, buf);
        if (*xp == ')') xp++;
        if (*xp == ')') xp++;
        while (*xp == ';' || *xp == '\n' || *xp == ' ' || *xp == '\t') xp++;
    }
}

/* Parse and execute: while/until COND; do BODY; done
 * is_until=0: while (loop while condition is true)
 * is_until=1: until (loop while condition is false) */
static int eval_cond_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* line, int is_until) {

    const char* keyword = is_until ? "until" : "while";
    const char* p = skip_ws(line);
    /* BUG-018: accept `while ((` and `while((` (no space) — same for until. */
    if (strncmp(p, keyword, 5) != 0 || !(isspace((unsigned char)p[5]) || p[5] == '(')) return -1;

    p = skip_ws(p + 5);
    const char* cond_start = p;

    /* Handle [[ ]] syntax — find matching ]] before looking for ; */
    if (p[0] == '[' && p[1] == '[') {
        /* Find matching ]] */
        int depth = 1;
        p += 2;
        while (*p && depth > 0) {
            if (p[0] == '[' && p[1] == '[') { depth++; p += 2; }
            else if (p[0] == ']' && p[1] == ']') { depth--; p += 2; }
            else { p++; }
        }
        /* Now find ; (or the multi-line newline separator) after ]] */
        p = jash_scan_unquoted(p, ";\n");
    } else {
        /* BUG-059/bash parity: `while COND<newline>do` is legal.
         * JINV-116: a varredura precisa respeitar aspas — `while [ "$x" = "a;b" ]`
         * terminava a condição no `;` de dentro da string e o `do` sumia. */
        p = jash_scan_unquoted(p, ";\n");
    }

    if (!*p) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: unexpected end of %s statement (missing ;). Did you mean: %s COND; do ...; done?%s\n",
                    J_ROLE_ERR, keyword, keyword, J_RESET);
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    /* ═══ JINV-082 — a CONDIÇÃO do laço também parava em 512 bytes ════════
     *
     * `char cond[512]` com o mesmo clamp mudo do corpo (abaixo). Uma condição
     * longa — `while [ -n "$x" ] && [ "$(cmd …)" != "…" ]` com nomes reais —
     * saía cortada e o laço passava a testar OUTRA coisa, sem uma palavra.
     * Mesma correção do JINV-080: pilha até 511 B (caso comum, zero malloc),
     * heap do tamanho exato acima, falta de memória = recusa ALTA. */
    size_t cond_len = p - cond_start;
    char  cond_inline[512];
    char* cond_heap = NULL;
    char* cond = cond_inline;
    if (cond_len >= sizeof(cond_inline)) {
        cond_heap = (char*)malloc(cond_len + 1);
        if (!cond_heap) {
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: %s: sem memoria para a condicao do laco — o laco NAO foi executado%s\n",
                        J_ROLE_ERR, keyword, J_RESET);
            { if (s && !s->interactive) s->exit_requested = 1; return 2; }
        }
        cond = cond_heap;
    }
    memcpy(cond, cond_start, cond_len);
    cond[cond_len] = 0;

    p = skip_ws(p + 1);
    if (strncmp(p, "do", 2) != 0 || !isspace(p[2])) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: %s requires 'do' keyword. Did you mean: %s COND; do ...; done?%s\n",
                    J_ROLE_ERR, keyword, keyword, J_RESET);
        if (cond_heap) free(cond_heap);   /* JINV-082 */
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    p = skip_ws(p + 2);
    const char* body_start = p;
    /* JINV-083 — o `done` que fecha, lido por PALAVRA e FORA DE ASPAS.
     *
     * O gemeo exato do site do `for`. O laco `while` e justamente a forma que
     * todo `configure` usa para varrer argumentos, e o corpo dele carrega o
     * `echo "$0 --help for help"` que fazia o `for ` CITADO subir a
     * profundidade — o `done` do autor deixava de fechar o laco. Faltava
     * tambem a fronteira a ESQUERDA de `done` (`undone` fechava). Uma leitura
     * so, em script.c: `jash_find_loop_close`. */
    const char* done_at = jash_find_loop_close(p);
    if (!done_at) {
        jash_writef(jash_stderr_writer, 0, "%sjash: syntax error: unexpected end of %s statement (missing done). Did you mean: %s COND; do ...; done?%s\n",
                    J_ROLE_ERR, keyword, keyword, J_RESET);
        if (cond_heap) free(cond_heap);   /* JINV-082 */
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }
    p = done_at;

    /* ═══ JINV-082 — o corpo do `while`/`until` não pode ser truncado ══════
     *
     * O gêmeo exato do JINV-080 (`for`), consertado no mesmo dia, no arquivo
     * ao lado, e deixado vivo aqui:
     *
     *     char body[1024];
     *     if (body_len >= sizeof(body)) body_len = sizeof(body) - 1;  // mudo
     *
     * Pior sintoma que o do `for`: o `for` ao menos morria alto às vezes
     * (o acumulador de main.c reclamava de `done` faltando). Aqui não —
     * o corpo perdia a cauda a partir de 2051 bytes de ARQUIVO e o laço
     * rodava assim mesmo, com rc 0 e stderr VAZIO. Medido:
     *
     *     while  2051 B  perde `echo MARCA`  │  rc 0, stderr vazio
     *     case   2044 B  idem (case_stmt.c)  │  rc 0, stderr vazio
     *
     * Um corpo de laço de 1 KB são ~15 linhas de comando. Todo laço real
     * passa disso — e o `while` é justamente a forma que os `configure`
     * usam para varrer argumentos.
     *
     * Correção (idêntica ao JINV-080): sem teto. Corpo até 1023 bytes segue
     * na pilha (caso comum, zero malloc); acima disso, heap do tamanho
     * exato. Falta de memória é recusa ALTA — o laço NÃO roda — e nunca
     * truncamento. Uma só porta de saída (`while_body_done`) libera os dois
     * buffers.
     *
     * Gate: tests/test_jinv082_loop_body_heap.sh */
    size_t body_len = p - body_start;
    char  body_inline[1024];
    char* body_heap = NULL;
    char* body = body_inline;
    if (body_len >= sizeof(body_inline)) {
        body_heap = (char*)malloc(body_len + 1);
        if (!body_heap) {
            jash_writef(jash_stderr_writer, 0,
                        "%sjash: %s: sem memoria para o corpo do laco — o laco NAO foi executado%s\n",
                        J_ROLE_ERR, keyword, J_RESET);
            if (cond_heap) free(cond_heap);
            { if (s && !s->interactive) s->exit_requested = 1; return 2; }
        }
        body = body_heap;
    }
    memcpy(body, body_start, body_len);
    body[body_len] = 0;

    /* Detect a trailing input/output redirect applied to the whole loop:
     *   while ...; do ...; done < file      (the common `while read` idiom)
     *   while ...; do ...; done > file
     * Without this, the post-`done` text "< file" was handed to jash_eval_line
     * as a bare command -> "<: command not found". We open the target and
     * dup2 it onto the loop's std fd for the loop's duration, then restore.
     * F E R M I ∞ H A R T */
    int redir_saved_fd = -1;   /* dup of the original fd to restore */
    int redir_target_fd = -1;  /* which std fd we redirected (0 or 1) */
    {
        const char* rp = p;
        if (strncmp(rp, "done", 4) == 0) rp = skip_ws(rp + 4);
        int want = -1;          /* 0 = stdin (<), 1 = stdout (>) */
        int append = 0;

        /* <<< here-string: passes an inline string as stdin for the whole loop.
         * bash expands the word (variable substitution, command substitution, etc.)
         * and appends a trailing newline before handing it to the loop stdin.
         * Use a temp file (mkstemp → dup2 → unlink) so we never need a real pipe. */
        if (rp[0] == '<' && rp[1] == '<' && rp[2] == '<') {
            rp = skip_ws(rp + 3);
            /* Extract the word — handle single/double quotes and bare tokens */
            char hs_raw[4096];
            unsigned hi = 0;
            char hs_quote = 0;
            if (*rp == '"' || *rp == '\'') {
                hs_quote = *rp++;
                while (*rp && *rp != hs_quote && hi + 2 < sizeof(hs_raw))
                    hs_raw[hi++] = *rp++;
                if (*rp == hs_quote) rp++;
            } else {
                while (*rp && *rp != ' ' && *rp != '\t' && *rp != ';' &&
                       *rp != '\n' && hi + 2 < sizeof(hs_raw))
                    hs_raw[hi++] = *rp++;
            }
            hs_raw[hi] = '\0';
            /* Expand variables (double-quoted and bare here-strings allow $VAR) */
            const char* hs_expanded = hs_raw;
            if (hs_quote != '\'' && s) hs_expanded = jash_param_expand(hs_raw, s);
            /* Build final content: expanded word + mandatory trailing newline */
            char hs_content[4096];
            unsigned hl = 0;
            const char* ep = hs_expanded ? hs_expanded : hs_raw;
            while (*ep && hl + 2 < sizeof(hs_content)) hs_content[hl++] = *ep++;
            hs_content[hl++] = '\n';
            hs_content[hl] = '\0';
            /* Write to a temp file and dup2 to stdin */
            char tmpname[256];
            snprintf(tmpname, sizeof(tmpname), "/tmp/jash_hs_XXXXXX");
            int tmpfd = mkstemp(tmpname);
            if (tmpfd >= 0) {
                sys_write(tmpfd, hs_content, hl);
                sys_close(tmpfd);
                int rdfd = open(tmpname, O_RDONLY);
                unlink(tmpname);
                if (rdfd >= 0) {
                    redir_target_fd = 0;
                    redir_saved_fd = dup(0);
                    dup2(rdfd, 0);
                    close(rdfd);
                    *(char*)p = '\0';   /* terminate at "done" */
                }
            }
        } else if (*rp == '<') { want = 0; rp = skip_ws(rp + 1); }
        else if (rp[0] == '>' && rp[1] == '>') { want = 1; append = 1; rp = skip_ws(rp + 2); }
        else if (*rp == '>') { want = 1; rp = skip_ws(rp + 1); }

        if (want >= 0 && *rp) {
            /* Extract the filename token (up to ws/;). */
            char fname[512];
            unsigned fi = 0;
            while (*rp && *rp != ' ' && *rp != '\t' && *rp != ';' &&
                   *rp != '\n' && fi + 1 < sizeof(fname))
                fname[fi++] = *rp++;
            fname[fi] = 0;
            int flags = (want == 0) ? O_RDONLY
                        : (O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
            int newfd = open(fname, flags, 0666);
            if (newfd >= 0) {
                redir_target_fd = want;
                redir_saved_fd = dup(want);
                dup2(newfd, want);
                close(newfd);
                /* Consume the redirect so the post-done handler below
                 * doesn't re-evaluate it as a command. */
                *(char*)p = '\0';   /* terminate at "done" — no trailing cmd */
            }
        }
    }

    int exit_code = 0;
    int ret_code  = 0;   /* JINV-082: uma so porta de saida, para liberar o heap */

    /* ─── FAST-PATH for arithmetic loops ────────────────────────────────────
     * The vast majority of shell loops look like:
     *     while [ $i -lt N ]; do i=$((i+1)); ... ; done
     * The full eval pipeline (tilde→brace→glob→cmdsub→arith→param) is overkill
     * here — every iter just compares one integer and updates one integer.
     * Detect that shape once, then run the hot loop without touching
     * jash_eval_line until the condition flips or the body grows non-trivial.
     *
     * Match shapes:
     *   COND :  [ $VAR OP NUM ]   OP in {-lt -le -gt -ge -eq -ne}
     *           [ $VAR OP $OTHER ]  (same OPs)
     *   BODY :  one or more  NAME=$((EXPR));  statements, last optionally bare.
     *           Body is reused as a string; arithmetic.c writes back into env.
     * F E R M I ∞ H A R T */
    {
        const char* cp = cond;
        while (*cp == ' ' || *cp == '\t') cp++;
        if (cp[0] == '[' && (cp[1] == ' ' || cp[1] == '\t')) {
            cp += 2;
            while (*cp == ' ' || *cp == '\t') cp++;
            if (cp[0] == '$' &&
                (isalpha((unsigned char)cp[1]) || cp[1] == '_')) {
                cp++;
                char lhs_name[64]; unsigned ln = 0;
                while ((isalnum((unsigned char)*cp) || *cp == '_') && ln + 1 < sizeof(lhs_name))
                    lhs_name[ln++] = *cp++;
                lhs_name[ln] = 0;
                while (*cp == ' ' || *cp == '\t') cp++;
                int op = 0; /* -lt -le -gt -ge -eq -ne */
                if (cp[0] == '-' && cp[2] && (cp[3] == ' ' || cp[3] == '\t')) {
                    if (cp[1] == 'l' && cp[2] == 't') op = 1;
                    else if (cp[1] == 'l' && cp[2] == 'e') op = 2;
                    else if (cp[1] == 'g' && cp[2] == 't') op = 3;
                    else if (cp[1] == 'g' && cp[2] == 'e') op = 4;
                    else if (cp[1] == 'e' && cp[2] == 'q') op = 5;
                    else if (cp[1] == 'n' && cp[2] == 'e') op = 6;
                }
                if (op) {
                    cp += 3;
                    while (*cp == ' ' || *cp == '\t') cp++;
                    /* rhs: either a literal int or $NAME */
                    int rhs_is_var = 0;
                    char rhs_name[64] = {0};
                    long rhs_lit = 0;
                    int rhs_negative = 0;
                    int rhs_ok = 0;
                    if (cp[0] == '$' && (isalpha((unsigned char)cp[1]) || cp[1] == '_')) {
                        cp++; unsigned rn = 0;
                        while ((isalnum((unsigned char)*cp) || *cp == '_') && rn + 1 < sizeof(rhs_name))
                            rhs_name[rn++] = *cp++;
                        rhs_name[rn] = 0;
                        if (rn) { rhs_is_var = 1; rhs_ok = 1; }
                    } else if (*cp == '-' || (*cp >= '0' && *cp <= '9')) {
                        if (*cp == '-') { rhs_negative = 1; cp++; }
                        while (*cp >= '0' && *cp <= '9') {
                            rhs_lit = rhs_lit * 10 + (*cp - '0'); cp++;
                        }
                        if (rhs_negative) rhs_lit = -rhs_lit;
                        rhs_ok = 1;
                    }
                    if (rhs_ok) {
                        while (*cp == ' ' || *cp == '\t') cp++;
                        if (cp[0] == ']' && (cp[1] == '\0' || cp[1] == ' ' || cp[1] == '\t')) {
                            /* Cond shape matched — verify body shape: a chain of
                             * NAME=$((EXPR)) statements, or a single `(( expr ))`
                             * command (see the (( ))-cond fast path below). */
                            const char* bp = NULL;
                            int body_chain = body_is_arith_chain(body, &bp);
                            int body_arith = !body_chain &&
                                             jash_arith_cmd_shape(body, s && s->interactive);
                            if (body_chain || body_arith) {
                                /* All conditions met — run the hot loop in-place. */
                                long iter_cap = 10000000;  /* hard ceiling, kills runaway loops */
                                int declined = 0;
                                while (iter_cap-- > 0) {
                                    const char* lv = jash_env_get(s, lhs_name);
                                    long lhs = (lv && *lv) ? strtol(lv, NULL, 0) : 0;
                                    long rhs = rhs_lit;
                                    if (rhs_is_var) {
                                        const char* rv = jash_env_get(s, rhs_name);
                                        rhs = (rv && *rv) ? strtol(rv, NULL, 0) : 0;
                                    }
                                    int cond_true = 0;
                                    switch (op) {
                                        case 1: cond_true = (lhs <  rhs); break;
                                        case 2: cond_true = (lhs <= rhs); break;
                                        case 3: cond_true = (lhs >  rhs); break;
                                        case 4: cond_true = (lhs >= rhs); break;
                                        case 5: cond_true = (lhs == rhs); break;
                                        case 6: cond_true = (lhs != rhs); break;
                                    }
                                    if (is_until) cond_true = !cond_true;
                                    if (!cond_true) break;
                                    /* Execute body */
                                    if (body_arith) {
                                        exit_code = jash_eval_arith_command(s, w, ctx, skip_ws(body));
                                        if (exit_code < 0) { declined = 1; break; }
                                    } else {
                                        run_arith_chain(s, bp);
                                    }
                                }
                                if (!declined) goto loop_done;
                            }
                        }
                    }
                }
            }
        }
    }

    /* ─── FAST-PATH for `(( expr ))` conditions ─────────────────────────────
     * The other canonical hot-loop shape:
     *     while (( i < N )); do i=$((i+1)); done
     *     while (( i < N )); do (( i++ ));   done
     * jash_arith_cmd_shape() (eval.c) certifies that cond/body are a single
     * `(( ... ))` command whose text the pre-phases of jash_eval_line would
     * pass through untouched; each iteration then re-evaluates through
     * jash_eval_arith_command() — the SAME function the normal dispatch
     * reaches, so side effects (i++, i+=2, var=expr, ...) and the rc mapping
     * (non-zero expr -> rc 0) behave identically. The NAME=$((EXPR)) chain
     * body of the [ ] fast path is accepted too. Anything outside these
     * shapes falls through to the general loop below; if the evaluator ever
     * declines (-1, no side effects) the general loop simply takes over. */
    if (jash_arith_cmd_shape(cond, s && s->interactive)) {
        const char* bp = NULL;
        int body_arith = jash_arith_cmd_shape(body, s && s->interactive);
        int body_chain = body_arith ? 0 : body_is_arith_chain(body, &bp);
        if (body_arith || body_chain) {
            long iter_cap = 10000000;  /* same runaway ceiling as above */
            int declined = 0;
            while (iter_cap-- > 0) {
                int crc = jash_eval_arith_command(s, w, ctx, cond);
                if (crc < 0) { declined = 1; break; }
                int cond_true = (crc == 0);
                if (is_until) cond_true = !cond_true;
                if (!cond_true) break;
                if (body_arith) {
                    exit_code = jash_eval_arith_command(s, w, ctx, skip_ws(body));
                    if (exit_code < 0) { declined = 1; break; }
                } else {
                    run_arith_chain(s, bp);
                    exit_code = 0;
                }
            }
            if (!declined) goto loop_done;
        }
    }

    /* No artificial iteration cap — POSIX while/until run until the condition
     * flips (or break / SIGINT). A hard 10000 cap silently corrupted long loops. */
    for (;;) {
        /* Evaluate condition */
        int cond_result = jash_eval_line(s, w, ctx, cond);
        
        /* while: exit when condition is false (non-zero)
         * until: exit when condition is true (zero) */
        if (is_until) {
            if (cond_result == 0) break;  /* until: condition true, exit */
        } else {
            if (cond_result != 0) break;  /* while: condition false, exit */
        }

        exit_code = jash_eval_line(s, w, ctx, body);
        /* BUG-012: return/exit from inside the loop stops it and bubbles up. */
        if (s->return_requested || s->exit_requested) break;
        /* Handle break (255) and continue (254) — BUG-166: N-level control
         * flow, same convention as for_loop.c.
         * JINV-073: rc sentinela + marca fora de banda (state.h) — sem a
         * marca, `while ...; do sh -c "exit 255"; done` saia do laco como se
         * o script tivesse pedido `break`. */
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
loop_done:

    /* Restore any loop-level redirect before handling post-done commands. */
    if (redir_saved_fd >= 0) {
        dup2(redir_saved_fd, redir_target_fd);
        close(redir_saved_fd);
    }

    /* BUG-012: return/exit inside the loop skips the post-`done` text.
     * JINV-082: daqui para baixo toda saida passa por `while_body_done`,
     * senao a condicao/corpo em heap vazam. */
    if (s->return_requested || s->exit_requested) { ret_code = exit_code; goto while_body_done; }

    /* JINV-075: mesma guarda do for_loop.c — um break/continue de nivel > 1
     * ainda subindo nao pode ser apagado pelo rc da cauda depois do `done`. */
    if (jash_loopctl_pending(s)) { ret_code = exit_code; goto while_body_done; }

    /* Check if there are commands after "done" */
    if (*p) {
        p += 4; /* skip "done" */
        p = skip_ws(p);
        if (*p == ';' || *p == '\n') {
            p++;
            p = skip_ws(p);
        }
        if (*p) {
            /* JINV-082 (como no JINV-080): a cauda pode ser outro laco que
             * tambem aloca — libera ANTES de recursar, nao depois. */
            int rest_rc;
            if (body_heap) { free(body_heap); body_heap = NULL; }
            if (cond_heap) { free(cond_heap); cond_heap = NULL; }
            rest_rc = jash_eval_line(s, w, ctx, p);
            return rest_rc;
        }
    }

    ret_code = exit_code;

while_body_done:                     /* JINV-082: porta unica de saida */
    if (body_heap) free(body_heap);
    if (cond_heap) free(cond_heap);
    return ret_code;
}

/* Parse and execute: while COND; do BODY; done */
int jash_eval_while_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* line) {
    return eval_cond_loop(s, w, ctx, line, 0);
}

/* Parse and execute: until COND; do BODY; done */
int jash_eval_until_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* line) {
    return eval_cond_loop(s, w, ctx, line, 1);
}
