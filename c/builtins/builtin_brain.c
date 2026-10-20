// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_brain — Liquid Reasoning Engine control panel
// F E R M I ∞ H A R T
//
// Commands:
//   brain suggest ["last command"]      predict next useful command
//   brain intent "fix cd tab"           classify intent + show confidence
//   brain impact <path>                 estimate blast-radius / anomaly risk
//   brain learn <cmd> rc=N next=...     explicit online training example
//   brain reward 1|-1                   reward/punish last prediction
//   brain guard [on|off|status]         toggle anomaly-detection guard
//   brain status                        show model statistics
//   brain reset                         forget all learned weights

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/jash_invoice.h>
#include <theme/io.h>
#include <liquid/jash_liquid.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscalls.h>
#include "semantic_builtin.h"

// ── Guard mode persistence ───────────────────────────────────────────────────

static int guard_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    int n = snprintf(out, sz, "%s/.jash/brain_guard_mode", home);
    if (n < 0 || (size_t)n >= sz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int guard_read(void) {
    char path[512];
    if (guard_path(path, sizeof(path))) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char buf[8] = {0};
    if (fread(buf, 1, sizeof(buf) - 1, f) == 0) buf[0] = '\0';  /* empty file = off */
    fclose(f);
    return buf[0] == '1';
}

static int guard_write(int on) {
    char path[512];
    if (guard_path(path, sizeof(path))) return -1;
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    int failed = fprintf(f, "%d\n", on) < 0;
    if (fclose(f)) failed = 1;
    return failed ? -1 : 0;
}

// ── Argument parsing helpers ─────────────────────────────────────────────────

static const char* skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static int parse_kv(const char* args, const char* key, char* out, size_t outsz) {
    const char* p = args;
    size_t klen = strlen(key);
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < outsz - 1) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return 1;
        }
        p++;
    }
    return 0;
}

static int parse_kv_int(const char* args, const char* key, int def) {
    char buf[32];
    if (parse_kv(args, key, buf, sizeof(buf))) {
        return atoi(buf);
    }
    return def;
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_suggest(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* args) {
    const char* a = skip_ws(args);
    if (!a || !*a) a = "";

    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) strlcpy(cwd, ".", sizeof(cwd));

    // Optional history priming: brain suggest --after "git add .;git commit"
    const char* after = NULL;
    const char* cmd = a;
    if (strncmp(a, "--after", 7) == 0) {
        after = skip_ws(a + 7);
        cmd = "";
        // look for closing quote or next token
        if (*after == '"' || *after == '\'') {
            char q = *after++;
            const char* end = strchr(after, q);
            if (end) {
                static char hist_buf[512];
                size_t n = (size_t)(end - after);
                if (n >= sizeof(hist_buf)) n = sizeof(hist_buf) - 1;
                memcpy(hist_buf, after, n);
                hist_buf[n] = '\0';
                after = hist_buf;
                cmd = skip_ws(end + 1);
            }
        }
    }

    // Prime hidden state with history
    if (after && *after) {
        char hist[512];
        snprintf(hist, sizeof(hist), "%s", after);
        char* token = strtok(hist, ";");
        while (token) {
            jash_liquid_prime(token, 0, 0, 0, cwd);
            token = strtok(NULL, ";");
        }
    }

    float features[JL_INPUT_DIM];
    jash_liquid_features(cmd, 0, 0, 0, cwd, features);

    jash_liquid_prediction_t p;
    jash_liquid_predict(features, &p);

    jash_writef(w, ctx, "[brain suggest] from '%s'\n", cmd);
    jash_writef(w, ctx, "  intent:       %s (%.1f%%)\n",
                JASH_LIQUID_INTENT_NAMES[p.top_intent_class],
                p.intent_confidence * 100.0f);
    jash_writef(w, ctx, "  next:         %s (%.1f%%)\n",
                p.top_cmd_name, p.cmd_confidence * 100.0f);
    if (p.top_k_classes[1] >= 0) {
        jash_writef(w, ctx, "  runners-up:   %s (%.1f%%), %s (%.1f%%)\n",
                    jash_liquid_class_label(p.top_k_classes[1]),
                    p.top_k_scores[1] * 100.0f,
                    jash_liquid_class_label(p.top_k_classes[2]),
                    p.top_k_scores[2] * 100.0f);
    }
    jash_writef(w, ctx, "  risk:         %.2f\n", p.risk);
    jash_writef(w, ctx, "  confidence:   %.2f\n", p.confidence);
    return 0;
}

