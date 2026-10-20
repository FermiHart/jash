// SPDX-License-Identifier: LicenseRef-SPL-1.0
// slm_mesh.c — SLM Mesh Module Entry Point
// Part of Jash Cognitive OS (ADR-0022)
//
// Malha de Inteligencia Fluida: routes tasks between local SLM and cloud LLMs.
// Registers cognitive statusbar section for real-time LLM status display.

#include <module/modjash.h>
#include <ui/statusbar.h>
#include <core/llm_status.h>
#include "cloud_bridge.h"
#include "task_router.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

// ── Module State ───────────────────────────────────────────────────────────

static int g_section_id = -1;

// ── Helpers ────────────────────────────────────────────────────────────────

static void mesh_write(jmod_writer_t w, void* ctx, const char* s) {
    if (w) w(ctx, s, strlen(s));
}

static void mesh_writef(jmod_writer_t w, void* ctx, const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && w) w(ctx, buf, (size_t)n);
}

// ── Statusbar Section (cognitive mode) ─────────────────────────────────────

static void cognitive_section(sb_seg_t* seg, const void* state) {
    (void)state;
    // Show mesh routing stats
    char stat_str[64];
    snprintf(stat_str, sizeof(stat_str), "mesh:%.0f%%l/%.0f%%c",
             tr_local_percent(), tr_cloud_percent());
    // seg_str would be called here — we use the sb_seg_t API
    // This function is called by the statusbar render loop
}

// ── Mesh Status Command ────────────────────────────────────────────────────

static int cmd_mesh_status(const jmod_call_t* call) {
    mesh_writef(call->writer, call->writer_ctx,
        "\033[1;36mmesh:\033[0m SLM Mesh v0.1.0\n");

    // Show connected providers
    for (int i = 0; i < CB_PROVIDER_COUNT; i++) {
        const char* name = cb_provider_name((cb_provider_t)i);
        int has_key = cb_has_key((cb_provider_t)i);
        const char* model = cb_provider_model((cb_provider_t)i);

        // Also check llm_status for active connection
        llm_status_t ls = llm_status_get();

        if (has_key || i == CB_PROVIDER_OLLAMA) {
            int is_active = (ls.provider == (llm_provider_type_t)(i + 1));
            mesh_writef(call->writer, call->writer_ctx,
                "  \033[32m\xe2\x97\x8f\033[0m %s (%s)%s\n",
                name, model,
                is_active ? " \033[1;36m<-- active\033[0m" : "");
        } else {
            mesh_writef(call->writer, call->writer_ctx,
                "  \033[90m\xe2\x97\x8b\033[0m %s (no key)\n", name);
        }
    }

    // Show routing stats
    mesh_writef(call->writer, call->writer_ctx,
        "  local: %.0f%%  cloud: %.0f%%\n",
        tr_local_percent(), tr_cloud_percent());

    // Show LLM status details
    llm_status_t ls = llm_status_get();
    if (ls.connected) {
        mesh_writef(call->writer, call->writer_ctx,
            "  status: \033[32mconnected\033[0m to %s",
            ls.model_name[0] ? ls.model_name : "LLM");
        if (ls.total_tokens > 0)
            mesh_writef(call->writer, call->writer_ctx,
                " | %llu tokens", (unsigned long long)ls.total_tokens);
        if (ls.avg_latency_ms > 0)
            mesh_writef(call->writer, call->writer_ctx,
                " | avg %ums", ls.avg_latency_ms);
        mesh_writef(call->writer, call->writer_ctx, "\n");
    } else {
        mesh_writef(call->writer, call->writer_ctx,
            "  status: \033[90mdisconnected\033[0m\n");
    }

    return 0;
}

// ── Mesh Query Command ────────────────────────────────────────────────────

static int cmd_mesh_query(const jmod_call_t* call, const char* args) {
    const char* task = args;
    while (*task == ' ') task++;

    if (!*task) {
        mesh_writef(call->writer, call->writer_ctx,
            "mesh query: query a LLM\n"
            "usage: mesh query <prompt>\n"
            "example: mesh query \"explain what this shell does\"\n");
        return 0;
    }

    // Detect task type and route
    tr_route_t route = tr_route(task, 0.75f);

    mesh_writef(call->writer, call->writer_ctx,
        "\033[1;36mmesh:\033[0m routing to %s (%s)\n",
        cb_provider_name(route.provider), route.model);

    // Update LLM status
    llm_status_set_provider(
        (llm_provider_type_t)(route.provider + 1),
        route.model);
    llm_status_set_inferring(1);
    llm_status_set_connected(1);
    jash_statusbar_mark_dirty();

    if (route.is_cloud) {
        // Query cloud provider
        char response[4096];
        uint32_t latency_ms = 0;

        cb_error_t err = cb_query(route.provider, route.model,
                                   task, "You are a helpful shell assistant.",
                                   response, sizeof(response), &latency_ms);

        // Update LLM status with results
        llm_status_set_inferring(0);
        llm_status_update_latency(latency_ms);
        llm_status_increment_requests();
        tr_record_route(&route);
        jash_statusbar_mark_dirty();

        if (err == CB_OK) {
            mesh_writef(call->writer, call->writer_ctx,
                "\033[1;32mresponse:\033[0m %s\n", response);
            // Count approximate tokens
            llm_status_add_tokens(strlen(response) / 4);
        } else {
            const char* err_msg = "unknown error";
            if (err == CB_ERR_AUTH)    err_msg = "missing API key";
            if (err == CB_ERR_CONNECT) err_msg = "connection failed";
            mesh_writef(call->writer, call->writer_ctx,
                "\033[1;31merror:\033[0m %s (%ums)\n", err_msg, latency_ms);
            llm_status_increment_errors();
        }
    } else {
        // Local SLM — not yet implemented (placeholder)
        mesh_writef(call->writer, call->writer_ctx,
            "\033[1;33mlocal SLM:\033[0m not available yet\n"
            "  Install Ollama and run: ollama pull %s\n"
            "  Then: mesh provider ollama\n",
            route.model);
        llm_status_set_inferring(0);
    }

    return 0;
}

