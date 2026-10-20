// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/echo — print arguments with -n and -e support.

#include "builtin.h"
#include "value_output.h"
#include <string.h>     /* strlen */

/* Write a string interpreting backslash escapes (bash echo -e). */
static void write_escaped(jash_writer_t w, void* ctx, const char* s) {
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
            case 'n':  { char c = '\n'; w(ctx, &c, 1); break; }
            case 't':  { char c = '\t'; w(ctx, &c, 1); break; }
            case 'r':  { char c = '\r'; w(ctx, &c, 1); break; }
            case 'a':  { char c = '\a'; w(ctx, &c, 1); break; }
            case 'b':  { char c = '\b'; w(ctx, &c, 1); break; }
            case 'f':  { char c = '\f'; w(ctx, &c, 1); break; }
            case 'v':  { char c = '\v'; w(ctx, &c, 1); break; }
            case '\\': { char c = '\\'; w(ctx, &c, 1); break; }
            case '0': {
                /* Octal: \0NNN */
                int val = 0;
                s++;
                for (int i = 0; i < 3 && *s >= '0' && *s <= '7'; i++, s++)
                    val = val * 8 + (*s - '0');
                char c = (char)val;
                w(ctx, &c, 1);
                continue; /* don't advance s again */
            }
            case 'x': {
                /* Hex: \xHH */
                int val = 0;
                s++;
                for (int i = 0; i < 2; i++, s++) {
                    if (*s >= '0' && *s <= '9') val = val * 16 + (*s - '0');
                    else if (*s >= 'a' && *s <= 'f') val = val * 16 + (*s - 'a' + 10);
                    else if (*s >= 'A' && *s <= 'F') val = val * 16 + (*s - 'A' + 10);
                    else break;
                }
                char c = (char)val;
                w(ctx, &c, 1);
                continue;
            }
            default: {
                /* Unknown escape: print as-is */
                w(ctx, s, 1);
                break;
            }
            }
            s++;
        } else if (*s == '\\') {
            /* JINV-079 — barra invertida no FIM da string: nao ha byte
             * seguinte para escapar, entao ela e DADO (bash imprime `\`).
             *
             * Este ramo nao existia, e a ausencia dele era um TRAVAMENTO, nao
             * um erro de saida: o `else` de baixo varria "ate a proxima
             * barra" e a barra era o proprio `*s`, entao a volta terminava
             * exatamente onde comecou e o `while (*s)` girava para sempre.
             * `jash -c 'echo -e "\\"'` prendia o shell sem mensagem, sem rc,
             * sem rastro — a classe JINV-062 (laco sem garantia de avanco),
             * aqui alcancavel por uma linha de usuario. */
            w(ctx, s, 1);
            s++;
        } else {
            /* Find run of non-backslash chars */
            const char* start = s;
            while (*s && *s != '\\') s++;
            if (s > start) w(ctx, start, (size_t)(s - start));
        }
    }
}

/* JINV-079 — uma palavra de flag e `-` seguido SO de n/e/E, e nada mais.
 *
 * Devolve 1 e aplica os efeitos em newline e escape; 0 = a palavra e OPERANDO
 * e o reconhecimento de flags acaba aqui (bash: `echo -nx hi` imprime
 * `-nx hi`, nao `x hi` — uma letra desconhecida invalida a palavra INTEIRA,
 * nao so ela mesma). `-` e `--` sozinhos tambem sao operandos.
 *
 * Tambem reconhece `--json` como flag de saida estruturada (ADR-0044 Arc B3). */
static int echo_flag_word(const char* a, int* newline, int* escape, int* format) {
    if (!a || a[0] != '-' || a[1] == '\0') return 0;
    for (const char* p = a + 1; *p; p++)
        if (*p != 'n' && *p != 'e' && *p != 'E') {
            int selected = !strcmp(a, "--json") ? 1
                : (!strcmp(a, "--data") || !strcmp(a, "--data-v2")) ? 2 : 0;
            if (selected) { if (*format && *format != selected) return -1; *format = selected; return 1; }
            return 0;
        }
    /* Esquerda para a direita: em `-eE` o `E` desliga o que o `e` ligou. */
    for (const char* p = a + 1; *p; p++) {
        if (*p == 'n')      *newline = 0;
        else if (*p == 'e') *escape  = 1;
        else                *escape  = 0;   /* 'E' */
    }
    return 1;
}

/* JINV-079 — a porta VALIDA do `echo`: a saida se monta a partir do ARGV.
 *
 * ═══ O QUE ESTAVA ERRADO ═══
 *
 * `jash_b_echo` (abaixo, mantido como fallback do dispatcher) recebe o RESTO
 * DA LINHA — uma string so, ja expandida, com as aspas removidas e as
 * fronteiras de palavra dissolvidas. Nesse ponto `echo "  x"` e `echo   x`
 * sao literalmente a mesma sequencia de bytes, e nenhuma quantidade de
 * cuidado dentro do builtin consegue distingui-las. Dai as duas metades do
 * defeito, uma espelho da outra:
 *
 *   · o `while (*args == ' ')` cortava o branco da ESQUERDA as cegas, entao
 *     `echo " "` imprimia so `\n` e `echo "  x  "` perdia o comeco;
 *   · o branco INTERNO saia como estava, entao `echo A     B` imprimia cinco
 *     espacos onde o bash imprime um.
 *
 * O bash nunca "corta branco": ele JUNTA argv[1..n] com UM espaco. O branco
 * entre palavras nunca chegou a existir para ele — e estrutura consumida pelo
 * tokenizador. Aqui e igual: quem separa e `jash_argv_build` (core/argv.c,
 * ADR-0036 W2), que conhece aspas; este builtin so junta.
 *
 * ═══ PAR SIMETRICO DO JINV-063 ═══
 *
 * O JINV-063 matou o lado DIREITO (o separador final da linha, em
 * `jash_rtrim_unquoted`) e deixou o esquerdo de proposito, para nao misturar
 * duas cirurgias no mesmo caminho quente. Esta e a outra metade.
 *
 * ═══ POR QUE A PORTA DE STRING FICA ═══
 *
 * Ela continua registrada como `fn` e continua sendo o caminho de qualquer
 * chamador que ainda nao passe pelo `jash_builtin_dispatch`. Nao se apaga um
 * caminho que ainda pode ser andado — acrescenta-se o certo ao lado e
 * aponta-se o despacho para ele (mesmo padrao do `test`/`[`, JINV-066).
 *
 * Catraca: tests/argv_migrated.txt + tests/test_argv_ratchet.sh.
 * Gate: tests/test_jinv079_echo_argv.sh (oraculo /bin/bash em toda assercao).
 */