static int cmd_intent(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    const char* text = skip_ws(args);
    if (!text || !*text) {
        jash_writes(w, ctx, "usage: brain intent \"<description>\"\n");
        return 1;
    }
    int ic = jash_liquid_intent_class(text);
    jash_writef(w, ctx, "[brain intent] '%s' -> %s (class %d)\n",
                text, JASH_LIQUID_INTENT_NAMES[ic], ic);
    return 0;
}

static int cmd_impact(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    const char* path = skip_ws(args);
    if (!path || !*path) {
        jash_writes(w, ctx, "usage: brain impact <path>\n");
        return 1;
    }
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) strlcpy(cwd, ".", sizeof(cwd));

    // Build a synthetic feature vector centered on the path under inspection.
    float features[JL_INPUT_DIM];
    jash_liquid_features(path, 0, 0, 1, cwd, features);

    jash_liquid_prediction_t p;
    jash_liquid_predict(features, &p);

    jash_writef(w, ctx, "[brain impact] %s\n", path);
    jash_writef(w, ctx, "  intent:       %s\n", JASH_LIQUID_INTENT_NAMES[p.top_intent_class]);
    jash_writef(w, ctx, "  risk:         %.2f\n", p.risk);
    jash_writef(w, ctx, "  confidence:   %.2f\n", p.confidence);
    if (p.risk > 0.7f) {
        jash_writes(w, ctx, "  ⚠ high predicted impact — consider speculate/expect first\n");
    }
    return 0;
}

static int cmd_learn(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    const char* a = skip_ws(args);
    if (!a || !*a) {
        jash_writes(w, ctx, "usage: brain learn <cmd> rc=N next=\"...\" intent=N reward=N\n");
        return 1;
    }

    char cmd[512] = {0};
    size_t i = 0;
    while (*a && *a != ' ' && *a != '\t' && i < sizeof(cmd) - 1) {
        cmd[i++] = *a++;
    }
    cmd[i] = '\0';
    const char* rest = skip_ws(a);

    int rc = parse_kv_int(rest, "rc", 0);
    int intent = -1;
    char itxt[64] = {0};
    if (parse_kv(rest, "intent", itxt, sizeof(itxt))) {
        // could be a number or a label like "build"
        char* endp = NULL;
        long inum = strtol(itxt, &endp, 10);
        if (endp && *endp == '\0') intent = (int)inum;
        else intent = jash_liquid_intent_class(itxt);
    }
    float reward = (float)parse_kv_int(rest, "reward", rc == 0 ? 1 : -1);

    char nextbuf[256] = {0};
    int next_class = -1;
    if (parse_kv(rest, "next", nextbuf, sizeof(nextbuf))) {
        next_class = jash_liquid_cmd_class(nextbuf);
    }

    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) strlcpy(cwd, ".", sizeof(cwd));

    jash_liquid_example_t ex = {0};
    jash_liquid_features(cmd, rc, 0, 0, cwd, ex.features);
    ex.cmd_class = jash_liquid_cmd_class(cmd);
    ex.next_cmd_class = next_class;
    ex.intent_class = intent >= 0 ? intent : jash_liquid_intent_class(cmd);
    ex.reward = reward;
    ex.timestamp_ns = 0;
    jash_liquid_learn(&ex);

    // Register human-readable labels for predicted classes.
    jash_liquid_update_label(jash_liquid_cmd_class(cmd), cmd);
    if (next_class >= 0 && nextbuf[0]) jash_liquid_update_label(next_class, nextbuf);

    jash_liquid_save();

    jash_writef(w, ctx, "[brain learn] observed '%s' rc=%d intent=%s reward=%.0f\n",
                cmd, rc, JASH_LIQUID_INTENT_NAMES[ex.intent_class], reward);
    return 0;
}

