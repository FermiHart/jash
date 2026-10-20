// SPDX-License-Identifier: LicenseRef-SPL-1.0
// cloud_bridge.c — HTTP/API bridge to external LLMs
// Part of Jash Cognitive OS (ADR-0022) — slm-mesh module
//
// Executes curl directly with argv. No shell, no libcurl dependency, and no
// way for prompts or API keys to become command syntax.

#include "cloud_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>

// ── Provider Configurations ────────────────────────────────────────────────

static const cb_config_t CONFIGS[CB_PROVIDER_COUNT] = {
    {  // Claude
        .key_env_var   = "ANTHROPIC_API_KEY",
        .endpoint      = "https://api.anthropic.com/v1/messages",
        .default_model = "claude-sonnet-4-20250514",
        .has_keys      = 0
    },
    {  // GPT
        .key_env_var   = "OPENAI_API_KEY",
        .endpoint      = "https://api.openai.com/v1/chat/completions",
        .default_model = "gpt-4o",
        .has_keys      = 0
    },
    {  // Groq
        .key_env_var   = "GROQ_API_KEY",
        .endpoint      = "https://api.groq.com/openai/v1/chat/completions",
        .default_model = "llama-3.3-70b-versatile",
        .has_keys      = 0
    },
    {  // Ollama
        .key_env_var   = NULL,
        .endpoint      = "http://localhost:11434/api/generate",
        .default_model = "llama3.2",
        .has_keys      = 1
    }
};

// ── Provider Status ───────────────────────────────────────────────────────

static int g_initialized = 0;

void cb_init(void) {
    g_initialized = 1;
}

// ── Public API ─────────────────────────────────────────────────────────────

const cb_config_t* cb_get_config(cb_provider_t provider) {
    if (provider >= CB_PROVIDER_COUNT) return NULL;
    return &CONFIGS[provider];
}

int cb_has_key(cb_provider_t provider) {
    if (provider >= CB_PROVIDER_COUNT) return 0;
    const cb_config_t* cfg = &CONFIGS[provider];
    if (!cfg->key_env_var) return cfg->has_keys;
    const char* key = getenv(cfg->key_env_var);
    return key && key[0] != '\0';
}

const char* cb_provider_name(cb_provider_t provider) {
    static const char* names[] = {"Claude", "GPT", "Groq", "Ollama"};
    if (provider >= CB_PROVIDER_COUNT) return "?";
    return names[provider];
}

const char* cb_provider_model(cb_provider_t provider) {
    if (provider >= CB_PROVIDER_COUNT) return NULL;
    return CONFIGS[provider].default_model;
}

// ── JSON body construction ──────────────────────────────────────────────────

static int cb_append(char* buf, size_t cap, size_t* pos, const char* text) {
    size_t n = strlen(text);
    if (*pos > cap || n > cap - *pos - 1) return -1;
    memcpy(buf + *pos, text, n + 1);
    *pos += n;
    return 0;
}

static int cb_append_json_string(char* buf, size_t cap, size_t* pos,
                                 const char* text) {
    if (cb_append(buf, cap, pos, "\"") != 0) return -1;
    for (const unsigned char* p = (const unsigned char*)(text ? text : "");
         *p; p++) {
        char escaped[8];
        const char* out = NULL;
        switch (*p) {
            case '"':  out = "\\\""; break;
            case '\\': out = "\\\\"; break;
            case '\b': out = "\\b"; break;
            case '\f': out = "\\f"; break;
            case '\n': out = "\\n"; break;
            case '\r': out = "\\r"; break;
            case '\t': out = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    out = escaped;
                }
                break;
        }
        if (out) {
            if (cb_append(buf, cap, pos, out) != 0) return -1;
        } else {
            if (*pos + 2 > cap) return -1;
            buf[(*pos)++] = (char)*p;
            buf[*pos] = '\0';
        }
    }
    return cb_append(buf, cap, pos, "\"");
}

