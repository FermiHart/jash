// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/caps_json — JINV-018. Contrato em caps_json.h.
// F E R M I ∞ H A R T

#include "caps_json.h"

#include <builtins/builtin.h>
#include <core/agent_json.h>   /* jash_json_esc */   /* jash_builtin_count */
#include <core/serve.h>         /* JASH_EXEC_JSON_LINE_MAX */
#include <core/json_writer.h>
#include <module/modjash.h>     /* JMOD_ABI_VERSION */
#include <osif/osinfo.h>        /* bear_osinfo_caps / _target */
#include <theme/banner.h>       /* JASH_VERSION / JASH_CODENAME */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef JASH_GITHASH
#define JASH_GITHASH "unknown"
#endif

/* A máscara OSIF, nome a nome. Uma tabela só — a mesma ordem dos bits em
 * osinfo.h, para que um bit novo lá apareça aqui sem virar mentira. */
static const struct { unsigned bit; const char* name; } OSIF_BITS[] = {
    { OSIF_CAP_SYSID,   "sysid"   },
    { OSIF_CAP_PROC,    "proc"    },
    { OSIF_CAP_MEM,     "mem"     },
    { OSIF_CAP_MOUNTS,  "mounts"  },
    { OSIF_CAP_NET,     "net"     },
    { OSIF_CAP_UPTIME,  "uptime"  },
    { OSIF_CAP_LOADAVG, "loadavg" },
    { OSIF_CAP_SPAWN,   "spawn"   },
};
#define OSIF_BITS_N ((int)(sizeof(OSIF_BITS) / sizeof(OSIF_BITS[0])))

/* Existe o arquivo sob $HOME? Medição, não suposição. (Concatenação literal:
 * um formato variável aqui viraria -Wformat-nonliteral.) */
static int home_has(const char* home, const char* rel) {
    if (!home || !*home) return 0;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", home, rel);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    return access(path, R_OK) == 0;
}


/* JINV-047 — inventário de builtins legível por máquina.
 *
 * `--capabilities-json` diz `"builtins":152`. Uma contagem nao ajuda quem
 * precisa decidir "posso rodar isto sem perguntar?". Este inventario da
 * `name`, `help` e `effect` — e o `effect` e o item que mata uma classe
 * inteira de bug: hoje um agente adivinha perigo por regex sobre o NOME, e
 * quem reportou este pedido foi mordido duas vezes no mesmo dia por isso
 * (comparacao sensivel a caixa deixou `Bash` passar; a allow-list de leitura
 * deixou `grep` minusculo passar).
 *
 * Campos emitidos, TODOS de dado real da tabela:
 *   name         de JASH_BUILTINS[]
 *   help         idem
 *   argv_native  o builtin tem argv_fn (recebe argv tokenizado, nao string)
 *   effect       classificacao EXPLICITA (builtins/builtin_effects.c)
 *
 * Nao ha campo inventado. `effect` sai "unknown" para quem ainda nao foi
 * classificado — e "unknown" e resposta, nao buraco: o consumidor deve
 * trata-lo como perigoso. */
int jash_builtins_json(void) {
    int n = jash_builtin_count();
    int fd = STDOUT_FILENO;
    jash_json_writer_t w;
    jash_jsonw_init(&w, jash_jsonw_fd_sink, &fd);
    jash_jsonw_raw(&w, "{\"builtins\":[");
    for (int i = 0; i < n; i++) {
        const jash_builtin_t* b = jash_builtin_at((uint32_t)i);
        if (!b || !b->name) continue;
        jash_jsonw_raw(&w, i ? ",{\"name\":" : "{\"name\":");
        jash_jsonw_string(&w, b->name);
        jash_jsonw_raw(&w, ",\"help\":");
        jash_jsonw_string(&w, b->help ? b->help : "");
        jash_jsonw_raw(&w, ",\"argv_native\":");
        jash_jsonw_raw(&w, b->argv_fn ? "true" : "false");
        jash_jsonw_raw(&w, ",\"effect\":");
        jash_jsonw_string(&w, jash_builtin_effect(b->name));
        jash_jsonw_raw(&w, "}");
    }
    jash_jsonw_raw(&w, "],\"count\":"); jash_jsonw_u64(&w, (uint64_t)n);
    jash_jsonw_raw(&w, ",\"classified\":");
    jash_jsonw_u64(&w, (uint64_t)jash_builtin_effect_classified());
    jash_jsonw_raw(&w, "}\n");
    return jash_jsonw_finish(&w) == 0 ? 0 : 1;
}

