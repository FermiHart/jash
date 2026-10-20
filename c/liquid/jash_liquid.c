// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_liquid.c — Closed-form Continuous-time (CfC) Liquid Reasoning Engine
// F E R M I ∞ H A R T
//
// Pure-C online learning recurrent network for jash.
// No external dependencies. Sovereign, local, tiny.

#include "jash_liquid.h"

#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syscalls.h>

// ── Hyperparameters ──────────────────────────────────────────────────────────

#define JL_VERSION          2
#define JL_LEARNING_RATE    0.10f
#define JL_LR_DECAY         0.9999f
#define JL_DT               1.0f   // one command = one unit of continuous time
#define JL_TAU_MIN          0.5f
#define JL_TAU_MAX          10.0f
#define JL_EPS              1e-7f
#define WEIGHTS_FILE        "liquid_weights.bin"
#define STATS_FILE          "liquid_stats.bin"
#define HIST_FILE           "liquid_cmd_hist.bin"

// ── Model weights ────────────────────────────────────────────────────────────

static float W_in[JL_HIDDEN][JL_INPUT_DIM];
static float W_rec[JL_HIDDEN][JL_HIDDEN];
static float b_hidden[JL_HIDDEN];
static float tau_raw[JL_HIDDEN];
static float E_cmd[JL_CMD_CLASSES][JL_CMD_EMBED];

static float W_out[JL_CMD_CLASSES][JL_HIDDEN];
static float b_out[JL_CMD_CLASSES];

static float W_intent[JL_INTENT_CLASSES][JL_HIDDEN];
static float b_intent[JL_INTENT_CLASSES];

static float W_risk[JL_HIDDEN];
static float b_risk;
static float W_conf[JL_HIDDEN];
static float b_conf;

static float g_hidden[JL_HIDDEN];
static float g_hidden_prev[JL_HIDDEN];
static float g_lr = JL_LEARNING_RATE;

// ── Command class histogram: best-effort human labels ────────────────────────

static jash_liquid_cmd_hist_t g_cmd_hist[JL_CMD_CLASSES];

// ── Recent command history for priming ───────────────────────────────────────

static char g_history[JL_CMD_HISTORY][JL_CMD_LABEL_MAX];
static int  g_history_len = 0;

// ── Session state carried across post_cmd calls ──────────────────────────────

static char g_last_cmd[JL_CMD_LABEL_MAX];
static int  g_last_rc = 0;
static float g_last_features[JL_INPUT_DIM];
static int  g_last_cmd_class = -1;
static int  g_guard_on = -1;

// ── Stats / state ────────────────────────────────────────────────────────────

static jash_liquid_stats_t g_stats;
static char g_data_dir[JL_MAX_PATH];
static int  g_initialized = 0;

// ── Intent names ─────────────────────────────────────────────────────────────

const char* JASH_LIQUID_INTENT_NAMES[] = {
    "explore",
    "build",
    "fix",
    "refactor",
    "deploy",
    "verify",
    "cleanup",
    "other",
};

// ── Timing ───────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    }
    return 0;
}

// ── Math helpers ─────────────────────────────────────────────────────────────

static inline float sigmoid(float x) {
    if (x >= 0.0f) {
        float z = expf(-x);
        return 1.0f / (1.0f + z);
    } else {
        float z = expf(x);
        return z / (1.0f + z);
    }
}

static inline float dtanh(float y) {
    return 1.0f - y * y;
}

static uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float randf(uint32_t* state) {
    return (float)xorshift32(state) / 4294967295.0f;
}

static float xavier(uint32_t* state, int fan_in) {
    float r = randf(state) * 2.0f - 1.0f;
    return r * sqrtf(1.0f / (float)(fan_in ? fan_in : 1));
}

static void softmax(const float* logits, float* probs, int n) {
    float maxv = logits[0];
    for (int i = 1; i < n; i++) if (logits[i] > maxv) maxv = logits[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        probs[i] = expf(logits[i] - maxv);
        sum += probs[i];
    }
    for (int i = 0; i < n; i++) probs[i] /= (sum + JL_EPS);
}

static void top_k(const float* probs, int n, int k, int* out_idx, float* out_val) {
    for (int i = 0; i < k; i++) { out_idx[i] = -1; out_val[i] = -1.0f; }
    for (int i = 0; i < n; i++) {
        float v = probs[i];
        for (int j = 0; j < k; j++) {
            if (v > out_val[j]) {
                for (int m = k - 1; m > j; m--) {
                    out_val[m] = out_val[m-1];
                    out_idx[m] = out_idx[m-1];
                }
                out_val[j] = v;
                out_idx[j] = i;
                break;
            }
        }
    }
}

// ── Hashing / tokenization ───────────────────────────────────────────────────

static uint32_t hash_fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (s && *s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static const char* first_token(const char* cmd) {
    static char tok[128];
    tok[0] = '\0';
    if (!cmd) return tok;
    const char* p = cmd;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < 127) {
        tok[i++] = *p++;
    }
    tok[i] = '\0';
    return tok;
}

