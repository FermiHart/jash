// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: set — shell options (errexit, nounset, xtrace, pipefail, posix, emacs, errtrace)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "builtin.h"
#include "shell_output.h"
#include <core/env.h>

static int print_options(jash_state_t* s);

/* ═══ JINV-067 / JINV-069 — a tabela ÚNICA de opções de shell ═════════════
 *
 * Tudo o que aceita uma opção de shell entra por aqui: o builtin `set`, e a
 * linha de comando (`jash -e -c …`, `jash -o errexit script.sh`) em c/main.c.
 * Antes havia duas listas: esta, e um `l/i/s/c` embutido no laço de argv que
 * dava `break` calado em qualquer outra letra — levando junto o `-c` que vinha
 * depois. Duas listas para a mesma pergunta é a raiz do JINV-067.
 *
 * `s == NULL` = modo VALIDAÇÃO: responde se a letra existe, sem escrever nada.
 * É o que permite main.c recusar `-Z` ANTES de alocar estado, sem manter uma
 * cópia da lista de letras válidas.
 *
 * As letras marcadas "inerte" existem no bash, são aceitas aqui, e este shell
 * ainda não muda de comportamento por causa delas. Elas são ARMAZENADAS e
 * relatadas por `set -o` — a divergência fica visível em vez de virar um
 * silêncio. O que NÃO é aceitável é o caso do `-n`: uma opção cujo valor é
 * emitir um veredito. Essa é recusada alto (JASH_OPT_UNSUPPORTED). */

/* JINV-069 — mensagem única. `jash -n f.sh` e `set -n` dizem exatamente isto. */
void jash_shell_opt_noexec_refuse(const char* como_foi_pedido) {
    fprintf(stderr,
        "jash: %s: checagem de sintaxe (noexec) NAO e suportada por este shell\n"
        "jash: jash nao separa analise de execucao — o mesmo passo que le e o\n"
        "jash: que roda. Nao existe estagio capaz de dizer \"isto e valido\" sem\n"
        "jash: executar, entao um rc 0 aqui seria uma aprovacao que este shell\n"
        "jash: nao pode sustentar (media: a envelopagem `if false; then <script>;\n"
        "jash: fi` perdeu sincronia e EXECUTOU o corpo em 773 de 2158 scripts\n"
        "jash: reais — audits/BENCHMARK-scripts-reais.md secao 3.4).\n"
        "jash: use `bash -n ARQUIVO` para checar sintaxe.\n",
        como_foi_pedido ? como_foi_pedido : "-n");
}

int jash_shell_opt_long(jash_state_t* s, const char* optname, int enable) {
    if (!optname || !*optname) {
        /* BUG-120a: `set -o` with no option name PRINTS the options (bash),
         * it is not an error. Em modo validação não há o que imprimir: um nome
         * vazio não é uma opção. */
        if (!s) return JASH_OPT_UNKNOWN;
        print_options(s);
        return JASH_OPT_OK;
    }

    /* JINV-069: `-o noexec` é a forma longa do `-n`. Mesma recusa. */
    if (strcmp(optname, "noexec") == 0) return JASH_OPT_UNSUPPORTED;

    /* Nomes longos ↔ letras curtas: uma linha só define os dois. */
    static const struct { const char* name; char letter; } por_letra[] = {
        { "errexit",   'e' }, { "nounset",  'u' }, { "xtrace",  'x' },
        { "verbose",   'v' }, { "noglob",   'f' }, { "noclobber", 'C' },
        { "monitor",   'm' }, { "allexport",'a' }, { "notify",  'b' },
        { "hashall",   'h' }, { "keyword",  'k' }, { "privileged", 'p' },
        { "onecmd",    't' }, { 0, 0 }
    };
    for (int i = 0; por_letra[i].name; i++)
        if (strcmp(optname, por_letra[i].name) == 0)
            return jash_shell_opt_short(s, por_letra[i].letter, enable);

    /* Opções que só têm forma longa. */
    if (strcmp(optname, "pipefail") == 0) { if (s) s->shell_opts.pipefail = enable; }
    else if (strcmp(optname, "posix") == 0) { if (s) s->shell_opts.posix = enable; }
    else if (strcmp(optname, "emacs") == 0) {
        if (s) { s->shell_opts.emacs = enable; if (enable) s->shell_opts.vi = 0; }
    } else if (strcmp(optname, "vi") == 0) {
        if (s) { s->shell_opts.vi = enable; if (enable) s->shell_opts.emacs = 0; }
    } else if (strcmp(optname, "errtrace") == 0) { if (s) s->shell_opts.errtrace = enable; }
    else return JASH_OPT_UNKNOWN;

    return JASH_OPT_OK;
}