static int build_body(char* buf, size_t cap, cb_provider_t provider,
                      const char* model, const char* prompt,
                      const char* system_prompt) {
    size_t pos = 0;
    if (cap == 0) return 0;
    buf[0] = '\0';
    if (cb_append(buf, cap, &pos, "{\"model\":") != 0 ||
        cb_append_json_string(buf, cap, &pos, model) != 0)
        return 0;

    switch (provider) {
    case CB_PROVIDER_CLAUDE:
        if (cb_append(buf, cap, &pos, ",\"max_tokens\":4096,\"system\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, system_prompt) != 0 ||
            cb_append(buf, cap, &pos, ",\"messages\":[{\"role\":\"user\",\"content\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, prompt) != 0 ||
            cb_append(buf, cap, &pos, "}]}") != 0)
            return 0;
        break;
    case CB_PROVIDER_GPT:
    case CB_PROVIDER_GROQ:
        if (cb_append(buf, cap, &pos, ",\"messages\":[{\"role\":\"system\",\"content\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, system_prompt) != 0 ||
            cb_append(buf, cap, &pos, "},{\"role\":\"user\",\"content\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, prompt) != 0 ||
            cb_append(buf, cap, &pos, "}]}") != 0)
            return 0;
        break;
    case CB_PROVIDER_OLLAMA:
        if (cb_append(buf, cap, &pos, ",\"prompt\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, prompt) != 0 ||
            cb_append(buf, cap, &pos, ",\"system\":") != 0 ||
            cb_append_json_string(buf, cap, &pos, system_prompt) != 0 ||
            cb_append(buf, cap, &pos, ",\"stream\":false}") != 0)
            return 0;
        break;
    default:
        return 0;
    }
    return (int)pos;
}

// ── Direct curl execution ───────────────────────────────────────────────────

static int cb_token_safe(const char* token) {
    if (!token || !*token || strlen(token) > 512) return 0;
    for (const unsigned char* p = (const unsigned char*)token; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '=')
            continue;
        return 0;
    }
    return 1;
}

static int cb_run_curl(char* const argv[], char* response, size_t response_cap,
                       size_t* total_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(pipefd[1]);
        execvp("curl", argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t total = 0;
    while (total + 1 < response_cap) {
        ssize_t n = read(pipefd[0], response + total, response_cap - total - 1);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        total += (size_t)n;
    }
    close(pipefd[0]);
    response[total] = '\0';
    if (total_out) *total_out = total;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int cb_exec_request(cb_provider_t provider, const char* body,
                           char* response, size_t response_cap,
                           size_t* total_out) {
    const cb_config_t* cfg = cb_get_config(provider);
    if (!cfg) return -1;

    char auth[640];
    char* argv[16];
    int argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "-s";
    argv[argc++] = "--max-time";
    argv[argc++] = "30";
    argv[argc++] = "-H";
    argv[argc++] = "Content-Type: application/json";

    if (provider == CB_PROVIDER_CLAUDE) {
        const char* key = getenv("ANTHROPIC_API_KEY");
        if (!cb_token_safe(key)) return -1;
        snprintf(auth, sizeof(auth), "x-api-key: %s", key);
        argv[argc++] = "-H"; argv[argc++] = auth;
        argv[argc++] = "-H"; argv[argc++] = "anthropic-version: 2023-06-01";
    } else if (provider == CB_PROVIDER_GPT || provider == CB_PROVIDER_GROQ) {
        const char* key = getenv(provider == CB_PROVIDER_GPT ?
                                 "OPENAI_API_KEY" : "GROQ_API_KEY");
        if (!cb_token_safe(key)) return -1;
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
        argv[argc++] = "-H"; argv[argc++] = auth;
    }

    argv[argc++] = "-d";
    argv[argc++] = (char*)body;
    argv[argc++] = (char*)cfg->endpoint;
    argv[argc] = NULL;
    return cb_run_curl(argv, response, response_cap, total_out);
}

// ── Extract response text from provider JSON ──────────────────────────────

static void extract_response(char* resp, cb_provider_t provider) {
    if (!resp || !*resp) return;

    // Find content start based on provider
    const char* start = NULL;
    const char* end = NULL;

    switch (provider) {
    case CB_PROVIDER_CLAUDE:
        // { "content": [{"text": "..."}] }
        start = strstr(resp, "\"text\":\"");
        if (start) {
            start += 8;
            end = strrchr(start, '"');
        }
        break;
    case CB_PROVIDER_GPT:
    case CB_PROVIDER_GROQ:
        // { "choices": [{"message": {"content": "..."}}] }
        start = strstr(resp, "\"content\":\"");
        if (start) {
            start += 11;
            end = strrchr(start, '"');
        }
        break;
    case CB_PROVIDER_OLLAMA:
        // { "response": "..." }
        start = strstr(resp, "\"response\":\"");
        if (start) {
            start += 12;
            end = strrchr(start, '"');
        }
        break;
    default:
        break;
    }

    if (start && end && end > start) {
        size_t len = (size_t)(end - start);
        memmove(resp, start, len);
        resp[len] = '\0';
        // Unescape common escapes
        char* d = resp;
        for (char* s = resp; *s; s++) {
            if (*s == '\\' && *(s+1) == 'n') { *d++ = '\n'; s++; }
            else if (*s == '\\' && *(s+1) == '"') { *d++ = '"'; s++; }
            else if (*s == '\\' && *(s+1) == 't') { *d++ = '\t'; s++; }
            else if (*s == '\\' && *(s+1) == '\\') { *d++ = '\\'; s++; }
            else { *d++ = *s; }
        }
        *d = '\0';
    } else {
        resp[0] = '\0';
    }
}

// ── Query an LLM ──────────────────────────────────────────────────────────

cb_error_t cb_query(cb_provider_t provider, const char* model,
                    const char* prompt, const char* system_prompt,
                    char* response, size_t response_cap,
                    uint32_t* latency_ms) {
    if (!response || response_cap == 0) return CB_ERR_PARSE;

    if (!g_initialized) cb_init();
    
    const cb_config_t* cfg = cb_get_config(provider);
    if (!cfg) return CB_ERR_CONNECT;
    if (!cb_has_key(provider)) return CB_ERR_AUTH;

    if (!model) model = cfg->default_model;

    if (!prompt) return CB_ERR_PARSE;

    // Build a properly escaped JSON body. A prompt is data; it must never be
    // concatenated into a shell command or JSON string unescaped.
    char body[4096];
    if (build_body(body, sizeof(body), provider, model, prompt, system_prompt) <= 0)
        return CB_ERR_PARSE;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t total = 0;
    int rc = cb_exec_request(provider, body, response, response_cap, &total);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t ms = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000000);
    if (latency_ms) *latency_ms = ms;

    if (rc != 0 || total == 0) return CB_ERR_CONNECT;

    // Extract meaningful response from JSON
    extract_response(response, provider);

    if (!response[0]) return CB_ERR_PARSE;
    return CB_OK;
}