static void copy_label(char* dst, size_t dstsz, const char* cmd) {
    if (!cmd || !*cmd) { dst[0] = '\0'; return; }
    const char* p = cmd;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    size_t i = 0;
    while (*p && i + 1 < dstsz) {
        dst[i++] = *p++;
    }
    dst[i] = '\0';
}

int jash_liquid_cmd_class(const char* cmd) {
    uint32_t h = hash_fnv1a(first_token(cmd));
    return (int)(h % JL_CMD_CLASSES);
}

// ── Histogram tracking ───────────────────────────────────────────────────────

static void update_cmd_hist(int class_id, const char* cmd) {
    if (class_id < 0 || class_id >= JL_CMD_CLASSES) return;
    g_cmd_hist[class_id].class_id = class_id;
    g_cmd_hist[class_id].count++;
    // Keep the most common label via naive count bump.
    char label[JL_CMD_LABEL_MAX];
    copy_label(label, sizeof(label), cmd);
    if (g_cmd_hist[class_id].count == 1 ||
        g_cmd_hist[class_id].label[0] == '\0') {
        snprintf(g_cmd_hist[class_id].label, sizeof(g_cmd_hist[class_id].label),
                 "%s", label);
    }
}

const char* jash_liquid_class_label(int class_id) {
    if (class_id < 0 || class_id >= JL_CMD_CLASSES) return "?";
    if (g_cmd_hist[class_id].label[0]) return g_cmd_hist[class_id].label;
    return "?";
}

void jash_liquid_update_label(int class_id, const char* cmd) {
    update_cmd_hist(class_id, cmd);
}

// ── Intent classification ────────────────────────────────────────────────────

static const char* jash_strcasestr(const char* haystack, const char* needle) {
    if (!needle || !*needle) return haystack;
    if (!haystack) return NULL;
    size_t n = strlen(needle);
    for (const char* h = haystack; *h; h++) {
        if (strncasecmp(h, needle, n) == 0) return h;
    }
    return NULL;
}

int jash_liquid_intent_class(const char* text) {
    if (!text || !*text) return 7;
    const char* t = text;
    while (*t == ' ' || *t == '\t') t++;
    if (jash_strcasestr(t, "fix") || jash_strcasestr(t, "bug") || jash_strcasestr(t, "repair")) return 2;
    if (jash_strcasestr(t, "build") || jash_strcasestr(t, "compile") || jash_strcasestr(t, "cmake")) return 1;
    if (jash_strcasestr(t, "make") && !jash_strcasestr(t, "makefile")) return 1;
    if (jash_strcasestr(t, "test") || jash_strcasestr(t, "verify") || jash_strcasestr(t, "check") || jash_strcasestr(t, "expect")) return 5;
    if (jash_strcasestr(t, "refactor") || jash_strcasestr(t, "restructure")) return 3;
    if (jash_strcasestr(t, "deploy") || jash_strcasestr(t, "release") || jash_strcasestr(t, "install")) return 4;
    if (jash_strcasestr(t, "clean") || jash_strcasestr(t, "remove") || jash_strcasestr(t, "rm ") || jash_strcasestr(t, "trash")) return 6;
    if (jash_strcasestr(t, "explore") || jash_strcasestr(t, "find") || jash_strcasestr(t, "search") || jash_strcasestr(t, "inspect")) return 0;
    return 7;
}

// ── Feature extraction ───────────────────────────────────────────────────────

static float file_type_signature(const char* path) {
    if (!path) return 0.0f;
    const char* ext = strrchr(path, '.');
    if (!ext) return 0.1f;
    if (strcasecmp(ext, ".c") == 0 || strcasecmp(ext, ".h") == 0) return 0.2f;
    if (strcasecmp(ext, ".py") == 0) return 0.3f;
    if (strcasecmp(ext, ".sh") == 0) return 0.4f;
    if (strcasecmp(ext, ".md") == 0) return 0.5f;
    if (strcasecmp(ext, ".json") == 0 || strcasecmp(ext, ".yaml") == 0 || strcasecmp(ext, ".yml") == 0) return 0.6f;
    if (strcasecmp(ext, ".txt") == 0) return 0.7f;
    return 0.8f;
}

static float git_state_signature(void) {
    // Lightweight heuristic: presence of dirty files.
    FILE* f = popen("git status --porcelain 2>/dev/null | wc -l", "r");
    if (!f) return 0.0f;
    int dirty = 0;
    if (fscanf(f, "%d", &dirty) != 1) dirty = 0;
    pclose(f);
    return sigmoid((float)dirty / 5.0f);
}