int jash_shell_opt_short(jash_state_t* s, char opt, int enable) {
    switch (opt) {
        case 'e':
            /* errexit: exit on any error */
            if (s) s->shell_opts.errexit = enable;
            break;
        case 'u':
            /* nounset: error on undefined variables */
            if (s) s->shell_opts.nounset = enable;
            break;
        case 'x':
            /* xtrace: print commands before execution */
            if (s) s->shell_opts.xtrace = enable;
            break;
        case 'v':
            /* verbose: print input lines */
            if (s) s->shell_opts.verbose = enable;
            break;
        case 'f':
            /* noglob: disable pathname (glob) expansion */
            if (s) s->shell_opts.noglob = enable;
            break;
        case 'C':
            /* noclobber: prevent > from overwriting existing files */
            if (s) s->shell_opts.noclobber = enable;
            break;
        case 'm':
            /* monitor: job-control monitor mode. No interactive job control
             * under -c, so this is a stored no-op — accept it silently the way
             * bash does for a non-interactive shell. Agent harnesses prefix
             * `set +m` to quiet job-control chatter; erroring here leaked
             * "jash: set: unknown option: -m" into every captured response. */
            if (s) s->shell_opts.monitor = enable;
            break;

        /* JINV-067 — as seis que faltavam. Armazenadas e relatadas por
         * `set -o`; o efeito de cada uma neste shell, medido contra o bash: */
        case 'a':
            /* allexport. MEDIDO: o jash já exporta toda variável do estado
             * para os filhos (`jash -c 'FOO=bar; sh -c "echo \$FOO"'` imprime
             * bar; no bash imprime vazio). Ou seja `-a` é o comportamento
             * corrente e `+a` é que não tem como ser honrado hoje — a
             * divergência é anterior a esta flag e está anotada no relatório. */
            if (s) s->shell_opts.allexport = enable;
            break;
        case 'b':
            /* notify: só observável com job control interativo, que não existe
             * sob -c nem no bash. Mesmo caso do -m acima. */
            if (s) s->shell_opts.notify = enable;
            break;
        case 'h':
            /* hashall: o bash liga por padrão. É cache de PATH — sem efeito
             * semântico observável; o jash resolve a cada vez. */
            if (s) s->shell_opts.hashall = enable;
            break;
        case 'k':
            /* keyword: atribuição em qualquer posição vai para o ambiente do
             * comando. Ainda não honrado — armazenado e reportado. */
            if (s) s->shell_opts.keyword = enable;
            break;
        case 'p':
            /* privileged. Ainda não honrado — armazenado e reportado. */
            if (s) s->shell_opts.privileged = enable;
            break;
        case 't':
            /* onecmd: sair após um comando. Sob `-c` o bash roda a string
             * inteira mesmo com -t (medido: `bash -t -c 'echo a; echo b'`
             * imprime os dois), então aqui é fiel. */
            if (s) s->shell_opts.onecmd = enable;
            break;

        /* JINV-069 — `-n` (noexec) NÃO É ACEITA. Ver a mensagem acima. */
        case 'n':
            return JASH_OPT_UNSUPPORTED;

        default:
            return JASH_OPT_UNKNOWN;
    }
    return JASH_OPT_OK;
}

