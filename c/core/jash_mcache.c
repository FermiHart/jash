// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_mcache.c — Memory Cache implementation
// Hidden state checkpointing for shell session context management.
// F E R M I ∞ H A R T

#include "jash_mcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

// ── Local State ──────────────────────────────────────────────────────────────

static jash_mcache_t g_mcache;
static char g_state_dir[MCACHE_PATH_MAX];
static int g_initialized = 0;

#define MCACHE_FILE "mcache.state"
#define MCACHE_MAGIC 0x4D434143  // "MCAC"
#define MCACHE_VERSION 1

// ── FNV-1a (64-bit) — matches Jash's existing hash from state.h ───────────────

static uint64_t fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static void fnv1a64_bytes(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint64_t h = fnv1a64(data, len);
    // Expand 64-bit hash to 32 bytes via iterative mixing
    for (int i = 0; i < 4; i++) {
        out[i*8 + 0] = (uint8_t)(h >> 56);
        out[i*8 + 1] = (uint8_t)(h >> 48);
        out[i*8 + 2] = (uint8_t)(h >> 40);
        out[i*8 + 3] = (uint8_t)(h >> 32);
        out[i*8 + 4] = (uint8_t)(h >> 24);
        out[i*8 + 5] = (uint8_t)(h >> 16);
        out[i*8 + 6] = (uint8_t)(h >> 8);
        out[i*8 + 7] = (uint8_t)(h >> 0);
        h = fnv1a64((uint8_t*)&h, 8);
    }
}

// ── State Derivation ─────────────────────────────────────────────────────────

void jash_mcache_derive(jash_mcache_state_t* out, const char* cwd,
                        const char* last_cmd, int rc, uint32_t env_hash)
{
    if (!out) return;

    // Feature vector is built from:
    //   [0..7]    — FNV-1a of CWD
    //   [8..15]   — FNV-1a of last command
    //   [16..23]  — env_hash (padded)
    //   [24..31]  — rc + timestamp quantized
    //   [32..255] — reserved / future expansion

    memset(out, 0, sizeof(*out));
    out->len = 256;

    uint64_t h;

    // CWD context
    h = fnv1a64((const uint8_t*)(cwd ? cwd : ""), cwd ? strlen(cwd) : 0);
    for (int i = 0; i < 8; i++) {
        out->data[i] = (uint8_t)(h >> (56 - i*8));
    }

    // Command context
    h = fnv1a64((const uint8_t*)(last_cmd ? last_cmd : ""), last_cmd ? strlen(last_cmd) : 0);
    for (int i = 0; i < 8; i++) {
        out->data[8 + i] = (uint8_t)(h >> (56 - i*8));
    }

    // Environment hash
    for (int i = 0; i < 8; i++) {
        out->data[16 + i] = (uint8_t)(env_hash >> (24 - i*8));
    }

    // RC (quantized 0..255) + time bucket (10-min epochs)
    uint8_t rc_q = (uint8_t)(rc < 0 ? 255 : (rc > 255 ? rc % 256 : rc));
    uint64_t t = 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        t = (uint64_t)ts.tv_sec / 600;  // 10-minute buckets
    }
    for (int i = 0; i < 8; i++) {
        out->data[24 + i] = (uint8_t)(t >> (56 - i*8));
    }
    out->data[24] ^= rc_q;  // fold RC into time bucket first byte
}

// ── Update ───────────────────────────────────────────────────────────────────

void jash_mcache_update(const jash_mcache_state_t* state) {
    if (!g_initialized || !state) return;
    g_mcache.hidden = *state;
    g_mcache.hidden.version++;
    g_mcache.last_write_ns = 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        g_mcache.last_write_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
}

const jash_mcache_state_t* jash_mcache_current(void) {
    if (!g_initialized) return NULL;
    return &g_mcache.hidden;
}

// ── Checkpoints ──────────────────────────────────────────────────────────────

