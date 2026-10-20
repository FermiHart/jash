// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtins: readonly, declare — variable attributes

#include "builtin.h"
#include "shell_output.h"
#include <core/env.h>
#include <string.h>
#include <stdlib.h>   /* JINV-086: malloc/free da linha de argumentos sem teto */

/* Global readonly set — simple bitmap for demo */
#define READONLY_MAX 64
static char readonly_names[READONLY_MAX][64];
static int readonly_count = 0;

int jash_readonly_is_set(const char *name) {
    int i;
    for (i = 0; i < readonly_count; i++)
        if (strcmp(readonly_names[i], name) == 0) return 1;
        else {
            size_t n = strlen(readonly_names[i]);
            if (!strncmp(readonly_names[i], name, n) && name[n] == '[') return 1;
        }
    return 0;
}

void jash_readonly_mark(const char *name) {
    if (!name || !*name || jash_readonly_is_set(name)) return;
    if (readonly_count < READONLY_MAX) {
        strncpy(readonly_names[readonly_count], name, 63);
        readonly_names[readonly_count][63] = 0;
        readonly_count++;
    }
}

size_t jash_readonly_count(void) { return (size_t)readonly_count; }
const char* jash_readonly_at(size_t index) {
    return index < (size_t)readonly_count ? readonly_names[index] : NULL;
}
int jash_readonly_mark_checked(const char* name) {
    if (!jash_readonly_can_mark(name)) return -1;
    if (jash_readonly_is_set(name)) return 0;
    if (readonly_count == READONLY_MAX) return -1;
    memcpy(readonly_names[readonly_count++], name, strlen(name) + 1);
    return 0;
}

int jash_readonly_can_mark(const char* name) {
    return name && *name && strlen(name) < sizeof(readonly_names[0]) &&
           (jash_readonly_is_set(name) || readonly_count < READONLY_MAX);
}

int jash_b_readonly(jash_state_t *s, jash_writer_t w, void *ctx,
                    const char *args) {
    int structured = jash_shell_state_try(s, w, ctx, "readonly", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    if (!args || !*args) {
        /* List readonly vars */
        int i;
        for (i = 0; i < readonly_count; i++) {
            jash_writes(w, ctx, "readonly ");
            jash_writes(w, ctx, readonly_names[i]);
            jash_writes(w, ctx, "\n");
        }
        return 0;
    }
    /* ═══ JINV-086 — `readonly V=…` cortava o valor em 254 bytes ═══════════
     *
     * Dois tetos mudos em série: `char buf[512]` (a linha INTEIRA de
     * argumentos, cortada em 510) e `char val[256]` (o valor, cortado em
     * 254). Medido contra o bash:
     *
     *     readonly V=<3000 a's>; echo ${#V}   jash 254 · bash 3000
     *
     * rc 0, stderr vazio. E `readonly` é, das três formas de atribuir, a que
     * pior perdoa o erro: a variável fica cortada E imutável, então o valor
     * errado não pode nem ser reposto depois.
     *
     * Conserto: `buf` do tamanho EXATO da linha (nunca estoura, é uma cópia),
     * com a pilha como caminho rápido; o valor vai direto de `eq + 1` para
     * `jash_env_set`, que já guarda no heap sem teto (`heap_val`, core/state.h)
     * — o `val[256]` intermediário deixa de existir.
     *
     * Gate: tests/test_jinv086_silent_caps.sh */
    char  buf_inline[512];
    char* buf_heap = NULL;
    char* buf = buf_inline;
    size_t alen = strlen(args);
    if (alen + 1 > sizeof(buf_inline)) {
        buf_heap = (char*)malloc(alen + 1);
        if (!buf_heap) {
            jash_writes(w, ctx, "jash: readonly: sem memoria — nada foi definido\n");
            return 2;
        }
        buf = buf_heap;
    }
    memcpy(buf, args, alen + 1);
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *eq = 0;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '=' && !eq) eq = p;
            p++;
        }
        char save = *p; *p = 0;
        int namelen = eq ? (int)(eq - start) : (int)(p - start);
        if (namelen > 0 && namelen < 64) {
            char name[64];
            int ni;
            for (ni = 0; ni < namelen; ni++) name[ni] = start[ni];
            name[namelen] = 0;
            /* Check not already readonly */
            if (!jash_readonly_is_set(name)) {
                if (readonly_count < READONLY_MAX) {
                    strlcpy(readonly_names[readonly_count], name, sizeof(readonly_names[readonly_count]));
                    readonly_count++;
                }
                if (eq) {
                    /* JINV-086: aqui morava `char val[256]` com corte em 254.
                     * `eq + 1` já é NUL-terminado (o token foi fechado acima
                     * com `*p = 0`), e `jash_env_set` copia para o heap sem
                     * teto — o intermediário só existia para truncar. */
                    jash_env_set(s, name, eq + 1);
                }
            }
        }
        *p = save;
        if (!eq && save) p++;
    }
    if (buf_heap) free(buf_heap);   /* JINV-086 */
    return 0;
}