void jash_liquid_features(const char* cmd, int rc, uint64_t duration_ms,
                          uint32_t mutation_count, const char* cwd,
                          float out[JL_INPUT_DIM]) {
    memset(out, 0, sizeof(float) * JL_INPUT_DIM);
    if (!cmd) cmd = "";
    if (!cwd) cwd = "";

    const char* tok = first_token(cmd);
    int cmd_class = jash_liquid_cmd_class(tok);
    uint32_t h_cwd = hash_fnv1a(cwd);

    // Learned command embedding (indices 0..JL_CMD_EMBED-1)
    for (int i = 0; i < JL_CMD_EMBED; i++) {
        out[i] = E_cmd[cmd_class][i];
    }

    // Hashed cwd features
    for (int i = 0; i < 4; i++) {
        out[JL_CMD_EMBED + i] = (float)((h_cwd >> (i * 8)) & 255) / 255.0f;
    }

    // Continuous / semantic features
    out[12] = rc == 0 ? 1.0f : 0.0f;
    out[13] = rc != 0 ? 1.0f : 0.0f;
    out[14] = sigmoid((float)duration_ms / 1000.0f - 1.0f);
    out[15] = sigmoid((float)mutation_count / 5.0f - 1.0f);

    // Command-shape features
    out[16] = sigmoid((float)strlen(cmd) / 80.0f - 0.5f);
    int pipes = 0, redirects = 0;
    for (const char* p = cmd; *p; p++) {
        if (*p == '|') pipes++;
        if (*p == '>' || *p == '<') redirects++;
    }
    out[17] = sigmoid((float)pipes / 3.0f);
    out[18] = sigmoid((float)redirects / 3.0f);

    // File-type signature of first path-like argument (heuristic)
    char path_arg[256] = {0};
    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t j = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '>' && j < sizeof(path_arg)-1) {
        path_arg[j++] = *p++;
    }
    out[19] = file_type_signature(path_arg);

    // Git state and recent-history features
    out[20] = git_state_signature();
    out[21] = g_history_len > 0 ? sigmoid((float)g_history_len / 8.0f) : 0.0f;
    out[22] = (g_history_len > 0) ? (float)(jash_liquid_cmd_class(g_history[g_history_len - 1]) % 16) / 15.0f : 0.0f;
    out[23] = sigmoid((float)g_stats.examples_seen / 100.0f);
}

// ── History ring ─────────────────────────────────────────────────────────────

static void push_history(const char* cmd) {
    if (!cmd || !*cmd) return;
    int pos = g_history_len % JL_CMD_HISTORY;
    snprintf(g_history[pos], sizeof(g_history[pos]), "%s", cmd);
    g_history_len++;
}

// ── Initialization ───────────────────────────────────────────────────────────

static void init_weights(void) {
    uint32_t seed = 0x9e3779b9u ^ (uint32_t)now_ns() ^ (uint32_t)getpid();
    for (int i = 0; i < JL_HIDDEN; i++) {
        for (int j = 0; j < JL_INPUT_DIM; j++) {
            W_in[i][j] = xavier(&seed, JL_INPUT_DIM + JL_HIDDEN);
        }
        for (int j = 0; j < JL_HIDDEN; j++) {
            W_rec[i][j] = xavier(&seed, JL_HIDDEN + JL_INPUT_DIM) * 0.5f;
        }
        b_hidden[i] = 0.0f;
        tau_raw[i] = -0.5f + randf(&seed);
    }
    for (int i = 0; i < JL_CMD_CLASSES; i++) {
        for (int j = 0; j < JL_CMD_EMBED; j++) {
            E_cmd[i][j] = xavier(&seed, JL_CMD_EMBED);
        }
    }
    for (int i = 0; i < JL_CMD_CLASSES; i++) {
        for (int j = 0; j < JL_HIDDEN; j++) W_out[i][j] = xavier(&seed, JL_HIDDEN);
        b_out[i] = 0.0f;
    }
    for (int i = 0; i < JL_INTENT_CLASSES; i++) {
        for (int j = 0; j < JL_HIDDEN; j++) W_intent[i][j] = xavier(&seed, JL_HIDDEN);
        b_intent[i] = 0.0f;
    }
    for (int j = 0; j < JL_HIDDEN; j++) {
        W_risk[j] = xavier(&seed, JL_HIDDEN);
        W_conf[j] = xavier(&seed, JL_HIDDEN);
    }
    b_risk = 0.0f;
    b_conf = 0.0f;
    memset(g_hidden, 0, sizeof(g_hidden));
    memset(g_hidden_prev, 0, sizeof(g_hidden_prev));
    memset(g_cmd_hist, 0, sizeof(g_cmd_hist));
    memset(g_history, 0, sizeof(g_history));
    memset(g_last_cmd, 0, sizeof(g_last_cmd));
    g_last_rc = 0;
    memset(g_last_features, 0, sizeof(g_last_features));
    g_last_cmd_class = -1;
    g_guard_on = -1;
    g_history_len = 0;
    g_lr = JL_LEARNING_RATE;
    g_stats = (jash_liquid_stats_t){0};
    g_stats.version = JL_VERSION;
    g_stats.hidden_size = JL_HIDDEN;
    g_stats.cmd_classes = JL_CMD_CLASSES;
    g_stats.intent_classes = JL_INTENT_CLASSES;
}

int jash_liquid_init(const char* data_dir) {
    if (g_initialized) return 0;
    if (data_dir) {
        size_t n = strlen(data_dir);
        if (n >= sizeof(g_data_dir)) { errno = ENAMETOOLONG; return -1; }
        memcpy(g_data_dir, data_dir, n + 1);
    } else {
        const char* home = getenv("HOME");
        if (!home || !home[0]) home = jash_tmpdir();
        int n = snprintf(g_data_dir, sizeof(g_data_dir), "%s/.jash", home);
        if (n < 0 || (size_t)n >= sizeof(g_data_dir)) { errno = ENAMETOOLONG; return -1; }
    }
    if (mkdir(g_data_dir, 0755) && errno != EEXIST) return -1;
    struct stat store;
    if (stat(g_data_dir, &store) || !S_ISDIR(store.st_mode)) { errno = ENOTDIR; return -1; }

    init_weights();
    if (jash_liquid_load() && errno != ENOENT) return -1;
    g_initialized = 1;
    return 0;
}