int jash_capabilities_json(void) {
    unsigned caps = bear_osinfo_caps();
    const char* home = getenv("HOME");

    int fd = STDOUT_FILENO;
    jash_json_writer_t w;
    jash_jsonw_init(&w, jash_jsonw_fd_sink, &fd);
    jash_jsonw_raw(&w, "{");

    /* ── identidade: a mesma que --whoami reporta ───────────────────────── */
    jash_jsonw_raw(&w, "\"jash\":{\"version\":"); jash_jsonw_string(&w, JASH_VERSION);
    jash_jsonw_raw(&w, ",\"codename\":"); jash_jsonw_string(&w, JASH_CODENAME);
    jash_jsonw_raw(&w, ",\"git\":"); jash_jsonw_string(&w, JASH_GITHASH);
    jash_jsonw_raw(&w, ",\"modabi\":"); jash_jsonw_u64(&w, JMOD_ABI_VERSION);
    jash_jsonw_raw(&w, "},\"target\":"); jash_jsonw_string(&w, bear_osinfo_target());

    /* ── OSIF: o que ESTE backend implementa, e o que NÃO ───────────────── */
    char mask[32];
    snprintf(mask, sizeof(mask), "0x%02x", caps);
    jash_jsonw_raw(&w, ",\"osif\":{\"mask\":"); jash_jsonw_string(&w, mask);
    jash_jsonw_raw(&w, ",\"have\":[");
    for (int i = 0, n = 0; i < OSIF_BITS_N; i++)
        if (caps & OSIF_BITS[i].bit) {
            if (n++) jash_jsonw_raw(&w, ",");
            jash_jsonw_string(&w, OSIF_BITS[i].name);
        }
    jash_jsonw_raw(&w, "],\"missing\":[");
    for (int i = 0, n = 0; i < OSIF_BITS_N; i++)
        if (!(caps & OSIF_BITS[i].bit)) {
            if (n++) jash_jsonw_raw(&w, ",");
            jash_jsonw_string(&w, OSIF_BITS[i].name);
        }
    jash_jsonw_raw(&w, "]}");

    /* ── execução estruturada: campos HONRADOS pela porta única ─────────────
     * `timeout_ms` sai como "reserved" de propósito: o protocolo o aceita e
     * NÃO o impõe (serve.h). Dizer `true` aqui seria a mentira que este
     * descritor existe para não contar. */
    jash_jsonw_raw(&w, ",\"exec\":{\"exec_json\":{\"argv\":true,\"line\":true,\"cwd\":true,"
           "\"env\":true,\"stdin\":true,\"timeout_ms\":\"reserved\",\"byte_strings_version\":2},"
           "\"serve\":true,\"line_max\":");
    jash_jsonw_u64(&w, JASH_EXEC_JSON_LINE_MAX);
    jash_jsonw_raw(&w, ",\"argv0_quoted\":true}");

    /* ── superfície de comandos ─────────────────────────────────────────── */
    jash_jsonw_raw(&w, ",\"builtins\":"); jash_jsonw_u64(&w, (uint64_t)jash_builtin_count());

    /* ── módulos: o que é EXIGIDO, não o que existe (JINV-013) ──────────── */
    jash_jsonw_raw(&w, ",\"modules\":{\"loader\":true,\"blake3_verify\":");
    jash_jsonw_string(&w, getenv("JASH_MODULE_REQUIRE_HASH") ? "required" : "optional");
    jash_jsonw_raw(&w, ",\"dir\":");
    jash_jsonw_raw(&w, home_has(home, ".jash/modules") ? "true" : "false");
    jash_jsonw_raw(&w, "}");

    /* ── atestação: o módulo está instalado? (medido, não presumido) ────── */
    jash_jsonw_raw(&w, ",\"attest\":{\"sigil_module\":");
    jash_jsonw_raw(&w, home_has(home, ".jash/modules/sigil.modjash") ? "true" : "false");
    jash_jsonw_raw(&w, ",\"sigil_env\":");
    jash_jsonw_string(&w, getenv("JASH_SIGIL") ? getenv("JASH_SIGIL") : "");
    jash_jsonw_raw(&w, "}}\n");
    return jash_jsonw_finish(&w) == 0 ? 0 : 1;
}