/* Nomes internos antigos, mantidos para o corpo de jash_b_set abaixo. */
static int set_long_option(jash_state_t* s, const char* optname, int enable) {
    int r = jash_shell_opt_long(s, optname, enable);
    if (r == JASH_OPT_UNSUPPORTED) {
        char como[80];
        snprintf(como, sizeof como, "set -o %s", optname ? optname : "");
        jash_shell_opt_noexec_refuse(como);
        /* Não-interativo: o bash pararia de executar a partir daqui (é o que
         * noexec faz). Continuar rodando o resto seria executar MAIS do que o
         * bash executaria — o lado perigoso da divergência. Então paramos. */
        if (s && !s->interactive) s->exit_requested = 1;
        return 2;
    }
    if (r == JASH_OPT_UNKNOWN) {
        fprintf(stderr, "jash: set: unknown option: %s\n",
                (optname && *optname) ? optname : "-o");
        return 1;
    }
    return 0;
}

static int set_short_option(jash_state_t* s, char opt, int enable) {
    int r = jash_shell_opt_short(s, opt, enable);
    if (r == JASH_OPT_UNSUPPORTED) {
        char como[16];
        snprintf(como, sizeof como, "set -%c", opt);
        jash_shell_opt_noexec_refuse(como);
        if (s && !s->interactive) s->exit_requested = 1;
        return -2;
    }
    return r;   /* 0 ok, -1 desconhecida */
}

/* Helper: print current shell option state */
/* BUG-120a: `set` with no args lists all shell variables as name=value
 * (sorted, values single-quoted bash-style when they contain anything
 * beyond a bare word). Internal keys (__…, positionals, array elements)
 * are skipped. This is what `set | grep '^name='` relies on. */
static int is_plain_word(const char* v) {
    for (const char* q = v; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (!(isalnum(c) || c == '_' || c == '/' || c == '.' ||
              c == '-' || c == ':' || c == '@' || c == '%' || c == '+'))
            return 0;
    }
    return 1;
}
static int print_shell_vars(jash_state_t* s, jash_writer_t w, void* ctx) {
    uint32_t n = jash_env_count(s);
    /* Collect eligible keys. */
    #define SV_MAX 512
    const char* names[SV_MAX];
    int nn = 0;
    for (uint32_t i = 0; i < n && nn < SV_MAX; i++) {
        const char *k = 0, *v = 0;
        if (!jash_env_at(s, i, &k, &v) || !k) continue;
        if (k[0] == '_' && k[1] == '_') continue;        /* jash internals */
        if (strchr(k, '[')) continue;                    /* array element key */
        int all_digit = k[0] != '\0';
        for (const char* q = k; *q; q++) if (*q < '0' || *q > '9') { all_digit = 0; break; }
        if (all_digit) continue;                         /* positional param */
        if (k[0] == '_' && k[1] == '\0') continue;       /* $_ */
        names[nn++] = k;
    }
    /* Insertion sort by name. */
    for (int i = 1; i < nn; i++) {
        const char* key = names[i]; int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) { names[j+1] = names[j]; j--; }
        names[j+1] = key;
    }
    for (int i = 0; i < nn; i++) {
        const char* v = jash_env_get(s, names[i]);
        if (!v) v = "";
        jash_writes(w, ctx, names[i]);
        jash_writes(w, ctx, "=");
        if (*v && is_plain_word(v)) {
            jash_writes(w, ctx, v);
        } else {
            /* single-quote, escaping embedded ' as '\'' (bash) */
            jash_writes(w, ctx, "'");
            for (const char* q = v; *q; q++) {
                if (*q == '\'') jash_writes(w, ctx, "'\\''");
                else { char cb[2] = { *q, 0 }; jash_writes(w, ctx, cb); }
            }
            jash_writes(w, ctx, "'");
        }
        jash_writes(w, ctx, "\n");
    }
    return 0;
}