void jash_liquid_cleanup(void) {
    if (!g_initialized) return;
    jash_liquid_save();
    g_initialized = 0;
}

// ── Forward pass ─────────────────────────────────────────────────────────────

static void liquid_step(const float x[JL_INPUT_DIM], float hidden[JL_HIDDEN]) {
    for (int i = 0; i < JL_HIDDEN; i++) {
        float z = b_hidden[i];
        for (int j = 0; j < JL_INPUT_DIM; j++) z += W_in[i][j] * x[j];
        for (int j = 0; j < JL_HIDDEN; j++) z += W_rec[i][j] * hidden[j];
        float f = tanhf(z);

        float tau = JL_TAU_MIN + (JL_TAU_MAX - JL_TAU_MIN) * sigmoid(tau_raw[i]);
        float alpha = 1.0f - expf(-JL_DT / tau);
        hidden[i] = hidden[i] * (1.0f - alpha) + f * alpha;
    }
}

static void compute_logits(const float hidden[JL_HIDDEN],
                           float cmd_logits[JL_CMD_CLASSES],
                           float intent_logits[JL_INTENT_CLASSES],
                           float* risk, float* confidence) {
    for (int i = 0; i < JL_CMD_CLASSES; i++) {
        float z = b_out[i];
        for (int j = 0; j < JL_HIDDEN; j++) z += W_out[i][j] * hidden[j];
        cmd_logits[i] = z;
    }
    for (int i = 0; i < JL_INTENT_CLASSES; i++) {
        float z = b_intent[i];
        for (int j = 0; j < JL_HIDDEN; j++) z += W_intent[i][j] * hidden[j];
        intent_logits[i] = z;
    }
    float r = b_risk;
    for (int j = 0; j < JL_HIDDEN; j++) r += W_risk[j] * hidden[j];
    *risk = sigmoid(r);

    float c = b_conf;
    for (int j = 0; j < JL_HIDDEN; j++) c += W_conf[j] * hidden[j];
    *confidence = sigmoid(c);
}

// ── Learning ─────────────────────────────────────────────────────────────────

