// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_brain.c — Durable Memory implementation
// Heat-decay facts + proof-keyed immortal facts for session-persistent context.
// F E R M I ∞ H A R T

#include "jash_brain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// ── Local State ──────────────────────────────────────────────────────────────

static jash_brain_fact_t  g_facts[BRAIN_FACTS_MAX];
static uint32_t           g_fact_count;
static jash_brain_stats_t g_stats;
static char               g_data_dir[512];
static int                g_initialized = 0;
static uint64_t           g_last_tidy_ns;

#define BRAIN_FILE "brain.dat"
#define BRAIN_MAGIC 0x42524149  // "BRAI"
#define BRAIN_VERSION 1

// ── Timing ───────────────────────────────────────────────────────────────────

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

// ── Internal Functions (declarations) ───────────────────────────────────────

static int  find_slot(const char* key);
static int  empty_slot(void);

// ── Find Slot ─────────────────────────────────────────────────────────────────

// Find slot by key, or return -1.
static int find_slot(const char* key) {
    if (!key) return -1;
    for (uint32_t i = 0; i < g_fact_count; i++) {
        if (strcmp(g_facts[i].key, key) == 0) return (int)i;
    }
    return -1;
}

// Find empty slot for new fact. Returns -1 if full.
static int empty_slot(void) {
    if (g_fact_count < BRAIN_FACTS_MAX) {
        return (int)(g_fact_count++);
    }
    // Try to find a forgotten (heat == 0) slot to reuse
    for (uint32_t i = 0; i < BRAIN_FACTS_MAX; i++) {
        if (g_facts[i].heat == 0 && g_facts[i].type != BRAIN_FACT_PROOF_KEYED
            && g_facts[i].type != BRAIN_FACT_PINNED) {
            return (int)i;
        }
    }
    return -1;
}

// ── Lazy init ────────────────────────────────────────────────────────────────
// The brain is touched only by the `brain` builtin and the liquid hook.
// Initializing (memset 1024 facts + stat/mkdir + full file load) on every
// `-c` startup is pure waste for runs that never consult it, so the first
// public entry point initializes from $HOME/.jash on demand. Behavior is
// unchanged: after ensure, the state is identical to eager startup init.

static void jash_brain_ensure(void) {
    if (g_initialized) return;
    const char* home = getenv("HOME");
    if (!home || !*home) {
        jash_brain_init(NULL);
        return;
    }
    char dir[512];
    size_t n = strlen(home);
    if (n + sizeof("/.jash") > sizeof(dir)) {
        jash_brain_init(NULL);
        return;
    }
    memcpy(dir, home, n);
    memcpy(dir + n, "/.jash", sizeof("/.jash"));
    jash_brain_init(dir);
}

// ── Init / Cleanup ───────────────────────────────────────────────────────────

int jash_brain_init(const char* data_dir) {
    if (g_initialized) return 0;

    memset(g_facts, 0, sizeof(g_facts));
    memset(&g_stats, 0, sizeof(g_stats));
    g_fact_count = 0;

    if (data_dir) {
        size_t n = strlen(data_dir);
        if (n >= sizeof(g_data_dir)) return -1;
        memcpy(g_data_dir, data_dir, n);
        g_data_dir[n] = '\0';

        struct stat st;
        if (stat(data_dir, &st) != 0) {
            if (mkdir(data_dir, 0700) != 0 && errno != EEXIST) return -1;
        } else if (!S_ISDIR(st.st_mode)) {
            return -1;
        }

        g_initialized = 1;
        jash_brain_load();
    } else {
        g_data_dir[0] = '\0';
        g_initialized = 1;
    }

    g_last_tidy_ns = now_ns();
    return 0;
}

void jash_brain_cleanup(void) {
    if (!g_initialized) return;
    
    // JINV-103: free all heap allocations before cleanup
    for (uint32_t i = 0; i < BRAIN_FACTS_MAX; i++) {
        if (g_facts[i].val_uses_heap && g_facts[i].val_heap) {
            free(g_facts[i].val_heap);
        }
        if (g_facts[i].tag_uses_heap && g_facts[i].tag_heap) {
            free(g_facts[i].tag_heap);
        }
    }
    
    if (g_data_dir[0]) jash_brain_save();
    memset(g_facts, 0, sizeof(g_facts));
    g_fact_count = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    g_initialized = 0;
}

// ── Store ────────────────────────────────────────────────────────────────────

