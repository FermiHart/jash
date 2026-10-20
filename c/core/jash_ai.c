/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* jash_ai.c — AI state stub + routing
 * F E R M I  ∞  H A R T  ·  contact@fermihart.com
 *
 * Keeps AI flags resident in core (statusbar reads them at every frame).
 * Full AI agent (Groq queries, context expansion, command execution):
 *   modules/ai/ai.modjash  — loaded with `? on`, unloaded with `? off`
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jash_ai.h"
#include "ai_observation.h"
#include <core/timeline.h>
#include <core/guard.h>
#include <core/jash_qa.h>
#include <core/jash_fastpath.h>
#include <module/blake3.h>
#include <module/jash_crypto.h>
#include <module/loader.h>
#include <module/sigil.h>
#include <theme/io.h>

/* ── Flags (core-resident; statusbar reads via exported accessors) ────────── */

static int g_ai_mode  = 0;
static int g_ai_auto  = 0;
static int g_ai_learn = 0;
static int g_ai_busy  = 0;
static int g_ai_debug = 0;

void jash_ai_init(void)       { /* flags default to 0 */ }

int  jash_ai_active(void)     { return g_ai_mode; }
int  jash_ai_auto(void)       { return g_ai_auto; }
int  jash_ai_learn(void)      { return g_ai_learn; }
int  jash_ai_busy(void)       { return g_ai_busy; }
int  jash_ai_debug(void)      { return g_ai_debug; }

void jash_ai_set_mode(int v)  { g_ai_mode  = v ? 1 : 0; }
void jash_ai_set_auto(int v)  { g_ai_auto  = v ? 1 : 0; if (v) g_ai_mode = 1; }
void jash_ai_set_learn(int v) { g_ai_learn = v ? 1 : 0; if (v) g_ai_mode = 1; }
void jash_ai_set_debug(int v) { g_ai_debug = v ? 1 : 0; }
void jash_ai_set_busy(int v)  { g_ai_busy  = v ? 1 : 0; }

int jash_ai_auto_begin(char* checkpoint, size_t capacity, int* depth,
                       int* trash_protected) {
    if (!checkpoint || !capacity || !depth || !trash_protected) return -1;
    checkpoint[0] = '\0';
    *trash_protected = jash_trash_protected();
    return jash_checkpoint_agent_begin(checkpoint, capacity, depth);
}

int jash_ai_auto_record(const char* checkpoint, const char* command,
                        int rc, int depth_before, int* depth_after,
                        char digest_hex[65]) {
    if (!command || !depth_after || !digest_hex) return -1;
    uint8_t hash[32];
    blake3_hash(command, strlen(command), hash);
    jash_blake3_hex(hash, digest_hex);
    *depth_after = jash_timeline_depth();
    char detail[256];
    int n = snprintf(detail, sizeof(detail),
        "checkpoint=%s command_blake3=%s rc=%d timeline=%d->%d",
        checkpoint && *checkpoint ? checkpoint : "unavailable", digest_hex,
        rc, depth_before, *depth_after);
    if (n < 0 || (size_t)n >= sizeof(detail)) return -1;
    /* Sigil's own chain remains authoritative: this records an event when
     * enabled, never pretends that a disabled/failed chain signed a receipt. */
    if (jash_sigil_get() >= SIGIL_LOG) jash_sigil_attest("ai.auto", detail);
    return 0;
}

/* Shim: callers that stripped the leading '?' call this */
void jash_ai_submit_query(jash_state_t* s, jash_writer_t w, void* ctx, const char* query) {
    char full[8192];
    snprintf(full, sizeof(full), "? %s", query ? query : "");
    jash_ai_route(s, w, ctx, full);
}

/* ── Spinner (for statusbar while busy) ───────────────────────────────────── */

void jash_ai_render_spinner(jash_writer_t w, void* ctx) {
    if (!g_ai_mode) return;
    /* braille spinner frames (UTF-8 octal to avoid hex-digit-greedy parse) */
    static const char* frames[] = {
        "\342\240\213","\342\240\231","\342\240\271","\342\240\270",
        "\342\240\274","\342\240\264","\342\240\246","\342\240\247",
        "\342\240\207","\342\240\217"
    };
    static int idx = 0;
    if (g_ai_busy) {
        jash_writes(w, ctx, "\033[31m");
        jash_writes(w, ctx, frames[idx]);
        jash_writes(w, ctx, "\033[0m");
        idx = (idx + 1) % 10;
    } else {
        jash_writes(w, ctx, "\033[32m\342\227\217\033[0m");
    }
}

/* ── Help ─────────────────────────────────────────────────────────────────── */

