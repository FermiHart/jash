// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_narrator.c — Zero-LLM Narration Engine implementation
// Template-based narration with anti-repeat ring for varied output.
// F E R M I ∞ H A R T

#include "jash_narrator.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ── Mode ─────────────────────────────────────────────────────────────────────

static jash_narrator_mode_t g_mode = NARRATOR_NORMAL;

void jash_narrator_init(void) {
    g_mode = NARRATOR_NORMAL;
}

void jash_narrator_set_mode(jash_narrator_mode_t mode) {
    g_mode = mode;
}

jash_narrator_mode_t jash_narrator_get_mode(void) {
    return g_mode;
}

// ── Anti-Repeat Ring ─────────────────────────────────────────────────────────

#define ANTI_REPEAT_RING  8
#define PHRASE_HISTORY    4

static int  g_ring_index[ANTI_REPEAT_RING];   // which op index last used
static int  g_ring_phrase[ANTI_REPEAT_RING];  // which phrase variant used
static int  g_ring_count = 0;

void jash_narrator_reset(void) {
    memset(g_ring_index, 0, sizeof(g_ring_index));
    memset(g_ring_phrase, 0, sizeof(g_ring_phrase));
    g_ring_count = 0;
}

// Return a phrase variant index, avoiding the last-used variant.
static int pick_variant(int op_idx, int variant_count) {
    if (variant_count <= 1) return 0;

    // Find the last variant used for this op
    int last_variant = -1;
    for (int i = g_ring_count - 1; i >= 0; i--) {
        if (g_ring_index[i] == op_idx) {
            last_variant = g_ring_phrase[i];
            break;
        }
    }

    // Pick a different variant
    int pick = rand() % variant_count;
    if (last_variant >= 0 && pick == last_variant && variant_count > 1) {
        pick = (pick + 1) % variant_count;
    }

    // Record in ring
    if (g_ring_count < ANTI_REPEAT_RING) {
        g_ring_index[g_ring_count] = op_idx;
        g_ring_phrase[g_ring_count] = pick;
        g_ring_count++;
    }

    return pick;
}

// ── Phrase Tables ────────────────────────────────────────────────────────────

// Returns (static_string, variant_count)
typedef struct {
    const char* phrases[6];
    int count;
} phrase_set_t;

static phrase_set_t get_phrases(jash_narrator_op_t op) {
    phrase_set_t ps = {{NULL}, 0};

    switch (op) {
        case NARR_OP_BUILD_OK:
            ps.phrases[0] = "build ok";
            ps.phrases[1] = "compilou limpo";
            ps.phrases[2] = "build passou";
            ps.count = 3;
            break;
        case NARR_OP_BUILD_FAIL:
            ps.phrases[0] = "build falhou";
            ps.phrases[1] = "erro de compilação";
            ps.phrases[2] = "build quebrou";
            ps.count = 3;
            break;
        case NARR_OP_TEST_OK:
            ps.phrases[0] = "testes passaram";
            ps.phrases[1] = "tudo verde";
            ps.phrases[2] = "testes ok";
            ps.count = 3;
            break;
        case NARR_OP_TEST_FAIL:
            ps.phrases[0] = "testes falharam";
            ps.phrases[1] = "teste quebrou";
            ps.count = 2;
            break;
        case NARR_OP_CREATE_FILE:
            ps.phrases[0] = "criado";
            ps.phrases[1] = "arquivo criado";
            ps.phrases[2] = "criou";
            ps.count = 3;
            break;
        case NARR_OP_MODIFY_FILE:
            ps.phrases[0] = "modificado";
            ps.phrases[1] = "arquivo alterado";
            ps.phrases[2] = "editado";
            ps.count = 3;
            break;
        case NARR_OP_DELETE_FILE:
            ps.phrases[0] = "removido";
            ps.phrases[1] = "arquivo deletado";
            ps.count = 2;
            break;
        case NARR_OP_MOVE_FILE:
            ps.phrases[0] = "movido";
            ps.phrases[1] = "renomeado";
            ps.count = 2;
            break;
        case NARR_OP_GIT_COMMIT:
            ps.phrases[0] = "commit feito";
            ps.phrases[1] = "committed";
            ps.count = 2;
            break;
        case NARR_OP_GIT_PUSH:
            ps.phrases[0] = "enviado";
            ps.phrases[1] = "push feito";
            ps.count = 2;
            break;
        case NARR_OP_GIT_PULL:
            ps.phrases[0] = "atualizado";
            ps.phrases[1] = "pull feito";
            ps.count = 2;
            break;
        case NARR_OP_INSTALL:
            ps.phrases[0] = "instalado";
            ps.count = 1;
            break;
        case NARR_OP_REMOVE:
            ps.phrases[0] = "removido";
            ps.count = 1;
            break;
        case NARR_OP_DEPLOY:
            ps.phrases[0] = "deploy feito";
            ps.phrases[1] = "publicado";
            ps.count = 2;
            break;
        case NARR_OP_SEARCH:
            ps.phrases[0] = "encontrado";
            ps.phrases[1] = "resultados";
            ps.count = 2;
            break;
        case NARR_OP_LEARN:
            ps.phrases[0] = "aprendido";
            ps.phrases[1] = "memorizado";
            ps.phrases[2] = "guardado";
            ps.count = 3;
            break;
        case NARR_OP_RECALL:
            ps.phrases[0] = "lembrado";
            ps.phrases[1] = "recuperado";
            ps.count = 2;
            break;
        case NARR_OP_APPROVE:
            ps.phrases[0] = "aprovado";
            ps.phrases[1] = "ok";
            ps.phrases[2] = "confirmado";
            ps.count = 3;
            break;
        case NARR_OP_REJECT:
            ps.phrases[0] = "rejeitado";
            ps.phrases[1] = "cancelado";
            ps.count = 2;
            break;
        case NARR_OP_TIMEOUT:
            ps.phrases[0] = "timeout";
            ps.phrases[1] = "demorou demais";
            ps.count = 2;
            break;
        case NARR_OP_ERROR:
            ps.phrases[0] = "erro";
            ps.phrases[1] = "falha";
            ps.count = 2;
            break;
        case NARR_OP_START:
            ps.phrases[0] = "iniciando";
            ps.phrases[1] = "começando";
            ps.count = 2;
            break;
        case NARR_OP_DONE:
            ps.phrases[0] = "pronto";
            ps.phrases[1] = "feito";
            ps.phrases[2] = "concluído";
            ps.count = 3;
            break;
        case NARR_OP_WAIT:
            ps.phrases[0] = "aguardando";
            ps.phrases[1] = "esperando";
            ps.count = 2;
            break;
        case NARR_OP_CONNECT:
            ps.phrases[0] = "conectado";
            ps.phrases[1] = "online";
            ps.count = 2;
            break;
        case NARR_OP_DISCONNECT:
            ps.phrases[0] = "desconectado";
            ps.count = 1;
            break;
        case NARR_OP_SYNC:
            ps.phrases[0] = "sincronizado";
            ps.phrases[1] = "sync feito";
            ps.count = 2;
            break;
        default:
            ps.phrases[0] = "ok";
            ps.count = 1;
            break;
    }

    return ps;
}