void jash_liquid_learn(const jash_liquid_example_t* ex) {
    if (!g_initialized) jash_liquid_init(NULL);
    if (!ex) return;

    g_stats.examples_seen++;
    g_lr *= JL_LR_DECAY;
    if (g_lr < 0.001f) g_lr = 0.001f;

    memcpy(g_hidden_prev, g_hidden, sizeof(g_hidden));

    // Forward
    float f[JL_HIDDEN];
    float alpha[JL_HIDDEN];
    for (int i = 0; i < JL_HIDDEN; i++) {
        float z = b_hidden[i];
        for (int j = 0; j < JL_INPUT_DIM; j++) z += W_in[i][j] * ex->features[j];
        for (int j = 0; j < JL_HIDDEN; j++) z += W_rec[i][j] * g_hidden[j];
        f[i] = tanhf(z);

        float tau = JL_TAU_MIN + (JL_TAU_MAX - JL_TAU_MIN) * sigmoid(tau_raw[i]);
        alpha[i] = 1.0f - expf(-JL_DT / tau);
        g_hidden[i] = g_hidden[i] * (1.0f - alpha[i]) + f[i] * alpha[i];
    }

    float cmd_logits[JL_CMD_CLASSES];
    float intent_logits[JL_INTENT_CLASSES];
    float risk_pred, conf_pred;
    compute_logits(g_hidden, cmd_logits, intent_logits, &risk_pred, &conf_pred);

    float cmd_probs[JL_CMD_CLASSES];
    float intent_probs[JL_INTENT_CLASSES];
    softmax(cmd_logits, cmd_probs, JL_CMD_CLASSES);
    softmax(intent_logits, intent_probs, JL_INTENT_CLASSES);

    // Gradients
    float dlogits_cmd[JL_CMD_CLASSES];
    memcpy(dlogits_cmd, cmd_probs, sizeof(dlogits_cmd));
    int target_cmd = ex->next_cmd_class >= 0 ? ex->next_cmd_class : 0;
    if (ex->next_cmd_class >= 0) dlogits_cmd[target_cmd] -= 1.0f;

    float dlogits_intent[JL_INTENT_CLASSES];
    memcpy(dlogits_intent, intent_probs, sizeof(dlogits_intent));
    int target_intent = ex->intent_class >= 0 ? ex->intent_class : 0;
    if (ex->intent_class >= 0) dlogits_intent[target_intent] -= 1.0f;

    float reward_target = ex->reward;
    float drisk = 2.0f * (risk_pred - reward_target) * risk_pred * (1.0f - risk_pred);
    float conf_target = sigmoid(fabsf(ex->reward) * 2.0f - 1.0f);
    float dconf = 2.0f * (conf_pred - conf_target) * conf_pred * (1.0f - conf_pred);

    // Backprop into hidden
    float dhidden[JL_HIDDEN];
    memset(dhidden, 0, sizeof(dhidden));
    for (int j = 0; j < JL_HIDDEN; j++) {
        for (int i = 0; i < JL_CMD_CLASSES; i++) dhidden[j] += dlogits_cmd[i] * W_out[i][j];
        for (int i = 0; i < JL_INTENT_CLASSES; i++) dhidden[j] += dlogits_intent[i] * W_intent[i][j];
        dhidden[j] += drisk * W_risk[j];
        dhidden[j] += dconf * W_conf[j];
    }

    // Through liquid gating
    float df[JL_HIDDEN];
    float dz[JL_HIDDEN];
    float dtau_raw[JL_HIDDEN];
    for (int i = 0; i < JL_HIDDEN; i++) {
        float dalpha = dhidden[i] * (f[i] - g_hidden_prev[i]);
        df[i] = dhidden[i] * alpha[i];

        float tau = JL_TAU_MIN + (JL_TAU_MAX - JL_TAU_MIN) * sigmoid(tau_raw[i]);
        float s = sigmoid(tau_raw[i]);
        float da_dtau_raw = -expf(-JL_DT / tau) * (JL_DT / (tau * tau)) * (JL_TAU_MAX - JL_TAU_MIN) * s * (1.0f - s);
        dtau_raw[i] = dalpha * da_dtau_raw;

        dz[i] = df[i] * dtanh(f[i]);

        for (int j = 0; j < JL_INPUT_DIM; j++) {
            W_in[i][j] -= g_lr * dz[i] * ex->features[j];
        }
        for (int j = 0; j < JL_HIDDEN; j++) {
            W_rec[i][j] -= g_lr * dz[i] * g_hidden_prev[j];
        }
        b_hidden[i] -= g_lr * dz[i];
        tau_raw[i] -= g_lr * dtau_raw[i];
    }

    // Update command embedding for the current command class.
    if (ex->cmd_class >= 0 && ex->cmd_class < JL_CMD_CLASSES) {
        for (int j = 0; j < JL_CMD_EMBED; j++) {
            float de = 0.0f;
            for (int i = 0; i < JL_HIDDEN; i++) {
                de += dz[i] * W_in[i][j];
            }
            E_cmd[ex->cmd_class][j] -= g_lr * de;
        }
    }

    // Output heads
    for (int i = 0; i < JL_CMD_CLASSES; i++) {
        for (int j = 0; j < JL_HIDDEN; j++) W_out[i][j] -= g_lr * dlogits_cmd[i] * g_hidden[j];
        b_out[i] -= g_lr * dlogits_cmd[i];
    }
    for (int i = 0; i < JL_INTENT_CLASSES; i++) {
        for (int j = 0; j < JL_HIDDEN; j++) W_intent[i][j] -= g_lr * dlogits_intent[i] * g_hidden[j];
        b_intent[i] -= g_lr * dlogits_intent[i];
    }
    for (int j = 0; j < JL_HIDDEN; j++) {
        W_risk[j] -= g_lr * drisk * g_hidden[j];
        W_conf[j] -= g_lr * dconf * g_hidden[j];
    }
    b_risk -= g_lr * drisk;
    b_conf -= g_lr * dconf;

    // Metrics
    float loss_cmd = 0.0f;
    if (ex->next_cmd_class >= 0 && cmd_probs[target_cmd] > 0.0f) {
        loss_cmd = -logf(cmd_probs[target_cmd] + JL_EPS);
    }
    if (g_stats.examples_seen == 1) g_stats.avg_loss = loss_cmd;
    else g_stats.avg_loss = 0.99f * g_stats.avg_loss + 0.01f * loss_cmd;

    int top1 = 0;
    for (int i = 1; i < JL_CMD_CLASSES; i++) if (cmd_probs[i] > cmd_probs[top1]) top1 = i;
    float correct = (ex->next_cmd_class >= 0 && top1 == ex->next_cmd_class) ? 1.0f : 0.0f;
    if (g_stats.examples_seen == 1) g_stats.accuracy = correct;
    else g_stats.accuracy = 0.99f * g_stats.accuracy + 0.01f * correct;
}

// ── Prediction ───────────────────────────────────────────────────────────────

void jash_liquid_predict(const float features[JL_INPUT_DIM],
                         jash_liquid_prediction_t* out) {
    if (!g_initialized) jash_liquid_init(NULL);
    if (!out) return;
    memset(out, 0, sizeof(*out));

    liquid_step(features, g_hidden);

    float cmd_logits[JL_CMD_CLASSES];
    float intent_logits[JL_INTENT_CLASSES];
    compute_logits(g_hidden, cmd_logits, intent_logits, &out->risk, &out->confidence);

    float cmd_probs[JL_CMD_CLASSES];
    float intent_probs[JL_INTENT_CLASSES];
    softmax(cmd_logits, cmd_probs, JL_CMD_CLASSES);
    softmax(intent_logits, intent_probs, JL_INTENT_CLASSES);

    int top_cmd = 0;
    for (int i = 1; i < JL_CMD_CLASSES; i++) if (cmd_probs[i] > cmd_probs[top_cmd]) top_cmd = i;
    out->top_cmd_class = top_cmd;
    out->cmd_confidence = cmd_probs[top_cmd];
    snprintf(out->top_cmd_name, sizeof(out->top_cmd_name), "%s", jash_liquid_class_label(top_cmd));

    int top_intent = 0;
    for (int i = 1; i < JL_INTENT_CLASSES; i++) if (intent_probs[i] > intent_probs[top_intent]) top_intent = i;
    out->top_intent_class = top_intent;
    out->intent_confidence = intent_probs[top_intent];

    top_k(cmd_probs, JL_CMD_CLASSES, 3, out->top_k_classes, out->top_k_scores);
}

