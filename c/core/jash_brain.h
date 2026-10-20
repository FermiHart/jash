// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_brain.h — Durable Memory: heat-decay facts + proof-keyed knowledge
// Port of BearCode's bear_brain + bear_proof into native C.
// Facts have configurable persistence: ephemeral decays, proof-keyed is immortal.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_BRAIN_H
#define JASH_CORE_BRAIN_H

#include <stdint.h>
#include <stddef.h>

// ── Constants ────────────────────────────────────────────────────────────────

#define BRAIN_FACTS_MAX         1024     // max stored facts
#define BRAIN_KEY_MAX            64      // fact key length
#define BRAIN_VAL_MAX           512      // fact value length (stack)
#define BRAIN_TAG_MAX            48      // category/tag length (stack)
#define BRAIN_VAL_HEAP_MAX      65536   // JINV-103: heap limit (64KB)
#define BRAIN_TAG_HEAP_MAX      4096    // JINV-103: heap tag limit (4KB)
#define BRAIN_HEAT_INIT          80      // initial heat for new facts
#define BRAIN_HEAT_PROOF        100      // proof-keyed facts start at 100
#define BRAIN_HEAT_DECAY_MS    300000    // 5 min per heat point

// ── Fact Types ───────────────────────────────────────────────────────────────

typedef enum {
    BRAIN_FACT_EPHEMERAL = 0,    // decays normally, lost on cleanup
    BRAIN_FACT_SESSION,          // lives for session duration
    BRAIN_FACT_PROOF_KEYED,      // backed by sigil proof, never decays
    BRAIN_FACT_PINNED,           // user-pinned, never decays
} jash_brain_fact_type_t;

// ── Fact ─────────────────────────────────────────────────────────────────────

typedef struct {
    char     key[BRAIN_KEY_MAX];
    char     val[BRAIN_VAL_MAX];
    char     tag[BRAIN_TAG_MAX];       // category: "build", "env", "user", "learned"
    char*    val_heap;                 // JINV-103: for val > BRAIN_VAL_MAX
    char*    tag_heap;                 // JINV-103: for tag > BRAIN_TAG_MAX
    jash_brain_fact_type_t type;
    uint32_t heat;                      // 0-100, 0 = forgotten
    uint64_t created_ns;                // when first stored
    uint64_t accessed_ns;               // last read
    uint64_t proof_seq;                 // sigil seq if proof-keyed, 0 otherwise
    uint32_t access_count;              // how many times read
    uint8_t  val_uses_heap : 1;         // JINV-103: flag for heap allocation
    uint8_t  tag_uses_heap : 1;         // JINV-103: flag for heap allocation
} jash_brain_fact_t;

// ── Brain Stats ──────────────────────────────────────────────────────────────

typedef struct {
    uint32_t total_facts;
    uint32_t ephemeral_count;
    uint32_t session_count;
    uint32_t proof_count;
    uint32_t pinned_count;
    uint32_t forgotten_count;   // facts that decayed to 0
    uint32_t tidied_count;      // total removed by tidy
} jash_brain_stats_t;

// ── Public API ───────────────────────────────────────────────────────────────

// Init with data directory (~/.jash/brain/). Returns 0 on success.
int  jash_brain_init(const char* data_dir);

// Cleanup and persist.
void jash_brain_cleanup(void);

// ── Store ────────────────────────────────────────────────────────────────────

// Store a fact. If key exists, updates value and resets heat.
// Returns 0 on success, -1 on failure (table full).
int  jash_brain_store(const char* key, const char* val, const char* tag,
                      jash_brain_fact_type_t type, uint64_t proof_seq);

// Convenience: store a proof-keyed fact (requires sigil seq).
int  jash_brain_store_proof(const char* key, const char* val, const char* tag,
                            uint64_t proof_seq);

// Convenience: store an ephemeral fact (decays over time).
int  jash_brain_store_ephemeral(const char* key, const char* val, const char* tag);

// ── Recall ───────────────────────────────────────────────────────────────────

// Find a fact by exact key. Returns NULL if not found or heat == 0.
const jash_brain_fact_t* jash_brain_recall(const char* key);

// Search facts by tag. Returns count of matches written to out (max out_cap).
int  jash_brain_search(const char* tag, jash_brain_fact_t* out, int out_cap);

// Search facts by value substring. Returns count.
int  jash_brain_search_val(const char* substr, jash_brain_fact_t* out, int out_cap);

// List all non-forgotten facts. Returns count.
int  jash_brain_list_all(jash_brain_fact_t* out, int out_cap);

// ── Forget / Tidy ────────────────────────────────────────────────────────────

// Forget a specific fact by key.
void jash_brain_forget(const char* key);

// Tidy: decay all ephemeral facts, remove those at heat == 0.
// Call periodically (e.g., every command or every N commands).
void jash_brain_tidy(void);

// ── Stats ────────────────────────────────────────────────────────────────────

jash_brain_stats_t jash_brain_stats(void);

// ── Persistence ──────────────────────────────────────────────────────────────

// Save brain to disk.
int  jash_brain_save(void);

// Load brain from disk.
int  jash_brain_load(void);

#endif // JASH_CORE_BRAIN_H