int jash_mcache_capture(const char* label, int rc, uint64_t sigil_seq,
                        uint32_t token_estimate)
{
    if (!g_initialized || !g_mcache.enabled) return -1;

    uint32_t slot = g_mcache.cp_head;
    jash_mcache_cp_t* cp = &g_mcache.cps[slot];

    // Fill checkpoint
    memset(cp, 0, sizeof(*cp));
    cp->seq = sigil_seq;
    cp->rc = rc;
    cp->heat = MCACHE_HEAT_INIT;
    cp->token_estimate = token_estimate;

    if (label) {
        size_t n = strlen(label);
        if (n >= MCACHE_LABEL_MAX) n = MCACHE_LABEL_MAX - 1;
        memcpy(cp->label, label, n);
        cp->label[n] = '\0';
    }

    // Hash current hidden state into checkpoint
    fnv1a64_bytes(g_mcache.hidden.data, g_mcache.hidden.len, cp->state_hash);

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        cp->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    // Advance ring
    g_mcache.cp_head = (slot + 1) % MCACHE_MAX_CHECKPOINTS;
    if (g_mcache.cp_count < MCACHE_MAX_CHECKPOINTS) {
        g_mcache.cp_count++;
    }
    g_mcache.total_captures++;
    g_mcache.heat = MCACHE_HEAT_INIT;  // refresh heat on capture

    return (int)slot;
}

int jash_mcache_find_by_seq(uint64_t seq) {
    if (!g_initialized || g_mcache.cp_count == 0) return -1;

    uint32_t count = g_mcache.cp_count;
    uint32_t tail = (g_mcache.cp_head + MCACHE_MAX_CHECKPOINTS - count) % MCACHE_MAX_CHECKPOINTS;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (tail + i) % MCACHE_MAX_CHECKPOINTS;
        if (g_mcache.cps[idx].seq == seq) {
            g_mcache.total_recalls++;
            return (int)idx;
        }
    }
    return -1;
}

int jash_mcache_find_best(const uint8_t* state_hash) {
    if (!g_initialized || !state_hash || g_mcache.cp_count == 0) return -1;

    uint32_t count = g_mcache.cp_count;
    uint32_t tail = (g_mcache.cp_head + MCACHE_MAX_CHECKPOINTS - count) % MCACHE_MAX_CHECKPOINTS;

    int best_idx = -1;
    uint32_t best_score = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (tail + i) % MCACHE_MAX_CHECKPOINTS;
        jash_mcache_cp_t* cp = &g_mcache.cps[idx];

        // XOR-popcount similarity on first 8 bytes of state hash
        uint32_t score = 0;
        for (int j = 0; j < 8; j++) {
            uint8_t x = state_hash[j] ^ cp->state_hash[j];
            // popcount of x
            score += (uint32_t)(__builtin_popcount(x));
        }
        // Higher heat = better
        score = score * (cp->heat + 1) / 100;

        if (score > best_score) {
            best_score = score;
            best_idx = (int)idx;
        }
    }

    if (best_idx >= 0) g_mcache.total_recalls++;
    return best_idx;
}

const jash_mcache_cp_t* jash_mcache_recall(int index) {
    if (!g_initialized || index < 0 || (uint32_t)index >= MCACHE_MAX_CHECKPOINTS)
        return NULL;

    jash_mcache_cp_t* cp = &g_mcache.cps[index];
    if (cp->seq == 0 && cp->timestamp_ns == 0) return NULL;  // empty slot

    g_mcache.total_recalls++;
    return cp;
}

// ── Heat / Decay ─────────────────────────────────────────────────────────────

void jash_mcache_tick(uint64_t now_ns) {
    if (!g_initialized || !g_mcache.enabled) return;

    // Decay main heat
    if (g_mcache.last_write_ns > 0 && now_ns > g_mcache.last_write_ns) {
        uint64_t elapsed_ms = (now_ns - g_mcache.last_write_ns) / 1000000ULL;
        uint32_t decay = (uint32_t)(elapsed_ms / MCACHE_HEAT_DECAY_MS);
        if (decay > 0) {
            if (decay > g_mcache.heat) {
                g_mcache.total_decays += g_mcache.heat;
                g_mcache.heat = 0;
            } else {
                g_mcache.heat -= decay;
                g_mcache.total_decays += decay;
            }
        }
    }

    // Decay checkpoint heat
    uint32_t count = g_mcache.cp_count;
    uint32_t tail = (g_mcache.cp_head + MCACHE_MAX_CHECKPOINTS - count) % MCACHE_MAX_CHECKPOINTS;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (tail + i) % MCACHE_MAX_CHECKPOINTS;
        jash_mcache_cp_t* cp = &g_mcache.cps[idx];
        if (cp->timestamp_ns > 0 && now_ns > cp->timestamp_ns) {
            uint64_t elapsed_ms = (now_ns - cp->timestamp_ns) / 1000000ULL;
            uint32_t decay = (uint32_t)(elapsed_ms / MCACHE_HEAT_DECAY_MS);
            if (decay > cp->heat) cp->heat = 0;
            else cp->heat -= decay;
        }
    }
}

