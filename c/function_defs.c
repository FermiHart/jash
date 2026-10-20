// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: function_defs — user-defined functions, function registry, return

#include "function_defs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* jash_function and jash_function_table are now defined in the header
 * (made public so callers can embed a table by value). */

void jash_function_table_init(jash_function_table* table) {
    if (!table) return;
    memset(table, 0, sizeof(*table));   /* JINV-086: zera todo `body_heap` */
}

/* JINV-086 — grava `body` na entrada `f` sem NENHUM teto.
 *
 * Até JASH_FN_BODY_INLINE-1 bytes o corpo fica inline (caso comum, zero
 * `malloc`, o mesmo perfil de memória de antes deste conserto). Acima disso,
 * heap do tamanho EXATO. Devolve 0 sem memória — e aí quem chama RECUSA, não
 * grava pela metade: a entrada só é publicada depois que o corpo inteiro
 * coube. */
static int jash_function_store_body(jash_function* f, const char* body) {
    size_t blen = strlen(body);
    if (blen < sizeof(f->body)) {
        memcpy(f->body, body, blen + 1);
        if (f->body_heap) { free(f->body_heap); f->body_heap = NULL; }
        return 1;
    }
    char* n = (char*)malloc(blen + 1);
    if (!n) return 0;                   /* recusa alta — nada é alterado */
    memcpy(n, body, blen + 1);
    if (f->body_heap) free(f->body_heap);
    f->body_heap = n;
    f->body[0] = 0;                     /* o corpo vive no heap agora */
    return 1;
}

int jash_function_define(jash_function_table* table, const char* name, const char* body) {
    if (!table || !name || !body) return JASH_FUNC_EFULL;
    if (table->count >= 32) return JASH_FUNC_EFULL;  /* Table full */

    /* Check if function already exists */
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->funcs[i].name, name) == 0) {
            /* Redefine: replace body. JINV-086 — sem teto, e o corpo antigo
             * (se estava no heap) é liberado por `store_body`. */
            if (!jash_function_store_body(&table->funcs[i], body))
                return JASH_FUNC_ENOMEM;
            return JASH_FUNC_OK;
        }
    }

    /* New function */
    jash_function* func = &table->funcs[table->count];
    /* JINV-086: pela invariante de posse, todo slot >= count tem
     * body_heap == NULL, então `store_body` aqui nunca libera nada vivo. */
    if (!jash_function_store_body(func, body))
        return JASH_FUNC_ENOMEM;        /* count NÃO avança: nada foi publicado */
    strlcpy(func->name, name, sizeof(func->name));
    func->name[sizeof(func->name) - 1] = 0;

    table->count++;
    return JASH_FUNC_OK;
}

const char* jash_function_lookup(jash_function_table* table, const char* name) {
    if (!table || !name) return NULL;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->funcs[i].name, name) == 0) {
            return jash_function_body_of(&table->funcs[i]);   /* JINV-086 */
        }
    }
    return NULL;
}

int jash_function_exists(jash_function_table* table, const char* name) {
    return jash_function_lookup(table, name) != NULL;
}

void jash_function_list(jash_function_table* table) {
    if (!table || table->count == 0) {
        return;
    }

    for (int i = 0; i < table->count; i++) {
        printf("%s() { ... }\n", table->funcs[i].name);
    }
}

void jash_function_delete(jash_function_table* table, const char* name) {
    if (!table || !name) return;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->funcs[i].name, name) == 0) {
            /* JINV-086: o corpo desta entrada some agora — se estava no heap,
             * é liberado ANTES do deslocamento, senão o ponteiro é sobrescrito
             * e vaza. */
            if (table->funcs[i].body_heap) {
                free(table->funcs[i].body_heap);
                table->funcs[i].body_heap = NULL;
            }
            /* Shift remaining functions down */
            for (int j = i; j < table->count - 1; j++) {
                table->funcs[j] = table->funcs[j + 1];
            }
            table->count--;
            /* JINV-086: o slot vago carrega uma CÓPIA do ponteiro que agora
             * pertence ao slot anterior. Zerá-lo mantém a invariante de posse
             * (slot >= count nunca tem `body_heap` vivo) — sem isto, definir
             * uma função nova liberaria um corpo em uso. */
            memset(&table->funcs[table->count], 0,
                   sizeof(table->funcs[table->count]));
            return;
        }
    }
}