int jash_brain_store(const char* key, const char* val, const char* tag,
                      jash_brain_fact_type_t type, uint64_t proof_seq)
{
    jash_brain_ensure();
    if (!g_initialized || !key) return -1;

    int slot = find_slot(key);
    if (slot < 0) {
        slot = empty_slot();
        if (slot < 0) return -1;
    }

    jash_brain_fact_t* f = &g_facts[slot];

    // Validate key before touching the existing fact.
    size_t n = strlen(key);
    if (n >= BRAIN_KEY_MAX) return -1;   /* refuse oversized key */

    /* Prepare heap replacements before mutating the slot. If either allocation
     * fails, the old fact remains intact — a store is atomic, never partial. */
    char* new_val_heap = NULL;
    char* new_tag_heap = NULL;
    int new_val_uses_heap = 0;
    int new_tag_uses_heap = 0;

    if (val) {
        n = strlen(val);
        if (n >= BRAIN_VAL_MAX) {
            if (n >= BRAIN_VAL_HEAP_MAX) return -1;
            new_val_heap = (char*)malloc(n + 1);
            if (!new_val_heap) return -1;
            memcpy(new_val_heap, val, n);
            new_val_heap[n] = '\0';
            new_val_uses_heap = 1;
        }
    }

    if (tag) {
        n = strlen(tag);
        if (n >= BRAIN_TAG_MAX) {
            if (n >= BRAIN_TAG_HEAP_MAX) { free(new_val_heap); return -1; }
            new_tag_heap = (char*)malloc(n + 1);
            if (!new_tag_heap) { free(new_val_heap); return -1; }
            memcpy(new_tag_heap, tag, n);
            new_tag_heap[n] = '\0';
            new_tag_uses_heap = 1;
        }
    }

    // Commit: only now replace the old value/tag.
    if (f->val_uses_heap && f->val_heap) free(f->val_heap);
    if (f->tag_uses_heap && f->tag_heap) free(f->tag_heap);

    n = strlen(key);
    memcpy(f->key, key, n);
    f->key[n] = '\0';

    if (new_val_uses_heap) {
        f->val_heap = new_val_heap;
        f->val_uses_heap = 1;
    } else {
        n = val ? strlen(val) : 0;
        if (val) memcpy(f->val, val, n);
        f->val[n] = '\0';
        f->val_heap = NULL;
        f->val_uses_heap = 0;
    }

    if (new_tag_uses_heap) {
        f->tag_heap = new_tag_heap;
        f->tag_uses_heap = 1;
    } else {
        n = tag ? strlen(tag) : 0;
        if (tag) memcpy(f->tag, tag, n);
        f->tag[n] = '\0';
        f->tag_heap = NULL;
        f->tag_uses_heap = 0;
    }

    f->type = type;
    f->proof_seq = proof_seq;
    f->heat = (type == BRAIN_FACT_PROOF_KEYED || type == BRAIN_FACT_PINNED)
              ? BRAIN_HEAT_PROOF : BRAIN_HEAT_INIT;
    f->created_ns = now_ns();
    f->accessed_ns = f->created_ns;
    f->access_count = 0;

    return 0;
}

int jash_brain_store_proof(const char* key, const char* val, const char* tag,
                           uint64_t proof_seq)
{
    return jash_brain_store(key, val, tag, BRAIN_FACT_PROOF_KEYED, proof_seq);
}

int jash_brain_store_ephemeral(const char* key, const char* val, const char* tag) {
    return jash_brain_store(key, val, tag, BRAIN_FACT_EPHEMERAL, 0);
}

// ── Recall ───────────────────────────────────────────────────────────────────

const jash_brain_fact_t* jash_brain_recall(const char* key) {
    jash_brain_ensure();
    if (!g_initialized || !key) return NULL;

    int slot = find_slot(key);
    if (slot < 0) return NULL;

    jash_brain_fact_t* f = &g_facts[slot];
    if (f->heat == 0) return NULL;

    f->accessed_ns = now_ns();
    f->access_count++;
    return f;
}

int jash_brain_search(const char* tag, jash_brain_fact_t* out, int out_cap) {
    jash_brain_ensure();
    if (!g_initialized || !tag || !out || out_cap <= 0) return 0;

    int count = 0;
    for (uint32_t i = 0; i < g_fact_count && count < out_cap; i++) {
        if (g_facts[i].heat > 0 && strcmp(g_facts[i].tag, tag) == 0) {
            out[count++] = g_facts[i];
        }
    }
    return count;
}

int jash_brain_search_val(const char* substr, jash_brain_fact_t* out, int out_cap) {
    jash_brain_ensure();
    if (!g_initialized || !substr || !out || out_cap <= 0) return 0;

    int count = 0;
    for (uint32_t i = 0; i < g_fact_count && count < out_cap; i++) {
        if (g_facts[i].heat > 0 && strstr(g_facts[i].val, substr)) {
            out[count++] = g_facts[i];
        }
    }
    return count;
}