// ── Public API ───────────────────────────────────────────────────────────────

int jash_narrate(jash_narrator_op_t op, const char* detail, char* buf, size_t cap) {
    if (!buf || cap == 0) return 0;

    if (g_mode == NARRATOR_SILENT) {
        buf[0] = '\0';
        return 0;
    }

    phrase_set_t ps = get_phrases(op);
    int variant = pick_variant((int)op, ps.count);
    const char* phrase = ps.phrases[variant];
    if (!phrase) return 0;

    if (g_mode == NARRATOR_MINIMAL) {
        int n = snprintf(buf, cap, "%s", phrase);
        if (n < 0) return 0;
        return (size_t)n >= cap ? (int)(cap - 1) : n;
    }

    if (detail && detail[0]) {
        int n = snprintf(buf, cap, "%s: %s", phrase, detail);
        if (n < 0) return 0;
        return (size_t)n >= cap ? (int)(cap - 1) : n;
    }

    int n = snprintf(buf, cap, "%s.", phrase);
    if (n < 0) return 0;
    return (size_t)n >= cap ? (int)(cap - 1) : n;
}

const char* jash_narrator_phrase(jash_narrator_op_t op) {
    // For quick status bar use — returns a static string (no detail, no anti-repeat)
    switch (op) {
        case NARR_OP_BUILD_OK:     return "✔ build";
        case NARR_OP_BUILD_FAIL:   return "✘ build";
        case NARR_OP_TEST_OK:      return "✔ tests";
        case NARR_OP_TEST_FAIL:    return "✘ tests";
        case NARR_OP_CREATE_FILE:  return "+file";
        case NARR_OP_MODIFY_FILE:  return "~file";
        case NARR_OP_DELETE_FILE:  return "-file";
        case NARR_OP_MOVE_FILE:    return ">file";
        case NARR_OP_GIT_COMMIT:   return "commit";
        case NARR_OP_GIT_PUSH:     return "push";
        case NARR_OP_GIT_PULL:     return "pull";
        case NARR_OP_INSTALL:      return "+pkg";
        case NARR_OP_DEPLOY:       return "deploy";
        case NARR_OP_SEARCH:       return "search";
        case NARR_OP_LEARN:        return "learn";
        case NARR_OP_RECALL:       return "recall";
        case NARR_OP_APPROVE:      return "✓ ok";
        case NARR_OP_REJECT:       return "✗ no";
        case NARR_OP_TIMEOUT:      return "⌛";
        case NARR_OP_ERROR:        return "⚠";
        case NARR_OP_DONE:         return "done";
        case NARR_OP_WAIT:         return "⋯";
        case NARR_OP_CONNECT:      return "↑";
        case NARR_OP_DISCONNECT:   return "↓";
        case NARR_OP_SYNC:         return "↻";
        default:                   return "·";
    }
}