// Prime the network with a previous command (advances hidden state).
void jash_liquid_prime(const char* cmd, int rc, uint64_t duration_ms,
                       uint32_t mutation_count, const char* cwd) {
    if (!g_initialized) jash_liquid_init(NULL);
    push_history(cmd);
    float features[JL_INPUT_DIM];
    jash_liquid_features(cmd, rc, duration_ms, mutation_count, cwd, features);
    liquid_step(features, g_hidden);
}

static int brain_guard_read(void) {
    if (g_guard_on >= 0) return g_guard_on;
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    char path[512];
    snprintf(path, sizeof(path), "%s/.jash/brain_guard_mode", home);
    FILE* f = fopen(path, "r");
    if (!f) { g_guard_on = 0; return 0; }
    char buf[8] = {0};
    if (fread(buf, 1, sizeof(buf) - 1, f) == 0) buf[0] = '\0';
    fclose(f);
    g_guard_on = (buf[0] == '1');
    return g_guard_on;
}

void jash_liquid_post_cmd(const char* cmd, int rc, uint64_t duration_ms,
                          uint32_t mutation_count, const char* cwd,
                          jash_writer_t w, void* ctx) {
    if (!jash_liquid_should_hook()) return;
    if (!g_initialized) jash_liquid_init(NULL);
    if (!cmd || !*cmd) return;

    // Skip compound command lines; the command-list splitter in eval.c feeds
    // individual segments. (Simple quote-unaware check is sufficient here.)
    for (const char* p = cmd; *p; p++) {
        if (*p == ';' || (*p == '&' && p[1] == '&') || (*p == '|' && p[1] == '|')) return;
    }

    push_history(cmd);

    float features[JL_INPUT_DIM];
    jash_liquid_features(cmd, rc, duration_ms, mutation_count, cwd, features);
    int cmd_class = jash_liquid_cmd_class(cmd);

    // Update human-readable label for this command class.
    jash_liquid_update_label(cmd_class, cmd);

    // Learn transition from previous command to this one.
    if (g_last_cmd[0]) {
        jash_liquid_example_t ex = {0};
        memcpy(ex.features, g_last_features, sizeof(ex.features));
        ex.cmd_class = g_last_cmd_class;
        ex.next_cmd_class = cmd_class;
        ex.intent_class = jash_liquid_intent_class(g_last_cmd);
        ex.reward = (rc == 0 && g_last_rc == 0) ? 1.0f : -1.0f;
        ex.timestamp_ns = now_ns();
        jash_liquid_learn(&ex);
    }

    // Guard: warn if current command is anomalous.
    if (brain_guard_read() && w) {
        jash_liquid_prediction_t p;
        jash_liquid_predict(features, &p);
        if (p.top_cmd_class != cmd_class && p.cmd_confidence > 0.5f && p.risk > 0.6f) {
            jash_writef(w, ctx,
                "\033[33m[brain guard] unexpected command '%s' (predicted '%s' %.0f%%; risk %.2f)\033[0m\n",
                cmd, p.top_cmd_name, p.cmd_confidence * 100.0f, p.risk);
        }
    }

    // Advance hidden state with observed command.
    liquid_step(features, g_hidden);

    // Carry state forward.
    snprintf(g_last_cmd, sizeof(g_last_cmd), "%s", cmd);
    g_last_rc = rc;
    memcpy(g_last_features, features, sizeof(g_last_features));
    g_last_cmd_class = cmd_class;

    // Persist occasionally (every 10 examples) and on exit.
    if (g_stats.examples_seen % 10 == 0) jash_liquid_save();
}

int jash_liquid_guard_alert(const char* cmd, int rc, const char* cwd) {
    if (!brain_guard_read()) return 0;
    if (!g_initialized) jash_liquid_init(NULL);
    float features[JL_INPUT_DIM];
    jash_liquid_features(cmd, rc, 0, 0, cwd, features);
    jash_liquid_prediction_t p;
    jash_liquid_predict(features, &p);
    int cmd_class = jash_liquid_cmd_class(cmd);
    return (p.top_cmd_class != cmd_class && p.cmd_confidence > 0.5f && p.risk > 0.6f);
}