// ── Mesh Provider Command ──────────────────────────────────────────────────

static int cmd_mesh_provider(const jmod_call_t* call, const char* args) {
    const char* sub = args;
    while (*sub == ' ') sub++;

    if (!*sub) {
        mesh_writef(call->writer, call->writer_ctx,
            "mesh provider: set or list LLM providers\n"
            "usage:\n"
            "  mesh provider                list available providers\n"
            "  mesh provider <name>         set preferred provider\n"
            "providers: claude, gpt, groq, ollama\n");
        return 0;
    }

    // Check known provider names
    if (strncmp(sub, "claude", 6) == 0)
        tr_set_cloud_provider(CB_PROVIDER_CLAUDE);
    else if (strncmp(sub, "gpt", 3) == 0)
        tr_set_cloud_provider(CB_PROVIDER_GPT);
    else if (strncmp(sub, "groq", 4) == 0)
        tr_set_cloud_provider(CB_PROVIDER_GROQ);
    else if (strncmp(sub, "ollama", 6) == 0)
        tr_set_cloud_provider(CB_PROVIDER_OLLAMA);
    else {
        mesh_writef(call->writer, call->writer_ctx,
            "mesh: unknown provider '%s'\n", sub);
        return 1;
    }

    cb_provider_t p = (cb_provider_t)(
        strncmp(sub, "claude", 6) == 0 ? CB_PROVIDER_CLAUDE :
        strncmp(sub, "gpt", 3) == 0 ? CB_PROVIDER_GPT :
        strncmp(sub, "groq", 4) == 0 ? CB_PROVIDER_GROQ :
        CB_PROVIDER_OLLAMA);

    mesh_writef(call->writer, call->writer_ctx,
        "\033[1;36mmesh:\033[0m cloud provider set to %s (%s)\n",
        cb_provider_name(p), cb_provider_model(p));

    return 0;
}

// ── Help ───────────────────────────────────────────────────────────────────

static void mesh_help(jmod_writer_t w, void* ctx) {
    mesh_write(w, ctx,
        "mesh: SLM Mesh — Intelligent Query Router (ADR-0022)\n"
        "\n"
        "  Commands:\n"
        "    mesh status             show mesh status and connected providers\n"
        "    mesh query <prompt>     route a query to the best provider\n"
        "    mesh provider <name>    set preferred cloud provider\n"
        "    mesh help               this help\n"
        "\n"
        "  Providers: claude (default), gpt, groq, ollama\n"
        "  Set API key in env: ANTHROPIC_API_KEY, OPENAI_API_KEY, GROQ_API_KEY\n"
        "\n"
        "  Routing (ADR-0022 §3): 90%% local SLM, 8%% on-device, 2%% cloud\n");
}

// ── Main Dispatch ──────────────────────────────────────────────────────────

static int mesh_dispatch(const jmod_call_t* call) {
    if (!call->verb || strcmp(call->verb, "mesh") != 0) return -1;

    const char* sub = call->args ? call->args : "";
    while (*sub == ' ') sub++;

#define MATCH(s) (strncmp(sub, s, strlen(s)) == 0 \
                  && (sub[strlen(s)] == ' ' || sub[strlen(s)] == '\0'))

    if (!*sub || MATCH("help"))          { mesh_help(call->writer, call->writer_ctx); return 0; }
    if (MATCH("status"))                 return cmd_mesh_status(call);
    if (MATCH("query"))                  return cmd_mesh_query(call, sub + 5);
    if (MATCH("provider"))               return cmd_mesh_provider(call, sub + 8);

    mesh_help(call->writer, call->writer_ctx);
    return 1;
#undef MATCH
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

static int mesh_init(void) {
    cb_init();
    llm_status_init();
    tr_reset_stats();

    // Register statusbar section for cognitive mode
    g_section_id = jash_statusbar_register_section(cognitive_section);

    return 0;
}

static void mesh_shutdown(void) {
    if (g_section_id >= 0)
        jash_statusbar_unregister_section(g_section_id);
    llm_status_cleanup();
}

// ── Module Ops ─────────────────────────────────────────────────────────────

jmod_ops_t jmod_ops = {
    .init           = mesh_init,
    .shutdown       = mesh_shutdown,
    .dispatch       = mesh_dispatch,
    .hook_pre_cmd   = NULL,
    .hook_post_cmd  = NULL,
    .hook_exit      = NULL,
    .hook_mutation  = NULL,
    .name           = "slm-mesh",
    .version        = "0.1.0",
    .description    = "SLM Mesh — Malha de Inteligencia Fluida (ADR-0022)",
    .caps_used      = JMOD_CAP_STATE_READ | JMOD_CAP_FS_READ,
};