uint32_t jash_mcache_heat(void) {
    return g_initialized ? g_mcache.heat : 0;
}

// ── Persistence ──────────────────────────────────────────────────────────────

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t hidden_version;
    uint8_t  hidden_data[MCACHE_STATE_BYTES];
    uint32_t hidden_len;
    uint32_t cp_head;
    uint32_t cp_count;
    uint64_t total_captures;
    uint64_t total_recalls;
    uint64_t total_decays;
    uint32_t heat;
    uint64_t last_write_ns;
    // Checkpoints follow
} mcache_disk_header_t;
#pragma pack(pop)

int jash_mcache_save(void) {
    if (!g_initialized) return -1;

    char path[MCACHE_PATH_MAX + 32];
    int n = snprintf(path, sizeof(path), "%s/%s", g_state_dir, MCACHE_FILE);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    mcache_disk_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = MCACHE_MAGIC;
    h.version = MCACHE_VERSION;
    h.hidden_version = g_mcache.hidden.version;
    memcpy(h.hidden_data, g_mcache.hidden.data, MCACHE_STATE_BYTES);
    h.hidden_len = g_mcache.hidden.len;
    h.cp_head = g_mcache.cp_head;
    h.cp_count = g_mcache.cp_count;
    h.total_captures = g_mcache.total_captures;
    h.total_recalls = g_mcache.total_recalls;
    h.total_decays = g_mcache.total_decays;
    h.heat = g_mcache.heat;
    h.last_write_ns = g_mcache.last_write_ns;

    size_t written = fwrite(&h, 1, sizeof(h), f);
    if (written != sizeof(h)) { fclose(f); return -1; }

    written = fwrite(g_mcache.cps, sizeof(jash_mcache_cp_t), MCACHE_MAX_CHECKPOINTS, f);
    if (written != MCACHE_MAX_CHECKPOINTS) { fclose(f); return -1; }

    fclose(f);
    return 0;
}

int jash_mcache_load(void) {
    if (!g_initialized) return -1;

    char path[MCACHE_PATH_MAX + 32];
    int n = snprintf(path, sizeof(path), "%s/%s", g_state_dir, MCACHE_FILE);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    mcache_disk_header_t h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -1; }

    if (h.magic != MCACHE_MAGIC || h.version != MCACHE_VERSION) { fclose(f); return -1; }

    g_mcache.hidden.version = h.hidden_version;
    memcpy(g_mcache.hidden.data, h.hidden_data, MCACHE_STATE_BYTES);
    g_mcache.hidden.len = h.hidden_len;
    g_mcache.cp_head = h.cp_head;
    g_mcache.cp_count = h.cp_count;
    g_mcache.total_captures = h.total_captures;
    g_mcache.total_recalls = h.total_recalls;
    g_mcache.total_decays = h.total_decays;
    g_mcache.heat = h.heat;
    g_mcache.last_write_ns = h.last_write_ns;

    size_t read = fread(g_mcache.cps, sizeof(jash_mcache_cp_t), MCACHE_MAX_CHECKPOINTS, f);
    if (read != MCACHE_MAX_CHECKPOINTS) { fclose(f); return -1; }

    fclose(f);
    return 0;
}

// ── Init / Cleanup ───────────────────────────────────────────────────────────

int jash_mcache_init(const char* state_dir) {
    if (g_initialized) return 0;

    memset(&g_mcache, 0, sizeof(g_mcache));
    g_mcache.enabled = 1;
    g_mcache.heat = MCACHE_HEAT_INIT;

    if (state_dir) {
        size_t n = strlen(state_dir);
        if (n >= MCACHE_PATH_MAX) return -1;
        memcpy(g_state_dir, state_dir, n);
        g_state_dir[n] = '\0';

        // Ensure directory exists
        struct stat st;
        if (stat(state_dir, &st) != 0) {
            if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) return -1;
        } else if (!S_ISDIR(st.st_mode)) {
            return -1;
        }

        g_initialized = 1;
        jash_mcache_load();
    } else {
        g_state_dir[0] = '\0';
        g_initialized = 1;
    }

    return 0;
}