int jash_brain_list_all(jash_brain_fact_t* out, int out_cap) {
    jash_brain_ensure();
    if (!g_initialized || !out || out_cap <= 0) return 0;

    int count = 0;
    for (uint32_t i = 0; i < g_fact_count && count < out_cap; i++) {
        if (g_facts[i].heat > 0) {
            out[count++] = g_facts[i];
        }
    }
    return count;
}

// ── Forget / Tidy ────────────────────────────────────────────────────────────

void jash_brain_forget(const char* key) {
    jash_brain_ensure();
    if (!g_initialized || !key) return;
    int slot = find_slot(key);
    if (slot < 0) return;
    // JINV-103: free heap allocations before clearing
    if (g_facts[slot].val_uses_heap && g_facts[slot].val_heap) {
        free(g_facts[slot].val_heap);
        g_facts[slot].val_heap = NULL;
    }
    if (g_facts[slot].tag_uses_heap && g_facts[slot].tag_heap) {
        free(g_facts[slot].tag_heap);
        g_facts[slot].tag_heap = NULL;
    }
    g_facts[slot].heat = 0;
    g_facts[slot].type = BRAIN_FACT_EPHEMERAL;
    g_stats.forgotten_count++;
}

void jash_brain_tidy(void) {
    jash_brain_ensure();
    if (!g_initialized) return;

    uint64_t now = now_ns();

    // Only tidy if enough time has passed (min 30s between tidies)
    if (g_last_tidy_ns > 0 && now - g_last_tidy_ns < 30000000000ULL) return;
    g_last_tidy_ns = now;

    for (uint32_t i = 0; i < g_fact_count; i++) {
        jash_brain_fact_t* f = &g_facts[i];

        // Proof-keyed and pinned facts never decay
        if (f->type == BRAIN_FACT_PROOF_KEYED || f->type == BRAIN_FACT_PINNED) continue;

        // Session facts last for session (don't decay)
        if (f->type == BRAIN_FACT_SESSION) continue;

        // Ephemeral facts: decay based on time since last access
        uint64_t elapsed_ms = (now - f->accessed_ns) / 1000000ULL;
        uint32_t decay = (uint32_t)(elapsed_ms / BRAIN_HEAT_DECAY_MS);

        if (decay > 0) {
            if (decay >= f->heat) {
                // JINV-103: free heap allocations when fact is forgotten
                if (f->val_uses_heap && f->val_heap) { free(f->val_heap); f->val_heap = NULL; }
                if (f->tag_uses_heap && f->tag_heap) { free(f->tag_heap); f->tag_heap = NULL; }
                f->heat = 0;
                g_stats.forgotten_count++;
                g_stats.tidied_count++;
            } else {
                f->heat -= decay;
            }
        }
    }
}

// ── Stats ────────────────────────────────────────────────────────────────────

jash_brain_stats_t jash_brain_stats(void) {
    jash_brain_stats_t s;
    memset(&s, 0, sizeof(s));

    if (!g_initialized) return s;

    for (uint32_t i = 0; i < g_fact_count; i++) {
        if (g_facts[i].heat == 0) continue;
        s.total_facts++;
        switch (g_facts[i].type) {
            case BRAIN_FACT_EPHEMERAL:   s.ephemeral_count++; break;
            case BRAIN_FACT_SESSION:     s.session_count++;   break;
            case BRAIN_FACT_PROOF_KEYED: s.proof_count++;     break;
            case BRAIN_FACT_PINNED:      s.pinned_count++;    break;
        }
    }

    s.forgotten_count = g_stats.forgotten_count;
    s.tidied_count = g_stats.tidied_count;
    return s;
}

// ── Persistence ──────────────────────────────────────────────────────────────

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t fact_count;
    uint32_t forgotten_count;
    uint32_t tidied_count;
} brain_disk_header_t;
#pragma pack(pop)