static int cmd_reward(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    const char* a = skip_ws(args);
    float r = a && *a ? (float)atoi(a) : 0.0f;
    if (r > 1.0f) r = 1.0f;
    if (r < -1.0f) r = -1.0f;
    jash_liquid_reward(r);
    jash_liquid_save();
    jash_writef(w, ctx, "[brain reward] applied %.0f\n", r);
    return 0;
}

static int cmd_guard(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    const char* a = skip_ws(args);
    int on = guard_read();
    if (!a || !*a || strncmp(a, "status", 6) == 0) {
        jash_writef(w, ctx, "[brain guard] %s\n", on ? "on" : "off");
        return 0;
    }
    if (strncmp(a, "on", 2) == 0) on = 1;
    else if (strncmp(a, "off", 3) == 0) on = 0;
    else {
        jash_writes(w, ctx, "usage: brain guard [on|off|status]\n");
        return 1;
    }
    if (guard_write(on)) return 1;
    jash_writef(w, ctx, "[brain guard] %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_status(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    jash_liquid_print_status(w, ctx);
    return 0;
}

static int cmd_reset(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    jash_liquid_reset();
    jash_writes(w, ctx, "[brain reset] learned weights cleared\n");
    return 0;
}

// ── Main dispatch ────────────────────────────────────────────────────────────

#include "brain_semantic.inc"

int jash_b_brain(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    int selected = semantic_dispatch(s, w, ctx, "brain", "jash.brain.v1", args, brain_semantic);
    if (selected >= 0) return selected;
    jash_liquid_init(NULL);
    {
        const char* a = args ? args : "";
        uint64_t n = strlen(a) / 4;
        if (n < 1) n = 1;
        jash_invoice_account_command(a, 1);
        jash_invoice_add_saved(JASH_INVOICE_BRAIN, n);
    }

    const char* a = skip_ws(args);
    if (!a || !*a) {
        jash_writes(w, ctx,
            "usage: brain <suggest|intent|impact|learn|reward|guard|status|reset>\n");
        return 1;
    }

    if (strncmp(a, "suggest", 7) == 0) {
        return cmd_suggest(s, w, ctx, skip_ws(a + 7));
    }
    if (strncmp(a, "intent", 6) == 0) {
        return cmd_intent(s, w, ctx, skip_ws(a + 6));
    }
    if (strncmp(a, "impact", 6) == 0) {
        return cmd_impact(s, w, ctx, skip_ws(a + 6));
    }
    if (strncmp(a, "learn", 5) == 0) {
        return cmd_learn(s, w, ctx, skip_ws(a + 5));
    }
    if (strncmp(a, "reward", 6) == 0) {
        return cmd_reward(s, w, ctx, skip_ws(a + 6));
    }
    if (strncmp(a, "guard", 5) == 0) {
        return cmd_guard(s, w, ctx, skip_ws(a + 5));
    }
    if (strncmp(a, "status", 6) == 0) {
        return cmd_status(s, w, ctx, skip_ws(a + 6));
    }
    if (strncmp(a, "reset", 5) == 0) {
        return cmd_reset(s, w, ctx, skip_ws(a + 5));
    }

    jash_writef(w, ctx, "brain: unknown subcommand '%s'\n", a);
    return 1;
}

SEMANTIC_ARGV_WRAPPER(brain, "brain", "jash.brain.v1", brain_semantic)