void jash_mcache_cleanup(void) {
    if (!g_initialized) return;
    if (g_state_dir[0]) jash_mcache_save();
    memset(&g_mcache, 0, sizeof(g_mcache));
    g_initialized = 0;
}

// ── Enable / Disable ─────────────────────────────────────────────────────────

void jash_mcache_enable(int enabled) {
    if (!g_initialized) return;
    g_mcache.enabled = enabled;
}

int jash_mcache_is_enabled(void) {
    return g_initialized && g_mcache.enabled;
}

// ── Post-Command Hook ────────────────────────────────────────────────────────

void jash_mcache_post_cmd(const char* cmd, int rc, uint64_t sigil_seq,
                          uint64_t now_ns)
{
    if (!g_initialized || !g_mcache.enabled) return;

    // Tick decay
    jash_mcache_tick(now_ns);

    // Derive new state
    char cwd[1024];
    cwd[0] = '\0';
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';

    uint32_t env_hash = 0;
    const char* path = getenv("PATH");
    if (path) {
        env_hash = (uint32_t)fnv1a64((const uint8_t*)path, strlen(path));
    }

    jash_mcache_state_t state;
    jash_mcache_derive(&state, cwd, cmd, rc, env_hash);
    jash_mcache_update(&state);

    // Capture checkpoint if:
    //   - non-zero exit (failure boundary)
    //   - or every N commands
    //   - or command matches capture patterns
    static uint32_t cmd_counter = 0;
    cmd_counter++;

    int should_capture = 0;
    char label[MCACHE_LABEL_MAX];

    if (rc != 0) {
        should_capture = 1;
        snprintf(label, sizeof(label), "fail:rc=%d", rc);
    } else if (cmd_counter % 10 == 0) {
        should_capture = 1;
        snprintf(label, sizeof(label), "periodic:%u", cmd_counter);
    } else if (cmd && (strstr(cmd, "make") || strstr(cmd, "cargo") ||
                       strstr(cmd, "cmake") || strstr(cmd, "git commit") ||
                       strstr(cmd, "git push") || strstr(cmd, "deploy")))
    {
        should_capture = 1;
        snprintf(label, sizeof(label), "event:%s", cmd);
    }

    // Truncate label if needed
    if (should_capture) {
        label[MCACHE_LABEL_MAX - 1] = '\0';
        jash_mcache_capture(label, rc, sigil_seq, 0);
    }
}

// ── Context for AI Prompts ───────────────────────────────────────────────────

int jash_mcache_context(char* buf, size_t cap) {
    if (!g_initialized || !buf || cap == 0) return 0;

    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, cap - pos,
        "mcache:heat=%u cps=%u total=%lu\n",
        g_mcache.heat, g_mcache.cp_count,
        (unsigned long)g_mcache.total_captures);

    // Include last 3 checkpoints
    uint32_t count = g_mcache.cp_count;
    uint32_t tail = (g_mcache.cp_head + MCACHE_MAX_CHECKPOINTS - count) % MCACHE_MAX_CHECKPOINTS;
    uint32_t show = count > 3 ? count - 3 : 0;

    for (uint32_t i = show; i < count && pos < cap; i++) {
        uint32_t idx = (tail + i) % MCACHE_MAX_CHECKPOINTS;
        jash_mcache_cp_t* cp = &g_mcache.cps[idx];
        pos += (size_t)snprintf(buf + pos, cap - pos,
            "  cp[%u] seq=%lu rc=%d heat=%u label=%s\n",
            idx, (unsigned long)cp->seq, cp->rc, cp->heat, cp->label);
    }

    if (pos >= cap) pos = cap - 1;
    buf[pos] = '\0';
    return (int)pos;
}

// ── Stats ────────────────────────────────────────────────────────────────────

const jash_mcache_t* jash_mcache_stats(void) {
    return g_initialized ? &g_mcache : NULL;
}
