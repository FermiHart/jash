// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: parser/select_loop — select VAR in WORDS; do BODY; done support.

#include "script.h"
#include "eval.h"
#include "quotes.h"   /* JINV-116: scanner único, ciente de aspas */
#include <core/env.h>
#include <core/readbuf.h>
#include <sys/syscalls.h>
#include <string.h>
#include <ctype.h>

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

static int parse_int(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

static void write_str(jash_writer_t w, const char* s) {
    w(0, s, strlen(s));
}

static void write_num(jash_writer_t w, int n) {
    char buf[16];
    int len = 0;
    char tmp[16];
    int t = 0;
    if (n == 0) tmp[t++] = '0';
    while (n > 0 && t < (int)sizeof(tmp)) {
        tmp[t++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (t > 0 && len < (int)sizeof(buf) - 1) buf[len++] = tmp[--t];
    buf[len] = '\0';
    write_str(w, buf);
}

int jash_eval_select_loop(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* line) {
    const char* p = skip_ws(line);
    if (strncmp(p, "select", 6) != 0 || !isspace((unsigned char)p[6])) return -1;

    p = skip_ws(p + 6);
    const char* var_start = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    size_t var_len = (size_t)(p - var_start);
    if (var_len == 0 || var_len >= 64) {
        jash_writes(w, ctx, "jash: syntax error: select requires a variable name\n");
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    char varname[64];
    memcpy(varname, var_start, var_len);
    varname[var_len] = '\0';

    p = skip_ws(p);
    if (strncmp(p, "in", 2) != 0 || !isspace((unsigned char)p[2])) {
        jash_writes(w, ctx, "jash: syntax error: select requires 'in' keyword\n");
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }
    p = skip_ws(p + 2);

    const char* words_start = p;
    /* BUG-059/bash parity: newline also ends the word list.
     * JINV-116: aspas mandam — `select x in "a;b" c` não termina no `;` citado. */
    p = jash_scan_unquoted(p, ";\n");
    if (!*p) {
        jash_writes(w, ctx, "jash: syntax error: unexpected end of select statement (missing ;)\n");
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    size_t words_len = (size_t)(p - words_start);
    char words[512];
    if (words_len >= sizeof(words)) words_len = sizeof(words) - 1;
    memcpy(words, words_start, words_len);
    words[words_len] = '\0';

    p = skip_ws(p + 1);
    if (strncmp(p, "do", 2) != 0 || !isspace((unsigned char)p[2])) {
        jash_writes(w, ctx, "jash: syntax error: select requires 'do' keyword\n");
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }

    p = skip_ws(p + 2);
    const char* body_start = p;
    /* JINV-083 — a QUARTA copia da varredura, e a mais pobre das quatro:
     * `while (*p && strncmp(p,"done",4) != 0) p++;` nao exigia fronteira de
     * palavra de NENHUM lado nem sabia o que e aspa, entao `echo undone` e
     * `echo "all done here"` fechavam o laco no meio da palavra, e nenhum
     * aninhamento era contado. Agora e a mesma leitura de todos os outros
     * lacos, com profundidade: `jash_find_loop_close` (script.c). */
    const char* done_at = jash_find_loop_close(p);
    if (!done_at) {
        jash_writes(w, ctx, "jash: syntax error: unexpected end of select statement (missing done)\n");
        { if (s && !s->interactive) s->exit_requested = 1; return 2; }
    }
    p = done_at;

    size_t body_len = (size_t)(p - body_start);
    char body[1024];
    if (body_len >= sizeof(body)) body_len = sizeof(body) - 1;
    memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    char items[32][128];
    int item_count = 0;
    const char* wp = words;
    while (*wp && item_count < 32) {
        wp = skip_ws(wp);
        if (!*wp) break;
        const char* start = wp;
        while (*wp && !isspace((unsigned char)*wp)) wp++;
        size_t len = (size_t)(wp - start);
        if (len >= sizeof(items[0])) len = sizeof(items[0]) - 1;
        memcpy(items[item_count], start, len);
        items[item_count][len] = '\0';
        item_count++;
    }

    for (int i = 0; i < item_count; i++) {
        write_num(w, i + 1);
        write_str(w, ") ");
        write_str(w, items[i]);
        write_str(w, "\n");
    }
    write_str(w, "#? ");

    char reply[32];
    unsigned ri = 0;
    {
        jash_readbuf_t rb;
        jash_readbuf_init(&rb, 0);
        while (ri < sizeof(reply) - 1) {
            char c;
            int rc = jash_readbuf_getc(&rb, &c);
            if (rc <= 0) break;
            if (c == '\n') break;
            reply[ri++] = c;
        }
    }
    reply[ri] = '\0';

    jash_env_set(s, "REPLY", reply);
    int choice = parse_int(reply);
    if (choice >= 1 && choice <= item_count) jash_env_set(s, varname, items[choice - 1]);
    else jash_env_set(s, varname, "");

    int rc = jash_eval_line(s, w, ctx, body);
    /* BUG-012: return/exit bubbles up (leave the flag set for the boundary). */
    if (s->return_requested || s->exit_requested) return rc;
    /* Handle break (255) and continue (254) like while/for loops do.
     * JINV-073: exige tambem a marca fora de banda (state.h) — um comando que
     * legitimamente sai com 255 dentro do corpo do `select` nao e um `break`,
     * e nao pode ter o status trocado por 0. */
    if (jash_loopctl_is_break(s, rc))    { jash_loopctl_clear(s); return 0; }
    if (jash_loopctl_is_continue(s, rc)) { jash_loopctl_clear(s); return 0; }
    return rc;
}