int jash_liquid_autosuggest(const char* prefix, const char* cwd,
                            char* out, size_t cap) {
    if (!g_initialized) jash_liquid_init(NULL);
    if (!prefix || !out || cap == 0) return 0;
    out[0] = '\0';

    float features[JL_INPUT_DIM];
    jash_liquid_features(prefix, 0, 0, 0, cwd ? cwd : ".", features);
    jash_liquid_prediction_t p;
    jash_liquid_predict(features, &p);

    // High-confidence next-command suggestion.
    if (p.cmd_confidence < 0.30f) return 0;
    const char* sug = jash_liquid_class_label(p.top_cmd_class);
    if (!sug || sug[0] == '?' || !sug[0]) return 0;

    size_t plen = strlen(prefix);
    size_t slen = strlen(sug);

    // If prefix is empty, suggest the whole next command.
    if (plen == 0) {
        if (slen >= cap) slen = cap - 1;
        memcpy(out, sug, slen);
        out[slen] = '\0';
        return 1;
    }

    // If prefix is a known command and suggestion extends it.
    if (plen < slen && strncmp(prefix, sug, plen) == 0) {
        const char* suffix = sug + plen;
        size_t n = strlen(suffix);
        if (n >= cap) n = cap - 1;
        memcpy(out, suffix, n);
        out[n] = '\0';
        return 1;
    }

    return 0;
}

int jash_liquid_should_hook(void) {
    const char* env = getenv("JASH_BRAIN");
    if (env && (env[0] == '1' || env[0] == 't' || env[0] == 'y')) return 1;
    if (env && strcmp(env, "lean") == 0) return 1;
    return 0;
}

void jash_liquid_reward(float reward) {
    if (!g_initialized) jash_liquid_init(NULL);
    jash_liquid_example_t ex = {0};
    ex.reward = reward;
    jash_liquid_learn(&ex);
}

// ── Reset / status ───────────────────────────────────────────────────────────

void jash_liquid_reset(void) {
    init_weights();
    g_stats.examples_seen = 0;
    g_stats.avg_loss = 0.0f;
    g_stats.accuracy = 0.0f;
    jash_liquid_save();
}

jash_liquid_stats_t jash_liquid_stats(void) {
    if (!g_initialized) jash_liquid_init(NULL);
    return g_stats;
}

void jash_liquid_print_status(jash_writer_t w, void* ctx) {
    if (!w) return;
    jash_liquid_stats_t s = jash_liquid_stats();
    jash_writef(w, ctx, "liquid model:\n");
    jash_writef(w, ctx, "  hidden:         %d\n", JL_HIDDEN);
    jash_writef(w, ctx, "  input dim:      %d\n", JL_INPUT_DIM);
    jash_writef(w, ctx, "  cmd classes:    %d\n", JL_CMD_CLASSES);
    jash_writef(w, ctx, "  intent classes: %d\n", JL_INTENT_CLASSES);
    jash_writef(w, ctx, "  examples:       %llu\n", (unsigned long long)s.examples_seen);
    jash_writef(w, ctx, "  avg loss:       %.4f\n", s.avg_loss);
    jash_writef(w, ctx, "  accuracy:       %.2f%%\n", s.accuracy * 100.0f);
    jash_writef(w, ctx, "  lr:             %.5f\n", g_lr);
    jash_writef(w, ctx, "  data dir:       %s\n", g_data_dir);
}

// ── Persistence ──────────────────────────────────────────────────────────────