int jash_b_echo_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                     int argc, char** argv) {
    (void)s;
    int newline = 1;
    int escape = 0;
    int format = 0;

    int i = 0;
    while (i < argc) {
        int flag = echo_flag_word(argv[i], &newline, &escape, &format);
        if (flag < 0) {
            jash_writes(jash_stderr_writer, NULL, "echo: conflicting output formats\n");
            return 2;
        }
        if (!flag) break;
        ++i;
    }

    if (format) {
        /* Structured echo represents argv, not the escaped/joined text view.
         * This preserves the established --json semantics of -e/-n. */
        size_t count = (size_t)(argc - i), bytes = 0;
        if (count > 65536u) return 2;
        jdv_value_t* values = count ? calloc(count, sizeof(*values)) : NULL;
        if (count && !values) return 125;
        for (size_t n = 0; n < count; n++) {
            const char* text = argv[i + (int)n];
            size_t length = strlen(text);
            if (length > 2u * 1024u * 1024u - bytes) { free(values); return 2; }
            bytes += length;
            values[n].kind = jash_json_utf8_valid_n(text, length) ? JDV_STRING : JDV_BYTES;
            values[n].as.slice = (jdv_slice_t){(const uint8_t*)text, length};
        }
        jdv_value_t list = {.kind = JDV_LIST, .as.list = {values, count}};
        jdv_limits_t limits = JDV_DEFAULT_LIMITS;
        limits.max_aggregate_bytes = 2u * 1024u * 1024u;
        limits.max_encoded_bytes = 4u * 1024u * 1024u;
        limits.max_nodes = 65537u; limits.max_list_items = 65536u;
        jdv_schema_t element = {.kind = JDV_SCHEMA_ANY};
        jdv_schema_t schema = {.kind = JDV_SCHEMA_LIST, .as.element = &element};
        uint64_t credit = bytes;
        int rc = format == 1 ? value_output_json(w, ctx, "echo", &list, &limits, 8u * 1024u * 1024u, 0)
            : value_output_data(w, ctx, &list, 1, &credit, &schema, &limits, 8u * 1024u * 1024u, 0, NULL);
        free(values);
        return rc;
    }

    for (int first = 1; i < argc; i++, first = 0) {
        if (!first) { char sp = ' '; w(ctx, &sp, 1); }
        if (escape) write_escaped(w, ctx, argv[i]);
        else        jash_writes(w, ctx, argv[i]);
    }

    if (newline) jash_writes(w, ctx, "\n");
    return 0;
}

/* Avaliador de STRING legado — preservado de proposito (JINV-079).
 *
 * NAO e mais o caminho do despacho: `jash_builtin_dispatch` prefere o
 * `argv_fn` acima. Fica como fallback de qualquer chamador que entregue uma
 * linha crua, e como o registro historico do que a porta de string conseguia
 * ver (nao as fronteiras de palavra — e por isso que ela erra `echo " "`).
 * Nada aqui foi apagado; o certo foi acrescentado ao lado. */
int jash_b_echo(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    int newline = 1;
    int escape = 0;
    if (args) {
        while (*args == ' ' || *args == '\t') args++;
        /* Parse flags: -n, -e, -E, -ne, -en, -neE, etc. */
        while (args[0] == '-' && args[1] && args[1] != ' ' && args[1] != '\t') {
            int consumed = 0;
            const char* p = args + 1;
            while (*p && *p != ' ' && *p != '\t') {
                if (*p == 'n') { newline = 0; consumed = 1; }
                else if (*p == 'e') { escape = 1; consumed = 1; }
                else if (*p == 'E') { escape = 0; consumed = 1; }
                else break; /* unknown flag char, stop */
                p++;
            }
            if (!consumed) break;
            args = p;
            while (*args == ' ' || *args == '\t') args++;
        }
        if (escape) {
            write_escaped(w, ctx, args);
        } else {
            /* Replace \x1E (array word-boundary sentinel) with space so that
             * `echo "${arr[@]}"` prints elements separated by spaces rather
             * than the raw RS control byte.  All other bytes pass through. */
            const char* p = args;
            const char* seg = p;
            while (*p) {
                if ((unsigned char)*p == 0x1E) {
                    if (p > seg) w(ctx, seg, (size_t)(p - seg));
                    { char sp = ' '; w(ctx, &sp, 1); }
                    seg = p + 1;
                }
                p++;
            }
            if (p > seg) w(ctx, seg, (size_t)(p - seg));
        }
    }
    if (newline) jash_writes(w, ctx, "\n");
    return 0;
}