void jash_ai_help(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "\r\n\033[1;36m=== JASH AI ===\033[0m\r\n\r\n");
    jash_writes(w, ctx, "\033[1mModulo:\033[0m\r\n");
    jash_writes(w, ctx, "  \033[32m? on [auto]\033[0m -- carregar AI; auto permite execucao de comandos\r\n");
    jash_writes(w, ctx, "  \033[33m? off\033[0m      -- desativar AI (descarrega modulo)\r\n");
    jash_writes(w, ctx, "  \033[90m? status\033[0m   -- estado atual\r\n");
    jash_writes(w, ctx, "\r\n\033[1mModos (requerem modulo carregado):\033[0m\r\n");
    jash_writes(w, ctx, "  \033[36m? auto\033[0m     -- modo profundo (AI executa comandos)\r\n");
    jash_writes(w, ctx, "  \033[36m? learn\033[0m    -- modo aprendizado (salva no Obsidian)\r\n");
    jash_writes(w, ctx, "  \033[36m? debug\033[0m    -- modo debug\r\n");
    jash_writes(w, ctx, "\r\n\033[1mQuery:\033[0m\r\n");
    jash_writes(w, ctx, "  \033[36m? <query>\033[0m  -- perguntar ao AI (requer ? on)\r\n");
    jash_writes(w, ctx, "  \033[36m? list files [--type=file] [--uid=UID] [dir]\033[0m -- observacao tipada, limitada e somente leitura\r\n");
    jash_writes(w, ctx, "  \033[36m? count files [--type=file] [--uid=UID] [dir]\033[0m -- contar stream tipado sem materializar ref\r\n");
    jash_writes(w, ctx, "  \033[36m? tools\033[0m    -- descobrir operacoes de observacao tipadas\r\n");
    jash_writes(w, ctx, "  \033[36m? tool {\"version\":1,...}\033[0m -- chamada local de ferramenta read-only por JSON\r\n");
    jash_writes(w, ctx, "  \033[36m? observation read latest [offset [limit]]\033[0m -- ler pagina da referencia local\r\n");
    jash_writes(w, ctx, "  \033[36m? resolva esse erro de compilacao\033[0m -- reparo explicito, ate 3 tentativas e build verificado\r\n");
    jash_writes(w, ctx, "\r\n\033[1mQA Engine:\033[0m\r\n");
    jash_writes(w, ctx, "  \033[36m? qa\033[0m       -- QA completo\r\n");
    jash_writes(w, ctx, "  \033[36m? qa scan\033[0m  -- escanear source\r\n");
    jash_writes(w, ctx, "  \033[36m? qa test\033[0m  -- rodar testes\r\n");
    jash_writes(w, ctx, "\r\n");
}

/* ── Route ────────────────────────────────────────────────────────────────── */