static int print_options(jash_state_t* s) {
    if (!s) return 0;

    /* Print short-form options first */
    printf("set ");
    if (s->shell_opts.errexit) printf("-e ");
    if (s->shell_opts.nounset) printf("-u ");
    if (s->shell_opts.xtrace) printf("-x ");
    if (s->shell_opts.verbose) printf("-v ");
    /* JINV-067: as letras novas são LIDAS aqui — nenhum campo fica morto. */
    if (s->shell_opts.noglob)     printf("-f ");
    if (s->shell_opts.allexport)  printf("-a ");
    if (s->shell_opts.notify)     printf("-b ");
    if (s->shell_opts.hashall)    printf("-h ");
    if (s->shell_opts.keyword)    printf("-k ");
    if (s->shell_opts.privileged) printf("-p ");
    if (s->shell_opts.onecmd)     printf("-t ");

    /* Print long-form options */
    if (s->shell_opts.pipefail) printf("-o pipefail ");
    if (s->shell_opts.posix) printf("-o posix ");
    if (s->shell_opts.emacs) printf("-o emacs ");
    if (s->shell_opts.vi) printf("-o vi ");
    if (s->shell_opts.errtrace) printf("-o errtrace ");
    if (s->shell_opts.noclobber) printf("-o noclobber ");
    if (s->shell_opts.monitor)   printf("-o monitor ");

    printf("\n");
    return 0;
}

static int replace_positionals(jash_state_t* s, const char* const* vals,
                               int count) {
    return jash_env_replace_positionals(s, vals, count);
}

/* Tokenise a string into an argv-style array respecting quotes.
 * Returns number of tokens.  Modifies `buf` in place. */
static int tokenise_set_args(char* buf, const char** out, int max) {
    /* BUG-077: word-split honoring quotes AND the \x1E array-element boundary
     * (emitted by "${a[@]}") — a \x1E splits a word even INSIDE double quotes,
     * so `set -- "${a[@]}"` with a=("a b" "c d") yields TWO positionals, not
     * one. Quote state (sq/dq) PERSISTS across a \x1E boundary because all the
     * elements of "${a[@]}" share one surrounding quote pair.
     *
     * Compacted in place: the write cursor `w` copies content bytes and drops
     * structural quotes, and stays <= the read cursor `p` (we never insert),
     * so no byte is ever written past the buffer — preserving the earlier
     * OOB-safety audit. */
    int argc = 0;
    char* p = buf;   /* read  cursor */
    char* w = buf;   /* write cursor — invariant: w <= p */
    int sq = 0, dq = 0;
    while (*p) {
        /* Between words: skip separators — but NOT when continuing inside an
         * open quote after a \x1E split (the following bytes are content). */
        if (!dq && !sq)
            while (*p == ' ' || *p == '\t' || *p == '\x1E') p++;
        if (!*p) break;
        if (argc >= max) return -1;

        char* tok = w;
        for (;;) {
            char c = *p;
            if (c == '\0') break;
            if (sq) {
                if (c == '\'') { sq = 0; p++; continue; }
                *w++ = c; p++; continue;
            }
            if (dq) {
                if (c == '"') { dq = 0; p++; continue; }
                if (c == '\x1E') break;               /* array boundary — end word, stay in dq */
                if (c == '\\' && (p[1] == '"' || p[1] == '\\' ||
                                  p[1] == '$' || p[1] == '`')) {
                    p++; *w++ = *p; p++; continue;    /* dq escape → the escaped char */
                }
                *w++ = c; p++; continue;
            }
            if (c == ' ' || c == '\t' || c == '\x1E') break;
            if (c == '\'') { sq = 1; p++; continue; }
            if (c == '"')  { dq = 1; p++; continue; }
            if (c == '\\') { if (p[1]) { p++; *w++ = *p; p++; } else p++; continue; }
            *w++ = c; p++;
        }

        /* Terminate the token. w <= p, so *w is within the buffer. If we
         * stopped on a separator, step past that single byte (the top-of-loop
         * skip handles any run that follows). */
        int hit_sep = (*p == ' ' || *p == '\t' || *p == '\x1E');
        *w = '\0';
        w++;
        {
            extern void jash_quote_unshield(char* s);
            jash_quote_unshield(tok);   /* BUG-096 */
        }
        out[argc++] = tok;
        if (hit_sep) p++;
    }
    return argc;
}

static int positional_capacity(jash_state_t* s, int count) {
    int old = 0;
    for (int i = 1; i <= JASH_POS_MAX; i++) {
        char key[8]; snprintf(key, sizeof key, "%d", i);
        if (!jash_env_get(s, key)) break;
        old++;
    }
    return s->env_count - (uint32_t)old + (uint32_t)count <= JASH_MAX_ENV;
}