int jash_brain_save(void) {
    if (!g_initialized || !g_data_dir[0]) return -1;

    char path[512 + 16];
    int n = snprintf(path, sizeof(path), "%s/%s", g_data_dir, BRAIN_FILE);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    brain_disk_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = BRAIN_MAGIC;
    h.version = BRAIN_VERSION;
    h.fact_count = g_fact_count;
    h.forgotten_count = g_stats.forgotten_count;
    h.tidied_count = g_stats.tidied_count;

    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -1; }
    
    // JINV-103: Serialize facts with values serialized (not as pointers)
    for (uint32_t i = 0; i < BRAIN_FACTS_MAX; i++) {
        // Write fixed-size part
        if (fwrite(&g_facts[i].key, 1, BRAIN_KEY_MAX, f) != BRAIN_KEY_MAX) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].tag, 1, BRAIN_TAG_MAX, f) != BRAIN_TAG_MAX) { fclose(f); return -1; }
        
        // Write value length and content
        uint32_t val_len = g_facts[i].val_uses_heap ? strlen(g_facts[i].val_heap) : strlen(g_facts[i].val);
        if (fwrite(&val_len, 1, sizeof(val_len), f) != sizeof(val_len)) { fclose(f); return -1; }
        if (val_len > 0) {
            const char* val_src = g_facts[i].val_uses_heap ? g_facts[i].val_heap : g_facts[i].val;
            if (fwrite(val_src, 1, val_len, f) != val_len) { fclose(f); return -1; }
        }
        
        // Write remaining fixed fields
        if (fwrite(&g_facts[i].type, 1, sizeof(g_facts[i].type), f) != sizeof(g_facts[i].type)) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].heat, 1, sizeof(g_facts[i].heat), f) != sizeof(g_facts[i].heat)) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].created_ns, 1, sizeof(g_facts[i].created_ns), f) != sizeof(g_facts[i].created_ns)) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].accessed_ns, 1, sizeof(g_facts[i].accessed_ns), f) != sizeof(g_facts[i].accessed_ns)) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].proof_seq, 1, sizeof(g_facts[i].proof_seq), f) != sizeof(g_facts[i].proof_seq)) { fclose(f); return -1; }
        if (fwrite(&g_facts[i].access_count, 1, sizeof(g_facts[i].access_count), f) != sizeof(g_facts[i].access_count)) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}

int jash_brain_load(void) {
    if (!g_initialized || !g_data_dir[0]) return -1;

    char path[512 + 16];
    int n = snprintf(path, sizeof(path), "%s/%s", g_data_dir, BRAIN_FILE);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    brain_disk_header_t h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return -1; }

    if (h.magic != BRAIN_MAGIC || h.version != BRAIN_VERSION) { fclose(f); return -1; }

    g_fact_count = h.fact_count;
    g_stats.forgotten_count = h.forgotten_count;
    g_stats.tidied_count = h.tidied_count;

    // JINV-103: Deserialize facts
    for (uint32_t i = 0; i < BRAIN_FACTS_MAX; i++) {
        // Read fixed-size parts
        if (fread(&g_facts[i].key, 1, BRAIN_KEY_MAX, f) != BRAIN_KEY_MAX) { fclose(f); return -1; }
        if (fread(&g_facts[i].tag, 1, BRAIN_TAG_MAX, f) != BRAIN_TAG_MAX) { fclose(f); return -1; }
        
        // Read value
        uint32_t val_len;
        if (fread(&val_len, 1, sizeof(val_len), f) != sizeof(val_len)) { fclose(f); return -1; }
        
        if (val_len >= BRAIN_VAL_MAX) {
            // JINV-103: Use heap for large values
            g_facts[i].val_heap = (char*)malloc(val_len + 1);
            if (!g_facts[i].val_heap) { fclose(f); return -1; }
            if (fread(g_facts[i].val_heap, 1, val_len, f) != val_len) {
                free(g_facts[i].val_heap);
                fclose(f);
                return -1;
            }
            g_facts[i].val_heap[val_len] = '\0';
            g_facts[i].val_uses_heap = 1;
        } else {
            if (fread(g_facts[i].val, 1, val_len, f) != val_len) { fclose(f); return -1; }
            g_facts[i].val[val_len] = '\0';
            g_facts[i].val_uses_heap = 0;
        }
        
        // Read remaining fields
        if (fread(&g_facts[i].type, 1, sizeof(g_facts[i].type), f) != sizeof(g_facts[i].type)) { fclose(f); return -1; }
        if (fread(&g_facts[i].heat, 1, sizeof(g_facts[i].heat), f) != sizeof(g_facts[i].heat)) { fclose(f); return -1; }
        if (fread(&g_facts[i].created_ns, 1, sizeof(g_facts[i].created_ns), f) != sizeof(g_facts[i].created_ns)) { fclose(f); return -1; }
        if (fread(&g_facts[i].accessed_ns, 1, sizeof(g_facts[i].accessed_ns), f) != sizeof(g_facts[i].accessed_ns)) { fclose(f); return -1; }
        if (fread(&g_facts[i].proof_seq, 1, sizeof(g_facts[i].proof_seq), f) != sizeof(g_facts[i].proof_seq)) { fclose(f); return -1; }
        if (fread(&g_facts[i].access_count, 1, sizeof(g_facts[i].access_count), f) != sizeof(g_facts[i].access_count)) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}