int jash_ai_route(jash_state_t* s, jash_writer_t w, void* ctx, const char* line) {
    if (!line || line[0] != '?') return 0;
    const char* cmd = line + 1;
    while (*cmd == ' ') cmd++;

    /* ? / ? help */
    if (*cmd == '\0' || strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        jash_ai_help(w, ctx);
        return 1;
    }

    /* ? on [auto|learn] — lazy-load module; modifiers are idempotent. */
    if (strncmp(cmd, "on", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        const char* options = cmd + 2;
        while (*options == ' ') options++;
        int request_auto = !strncmp(options, "auto", 4) &&
                           (!options[4] || options[4] == ' ' || options[4] == '\t');
        int request_learn = !strncmp(options, "learn", 5) &&
                            (!options[5] || options[5] == ' ' || options[5] == '\t');
        if (!jmod_find("ai")) {
            char sp[512];
            if (jmod_resolve("ai", sp, sizeof(sp)) == 1) {
                if (jmod_load(sp, 0xFFFFFFFF) != 0) {
                    jash_writes(w, ctx, "\033[31m[ai]\033[0m falha ao carregar modulo\r\n");
                    return 1;
                }
            } else {
                jash_writes(w, ctx, "\033[31m[ai]\033[0m ai.modjash nao encontrado\r\n");
                jash_writes(w, ctx,
                    "\033[90m(buildar: bash modules/build_module.sh modules/ai)\033[0m\r\n");
                return 1;
            }
        }
        g_ai_mode = 1;
        if (request_auto) g_ai_auto = 1;
        if (request_learn) g_ai_learn = 1;
        if (g_ai_auto) {
            jash_writes(w, ctx,
                "\033[1;32m[ai]\033[0m AUTO ON -- comandos podem ser executados sem confirmacao\r\n");
        } else {
            jash_writes(w, ctx,
                "\033[1;32m[ai]\033[0m ON -- modulo carregado (DISPLAY ONLY; nada sera salvo/executado; use '? on auto' para executar)\r\n");
        }
        return 1;
    }

    /* ? off */
    if (strncmp(cmd, "off", 3) == 0 && (cmd[3] == '\0' || cmd[3] == ' ')) {
        g_ai_mode = 0; g_ai_auto = 0; g_ai_learn = 0; g_ai_busy = 0;
        jash_ai_observation_forget();
        if (jmod_find("ai")) jmod_unload("ai");
        jash_writes(w, ctx, "\033[1;33m[ai]\033[0m OFF -- modulo descarregado\r\n");
        return 1;
    }

    /* ? status */
    if (strcmp(cmd, "status") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[ai] active=%d auto=%d learn=%d debug=%d busy=%d module=%s\r\n",
            g_ai_mode, g_ai_auto, g_ai_learn, g_ai_debug, g_ai_busy,
            jmod_find("ai") ? "loaded" : "not-loaded");
        jash_writes(w, ctx, buf);
        return 1;
    }

    /* ? auto */
    if (strncmp(cmd, "auto", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        g_ai_auto ^= 1;
        if (g_ai_auto) g_ai_mode = 1;
        jash_writes(w, ctx, g_ai_auto ?
            "\033[32m[ai]\033[0m auto ON\r\n" : "\033[33m[ai]\033[0m auto OFF\r\n");
        return 1;
    }

    /* ? learn */
    if (strncmp(cmd, "learn", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        g_ai_learn ^= 1;
        if (g_ai_learn) g_ai_mode = 1;
        jash_writes(w, ctx, g_ai_learn ?
            "\033[33m[ai]\033[0m learn ON\r\n" : "\033[90m[ai]\033[0m learn OFF\r\n");
        return 1;
    }

    /* ? debug */
    if (strncmp(cmd, "debug", 5) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        g_ai_debug ^= 1;
        jash_writes(w, ctx, g_ai_debug ?
            "\033[36m[ai]\033[0m debug ON\r\n" : "\033[90m[ai]\033[0m debug OFF\r\n");
        return 1;
    }

    /* ? qa [...] — QA engine stays in core (no groq dependency) */
    if (strncmp(cmd, "qa", 2) == 0 && (cmd[2] == '\0' || cmd[2] == ' ')) {
        const char* sub = cmd + 2;
        while (*sub == ' ') sub++;
        const char* home = getenv("HOME");
        char sdir[512];
        snprintf(sdir, sizeof(sdir), "%s/refatorados/Jash/c", home ? home : ".");
        if (*sub == '\0' || strcmp(sub, "run") == 0)
            jash_qa_run(w, ctx, sdir);
        else if (strcmp(sub, "scan") == 0)
            jash_qa_scan(w, ctx, sdir);
        else if (strcmp(sub, "test") == 0)
            jash_qa_test(w, ctx);
        else if (strcmp(sub, "status") == 0)
            jash_qa_status(w, ctx);
        else if (strncmp(sub, "aggressive", 10) == 0) {
            const char* jb = sub + 10;
            while (*jb == ' ') jb++;
            jash_qa_aggressive(w, ctx, *jb ? jb : "./build/jash-release");
        } else {
            jash_writes(w, ctx, "\033[90m[qa] Uso: ? qa [run|scan|test|status]\033[0m\r\n");
        }
        return 1;
    }

    /* ? — fastpath: smalltalk sem LLM */
    {
        jash_fastpath_result_t fp = jash_fastpath_classify(cmd);
        if (fp.kind != FASTPATH_NONE) {
            jash_writes(w, ctx, fp.response);
            jash_writes(w, ctx, "\r\n");
            return 1;
        }
    }

    /* All other ? commands → ai module */
    jmod_entry_t* mod = jmod_find("ai");
    if (mod && mod->ops && mod->ops->dispatch) {
        jmod_call_t mc = { s, (jmod_writer_t)w, ctx, "?", cmd };
        int rc = mod->ops->dispatch(&mc);
        return rc >= 0 ? 1 : 0;
    }

    if (!g_ai_mode) {
        jash_writes(w, ctx,
            "\033[33m[ai]\033[0m Use '\033[36m? on\033[0m' para ativar o AI\r\n");
        return 1;
    }
    jash_writes(w, ctx,
        "\033[31m[ai]\033[0m Modulo nao carregado -- tente "
        "\033[36m? off\033[0m e \033[36m? on\033[0m\r\n");
    return 1;
}