static int path_join(char* out, size_t sz, const char* dir, const char* file) {
    int n = snprintf(out, sz, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sz) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int jash_liquid_save(void) {
    if (!g_data_dir[0]) return -1;
    char path[JL_MAX_PATH];
    if (path_join(path, sizeof(path), g_data_dir, WEIGHTS_FILE)) return -1;

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    int ok = 1;
    ok &= fwrite(W_in, sizeof(W_in), 1, f) == 1;
    ok &= fwrite(W_rec, sizeof(W_rec), 1, f) == 1;
    ok &= fwrite(b_hidden, sizeof(b_hidden), 1, f) == 1;
    ok &= fwrite(tau_raw, sizeof(tau_raw), 1, f) == 1;
    ok &= fwrite(E_cmd, sizeof(E_cmd), 1, f) == 1;
    ok &= fwrite(W_out, sizeof(W_out), 1, f) == 1;
    ok &= fwrite(b_out, sizeof(b_out), 1, f) == 1;
    ok &= fwrite(W_intent, sizeof(W_intent), 1, f) == 1;
    ok &= fwrite(b_intent, sizeof(b_intent), 1, f) == 1;
    ok &= fwrite(W_risk, sizeof(W_risk), 1, f) == 1;
    ok &= fwrite(&b_risk, sizeof(b_risk), 1, f) == 1;
    ok &= fwrite(W_conf, sizeof(W_conf), 1, f) == 1;
    ok &= fwrite(&b_conf, sizeof(b_conf), 1, f) == 1;
    ok &= fwrite(g_hidden, sizeof(g_hidden), 1, f) == 1;
    ok &= fwrite(&g_lr, sizeof(g_lr), 1, f) == 1;
    ok &= fwrite(g_last_cmd, sizeof(g_last_cmd), 1, f) == 1;
    ok &= fwrite(&g_last_rc, sizeof(g_last_rc), 1, f) == 1;
    ok &= fwrite(g_last_features, sizeof(g_last_features), 1, f) == 1;
    ok &= fwrite(&g_last_cmd_class, sizeof(g_last_cmd_class), 1, f) == 1;
    if (fclose(f)) ok = 0;
    if (!ok) { if (!errno) errno = EIO; return -1; }

    g_stats.last_saved_ns = now_ns();
    if (path_join(path, sizeof(path), g_data_dir, STATS_FILE)) return -1;
    FILE* sf = fopen(path, "wb");
    if (!sf) return -1;
    if (fwrite(&g_stats, sizeof(g_stats), 1, sf) != 1) ok = 0;
    if (fclose(sf)) ok = 0;
    if (!ok) { if (!errno) errno = EIO; return -1; }

    if (path_join(path, sizeof(path), g_data_dir, HIST_FILE)) return -1;
    FILE* hf = fopen(path, "wb");
    if (!hf) return -1;
    if (fwrite(g_cmd_hist, sizeof(g_cmd_hist), 1, hf) != 1) ok = 0;
    if (fclose(hf)) ok = 0;

    return ok ? 0 : -1;
}

int jash_liquid_load(void) {
    if (!g_data_dir[0]) return -1;
    char path[JL_MAX_PATH];
    if (path_join(path, sizeof(path), g_data_dir, WEIGHTS_FILE)) return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    int ok = 1;
    ok &= fread(W_in, sizeof(W_in), 1, f) == 1;
    ok &= fread(W_rec, sizeof(W_rec), 1, f) == 1;
    ok &= fread(b_hidden, sizeof(b_hidden), 1, f) == 1;
    ok &= fread(tau_raw, sizeof(tau_raw), 1, f) == 1;
    ok &= fread(E_cmd, sizeof(E_cmd), 1, f) == 1;
    ok &= fread(W_out, sizeof(W_out), 1, f) == 1;
    ok &= fread(b_out, sizeof(b_out), 1, f) == 1;
    ok &= fread(W_intent, sizeof(W_intent), 1, f) == 1;
    ok &= fread(b_intent, sizeof(b_intent), 1, f) == 1;
    ok &= fread(W_risk, sizeof(W_risk), 1, f) == 1;
    ok &= fread(&b_risk, sizeof(b_risk), 1, f) == 1;
    ok &= fread(W_conf, sizeof(W_conf), 1, f) == 1;
    ok &= fread(&b_conf, sizeof(b_conf), 1, f) == 1;
    ok &= fread(g_hidden, sizeof(g_hidden), 1, f) == 1;
    ok &= fread(&g_lr, sizeof(g_lr), 1, f) == 1;
    ok &= fread(g_last_cmd, sizeof(g_last_cmd), 1, f) == 1;
    ok &= fread(&g_last_rc, sizeof(g_last_rc), 1, f) == 1;
    ok &= fread(g_last_features, sizeof(g_last_features), 1, f) == 1;
    ok &= fread(&g_last_cmd_class, sizeof(g_last_cmd_class), 1, f) == 1;
    if (fgetc(f) != EOF || ferror(f)) ok = 0;
    if (fclose(f)) ok = 0;
    if (!memchr(g_last_cmd, 0, sizeof(g_last_cmd)) ||
        g_last_cmd_class < -1 || g_last_cmd_class >= JL_CMD_CLASSES) ok = 0;
    if (!ok) { errno = EINVAL; return -1; }

    if (path_join(path, sizeof(path), g_data_dir, STATS_FILE)) return -1;
    FILE* sf = fopen(path, "rb");
    if (sf) {
        jash_liquid_stats_t loaded;
        if (fread(&loaded, sizeof(loaded), 1, sf) == 1 && fgetc(sf) == EOF && !ferror(sf) &&
            loaded.version == JL_VERSION &&
            ((loaded.hidden_size == JL_HIDDEN && loaded.cmd_classes == JL_CMD_CLASSES && loaded.intent_classes == JL_INTENT_CLASSES) ||
             (!loaded.hidden_size && !loaded.cmd_classes && !loaded.intent_classes)) &&
            isfinite(loaded.avg_loss) && isfinite(loaded.accuracy)) {
            /* Early v2 writers left these redundant metadata fields at zero.
             * Their actual weight layout is fixed and checked independently. */
            loaded.hidden_size = JL_HIDDEN;
            loaded.cmd_classes = JL_CMD_CLASSES;
            loaded.intent_classes = JL_INTENT_CLASSES;
            g_stats = loaded;
        } else ok = 0;
        if (fclose(sf)) ok = 0;
    } else if (errno != ENOENT) ok = 0;
    if (!ok) { errno = EINVAL; return -1; }

    if (path_join(path, sizeof(path), g_data_dir, HIST_FILE)) return -1;
    FILE* hf = fopen(path, "rb");
    if (hf) {
        if (fread(g_cmd_hist, sizeof(g_cmd_hist), 1, hf) != 1 || fgetc(hf) != EOF || ferror(hf)) ok = 0;
        if (fclose(hf)) ok = 0;
        for (size_t i = 0; i < JL_CMD_CLASSES; i++) if (!memchr(g_cmd_hist[i].label, 0, sizeof(g_cmd_hist[i].label))) ok = 0;
    } else if (errno != ENOENT) ok = 0;
    if (!ok) { errno = EINVAL; return -1; }

    return ok ? 0 : -1;
}