int jash_b_set(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "set", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    (void)w;
    (void)ctx;

    if (!s) {
        return 1;
    }

    if (!args || !*args) {
        /* BUG-120a: `set` with no args lists VARIABLES (bash), not options.
         * `set -o` prints the options. */
        return print_shell_vars(s, w, ctx);
    }

    /* Make a mutable copy for tokenisation */
    size_t args_len = strlen(args);
    char* buf = malloc(args_len + 1);
    if (!buf) return 1;
    memcpy(buf, args, args_len + 1);

    /* Skip leading whitespace to check for -- */
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;

    /* Handle "--" at start of args */
    if (p[0] == '-' && p[1] == '-') {
        const char* after = p + 2;
        while (*after == ' ' || *after == '\t') after++;

        const char* pvals[JASH_POS_MAX + 1];
        char* mutable_after = (char*)after;
        int pcount = tokenise_set_args(mutable_after, pvals, JASH_POS_MAX);
        if (pcount < 0) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: more than 384 positional parameters — refusing\n");
            free(buf);
            return 1;
        }
        if (!positional_capacity(s, pcount)) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: positional storage exhausted — refusing\n");
            free(buf);
            return 1;
        }
        if (replace_positionals(s, pvals, pcount) != 0) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: positional storage exhausted\n");
            free(buf);
            return 1;
        }
        free(buf);
        return 0;
    }

    /* Look for " -- " embedded in args */
    const char* dd = strstr(buf, " -- ");
    if (dd) {
        const char* after = dd + 4;
        while (*after == ' ' || *after == '\t') after++;

        const char* pvals[JASH_POS_MAX + 1];
        char* mutable_after = (char*)after;
        int pcount = tokenise_set_args(mutable_after, pvals, JASH_POS_MAX);
        if (pcount < 0) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: more than 384 positional parameters — refusing\n");
            free(buf);
            return 1;
        }
        if (!positional_capacity(s, pcount)) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: positional storage exhausted — refusing\n");
            free(buf);
            return 1;
        }
        if (replace_positionals(s, pvals, pcount) != 0) {
            jash_writes(jash_stderr_writer, 0,
                        "jash: set: positional storage exhausted\n");
            free(buf);
            return 1;
        }
        free(buf);
        return 0;
    }

    /* Parse options: set -e, set -u, set -ex, set -o pipefail, etc */
    int i = 0;
    int enable = 1;

    while (buf[i]) {
        /* Skip whitespace */
        if (buf[i] == ' ' || buf[i] == '\t') {
            i++;
            continue;
        }

        /* Parse enable/disable modifier */
        if (buf[i] == '-') {
            enable = 1;
            i++;
            continue;
        }

        if (buf[i] == '+') {
            enable = 0;
            i++;
            continue;
        }

        /* Now parse option character(s) or long-form */
        if (buf[i] == 'o') {
            i++;

            /* Skip optional space between -o and optname */
            while (buf[i] && (buf[i] == ' ' || buf[i] == '\t')) {
                i++;
            }

            /* Collect the option name */
            int j = 0;
            char optname[64];
            while (buf[i] && buf[i] != ' ' && buf[i] != '\t' &&
                   buf[i] != '-' && buf[i] != '+' && j < 63) {
                optname[j++] = buf[i];
                i++;
            }
            optname[j] = '\0';

            int lres = set_long_option(s, optname, enable);
            if (lres != 0) {
                free(buf);
                return lres;   /* JINV-069: 2 = recusa de noexec, 1 = desconhecida */
            }
        } else {
            /* Short option: handle composite flags like -ex */
            char opt = buf[i];
            i++;

            /* Validate and set the option */
            int res = set_short_option(s, opt, enable);
            if (res == -2) {   /* JINV-069: recusa já reportada, rc 2 */
                free(buf);
                return 2;
            }
            if (res < 0) {
                fprintf(stderr, "jash: set: unknown option: -%c\n", opt);
                free(buf);
                return 1;
            }
        }
    }

    free(buf);
    return 0;
}
