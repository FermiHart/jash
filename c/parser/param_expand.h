// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/param_expand — parameter expansion ${var...}

#ifndef JASH_PARSER_PARAM_EXPAND_H
#define JASH_PARSER_PARAM_EXPAND_H

#include <core/state.h>
#include <stddef.h>   /* JINV-087: size_t na porta de divisão de campos */

/* Include variable cache for performance optimization */
#include <core/var_cache.h>

/* Parameter expansion modes */
typedef enum {
    PARAM_BASIC,        /* ${var} */
    PARAM_DEFAULT,      /* ${var:-default} */
    PARAM_ASSIGN,       /* ${var:=default} */
    PARAM_STRIP_SHORT,  /* ${var#pattern} — remove shortest prefix */
    PARAM_STRIP_LONG,   /* ${var##pattern} — remove longest prefix */
    PARAM_STRIP_SUFFIX_SHORT,  /* ${var%pattern} — remove shortest suffix */
    PARAM_STRIP_SUFFIX_LONG,   /* ${var%%pattern} — remove longest suffix */
    PARAM_LENGTH,       /* ${#var} — length of variable */
    PARAM_UPPER,        /* ${var^} — uppercase first char */
    PARAM_LOWER,        /* ${var,} — lowercase first char */
    PARAM_UPPER_ALL,    /* ${var^^} — uppercase all chars */
    PARAM_LOWER_ALL,    /* ${var,,} — lowercase all chars */
    PARAM_SUBSTITUTE,   /* ${var/old/new} — substitute first occurrence */
    PARAM_SUBSTITUTE_ALL,   /* ${var//old/new} — substitute all occurrences */
    PARAM_SLICE,        /* ${var:start:length} — slice string */
} param_expand_mode;

/* Perform parameter expansion on a string.
 * 
 * Supports the following expansions:
 *   ${var}          - Basic expansion (returns empty placeholder)
 *   ${#var}         - Length of variable
 *   ${var#pattern}  - Remove shortest prefix matching pattern
 *   ${var##pattern} - Remove longest prefix matching pattern
 *   ${var%pattern}  - Remove shortest suffix matching pattern
 *   ${var%%pattern} - Remove longest suffix matching pattern
 *   ${var/old/new}  - Replace first occurrence of old with new
 *   ${var//old/new} - Replace all occurrences of old with new
 *   ${var:start:len} - Extract substring from start with length
 *   ${var^}         - Capitalize first character
 *   ${var^^}        - Uppercase all characters
 *   ${var,}         - Lowercase first character
 *   ${var,,}        - Lowercase all characters
 * 
 * Returns: pointer to static buffer with expanded result */
const char* jash_param_expand(const char* str, void* state);
/* Command-argument variant: IFS word-splits unquoted expansion results (BUG-010). */
const char* jash_param_expand_split(const char* str, void* state);
/* Same split/error/nesting contract; caller already found parameter expansion
 * in this exact input with jash_has_param_expansion. Skips the presence scan. */
const char* jash_param_expand_split_known(const char* str, void* state);

/* Array element count without buffer-size artifacts (BUG-065). */
int jash_array_count(jash_state_t* s, const char* base);

/* ═══ JINV-087 — a ÚNICA leitura de $IFS para separação de campos ══════════
 *
 * POSIX XCU 2.6.5: a divisão por IFS se aplica ao resultado de expansão NÃO
 * CITADA — nunca a texto literal, nunca ao que saiu de dentro de aspas. A
 * marca de "veio de expansão não citada" existe apenas NO PONTO da expansão;
 * quem divide depois, sobre a cadeia pronta, está adivinhando.
 *
 * Por isso a divisão mora aqui, junto de quem sabe: `jash_ifs_field_put`
 * recebe o byte ORIGINAL do valor (para decidir se é separador) e o byte já
 * BLINDADO pelo chamador (para emitir, quando for dado). Os dois produtores
 * de expansão a usam — param_expand (`$VAR`, `${…}`, `$@`, `$*`) e cmdsub
 * (`$(…)`, crases) — e é por isso que ela é pública em vez de estática: o
 * projeto já encontrou OITO vezes o par "duas implementações da mesma coisa,
 * uma empobrecida", e a divisão por IFS já era uma delas.
 *
 * Regras (as do POSIX, uma linha cada):
 *   - caractere de IFS não-branco  → JASH_FIELD_SEP (delimitador exato);
 *   - branco que ESTÁ no IFS       → um único espaço, corrida colapsada, e
 *                                    absorvido se colado num delimitador;
 *   - branco FORA do IFS           → DADO: sentinela de proteção quando
 *                                    `protect_ws`, senão passa como está;
 *   - qualquer outro byte          → o byte blindado do chamador.
 *
 * `protect_ws` é 0 no caminho de array (`${a[@]}` sem aspas), onde o espaço
 * é a JUNÇÃO estrutural entre elementos e não dado — protegê-lo grudaria os
 * elementos num campo só. */
void jash_ifs_field_begin(void* state, int protect_ws);
void jash_ifs_field_put(char orig, char shielded, char* out, size_t cap, size_t* o);
void jash_ifs_field_flush(char* out, size_t cap, size_t* o);
/* 1 quando há divisão de campo ATIVA (o produtor deve emitir pela porta acima). */
int  jash_ifs_field_active(void);
void jash_ifs_field_end(void);

/* A marca de PALAVRA (JASH_EXP_MARK, veja cmdsub.h). `_wanted` diz se vale a
 * pena emiti-la — é 0 com IFS branco, e aí NADA muda no texto expandido.
 * `_is_hard_sep` é a mesma leitura de `$IFS` respondendo por byte, para que a
 * varredura de palavras do `for` não precise reler a variável (seria a segunda
 * implementação da mesma coisa; este projeto já achou oito). */
void jash_ifs_word_mark_enable(int on);
int  jash_ifs_word_mark_wanted(void);
int  jash_ifs_is_hard_sep(char c);
/* O estado CRU do interruptor, para salvar/restaurar em reentrância: um `for`
 * dentro de um `$( )` dentro da lista de palavras de outro `for` reentra em
 * tudo isto e deixaria o de fora desarmado. */
int  jash_ifs_word_mark_state(void);

/* Variante do for/select: divide o resultado de expansão NÃO CITADA em campos
 * marcados com JASH_FIELD_SEP, protegendo o branco que não está em $IFS.
 * (JINV-087; a irmã `_split` acima é a do argv, que marca com espaço.) */
const char* jash_param_expand_split_fields(const char* str, void* state);

/* Last parameter-expansion error, e.g. ${var?message}. */
int jash_param_expand_error(void);
const char* jash_param_expand_error_msg(void);
void jash_param_expand_clear_error(void);

/* Check if string contains ${...} pattern */
int jash_has_param_expansion(const char* str);

#endif
