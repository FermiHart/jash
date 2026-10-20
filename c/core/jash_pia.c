// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_pia.c — Protocol of Intent Atoms implementation
// Cross-message interning (§N references), delta-dedup, prefix-stable compression.
// F E R M I ∞ H A R T

#include "jash_pia.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ── FNV-1a (64-bit) — same as other modules ─────────────────────────────────

static uint64_t fnv1a64(const uint8_t* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

// ── Hex Digest ───────────────────────────────────────────────────────────────

static void to_hex(const uint8_t* data, size_t len, char* out) {
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i*2]     = hex[(data[i] >> 4) & 0x0F];
        out[i*2 + 1] = hex[data[i] & 0x0F];
    }
    out[len*2] = '\0';
}

// ── Init / Reset ─────────────────────────────────────────────────────────────

void pia_init(pia_session_t* pia) {
    if (!pia) return;
    memset(pia, 0, sizeof(*pia));
    pia->next_id = 1;
}

void pia_reset(pia_session_t* pia) {
    if (!pia) return;
    memset(pia->table, 0, sizeof(pia->table));
    pia->count = 0;
    pia->next_id = 1;
    pia->msg_counter = 0;
}

// ── Intern Table ─────────────────────────────────────────────────────────────

// Find or create an intern entry for a string.
// Returns key ("§N") if already interned, or interns and returns key.
static const char* intern(pia_session_t* pia, const char* str, size_t len,
                          char* key_out, size_t key_cap)
{
    if (!pia || !str || len == 0) return NULL;

    // Truncate if too long
    if (len > PIA_INTERN_VAL_MAX) len = PIA_INTERN_VAL_MAX;

    // Check if already interned (exact match)
    for (uint32_t i = 0; i < pia->count; i++) {
        size_t vlen = strlen(pia->table[i].val);
        if (vlen == len && memcmp(pia->table[i].val, str, len) == 0) {
            pia->table[i].ref_count++;
            if (key_out) {
                snprintf(key_out, key_cap, "§%u", i + 1);
            }
            return pia->table[i].key;
        }
    }

    // Add new entry if there's room
    if (pia->count >= PIA_MAX_INTERN_TABLE) return NULL;

    uint32_t idx = pia->count++;
    memcpy(pia->table[idx].val, str, len);
    pia->table[idx].val[len] = '\0';

    int n = snprintf(pia->table[idx].key, sizeof(pia->table[idx].key), "§%u", pia->next_id++);
    if (n < 0 || (size_t)n >= sizeof(pia->table[idx].key)) return NULL;

    pia->table[idx].ref_count = 1;
    pia->table[idx].saved_bytes = (uint32_t)(len > strlen(pia->table[idx].key) ? len - strlen(pia->table[idx].key) : 0);
    pia->total_saved_bytes += pia->table[idx].saved_bytes;

    if (key_out) {
        snprintf(key_out, key_cap, "%s", pia->table[idx].key);
    }
    return pia->table[idx].key;
}

// ── Compression ──────────────────────────────────────────────────────────────

// Find repeated words/phrases in input and replace with §N references.
// Uses a simple greedy strategy: find the longest repeated substring.
int pia_compress(pia_session_t* pia, const char* input, char* out, size_t cap) {
    if (!pia || !input || !out || cap == 0) return 0;

    size_t in_len = strlen(input);
    if (in_len == 0) { out[0] = '\0'; return 0; }

    // For short inputs, just pass through
    if (in_len < 32) {
        size_t n = in_len < cap ? in_len : cap - 1;
        memcpy(out, input, n);
        out[n] = '\0';
        return (int)n;
    }

    // Strategy: look for repeated substrings >= 12 bytes within this message.
    // For each repeated substring, intern it and replace with §N.

    char temp[PIA_CHUNK_MAX];
    size_t temp_len = 0;
    size_t i = 0;

    while (i < in_len && temp_len < cap - 16) {
        // Try to find a long repeat
        int found_repeat = 0;
        size_t best_start = 0;
        size_t best_len = 0;

        // Search for repeated substrings (minimum 12 chars)
        for (size_t start = i; start < in_len && !found_repeat; start++) {
            size_t max_len = in_len - start;
            if (max_len > 128) max_len = 128;  // limit search window

            for (size_t slen = max_len; slen >= 12 && !found_repeat; slen--) {
                // Check if this substring appears again later
                for (size_t j = start + slen; j + slen <= in_len; j++) {
                    if (memcmp(input + start, input + j, slen) == 0) {
                        best_start = start;
                        best_len = slen;
                        found_repeat = 1;
                        break;
                    }
                }
            }
        }

        if (found_repeat && best_len >= 12) {
            // Intern the repeated substring
            char key[PIA_INTERN_KEY_MAX];
            if (intern(pia, input + best_start, best_len, key, sizeof(key))) {
                // Copy non-repeated prefix before best_start
                if (best_start > i) {
                    size_t to_copy = best_start - i;
                    if (temp_len + to_copy >= cap - 16) to_copy = cap - 16 - temp_len;
                    memcpy(temp + temp_len, input + i, to_copy);
                    temp_len += to_copy;
                }

                // Append §N reference
                size_t kn = strlen(key);
                if (temp_len + kn < cap - 1) {
                    memcpy(temp + temp_len, key, kn);
                    temp_len += kn;
                }

                // Skip past the repeated text (keep one occurrence as literal before §N)
                i = best_start + best_len;

                // Also skip the matching occurrence
                // (We already output literal text from i to best_start, so
                //  the repeated occurrence is what we're replacing with §N)
                pia->total_saved_bytes += best_len - kn;
                continue;
            }
        }

        // No repeat found at this position — copy literal
        temp[temp_len++] = input[i++];
    }

    // Null-terminate
    temp[temp_len] = '\0';

    // Copy to output
    size_t n = temp_len < cap ? temp_len : cap - 1;
    memcpy(out, temp, n);
    out[n] = '\0';

    pia->total_input_bytes += in_len;
    return (int)n;
}

// ── Decompression ────────────────────────────────────────────────────────────

int pia_decompress(pia_session_t* pia, const char* compressed, char* out, size_t cap) {
    if (!pia || !compressed || !out || cap == 0) return -1;

    size_t out_len = 0;
    size_t i = 0;
    size_t in_len = strlen(compressed);

    // § is UTF-8: 0xC2 0xA7 — check first byte only
    while (i < in_len && out_len < cap - 1) {
        if ((unsigned char)compressed[i] == 0xC2 && i + 1 < in_len
            && (unsigned char)compressed[i+1] == 0xA7) {
            // Parse §N reference (skip both UTF-8 bytes)
            i += 2;
            uint32_t idx = 0;
            while (i < in_len && isdigit((unsigned char)compressed[i])) {
                idx = idx * 10 + (uint32_t)(compressed[i] - '0');
                i++;
            }

            if (idx > 0 && idx <= pia->count) {
                const char* val = pia->table[idx - 1].val;
                size_t vlen = strlen(val);
                if (out_len + vlen < cap - 1) {
                    memcpy(out + out_len, val, vlen);
                    out_len += vlen;
                }
            }
        } else {
            out[out_len++] = compressed[i++];
        }
    }

    out[out_len] = '\0';
    return (int)out_len;
}

// ── Delta-Dedup ──────────────────────────────────────────────────────────────

const char* pia_delta_dedup(pia_session_t* pia, const char* tool_output,
                            char* compressed, size_t compressed_cap)
{
    if (!pia || !tool_output) return NULL;

    size_t len = strlen(tool_output);
    if (len == 0) return NULL;

    // Only dedup outputs >= 64 bytes (small outputs aren't worth it)
    if (len < 64) return NULL;

    // Hash the output using FNV-1a
    uint64_t h = fnv1a64((const uint8_t*)tool_output, len);

    // Hex-encode the hash as key
    uint8_t h_bytes[8];
    for (int i = 0; i < 8; i++) {
        h_bytes[i] = (uint8_t)(h >> (56 - i*8));
    }
    char hex[17];  // 8 bytes -> 16 hex chars + NUL
    to_hex(h_bytes, 8, hex);

    // Search for existing hex key in intern table
    char search_key[PIA_INTERN_KEY_MAX];
    snprintf(search_key, sizeof(search_key), "§%s", hex);

    for (uint32_t i = 0; i < pia->count; i++) {
        if (strcmp(pia->table[i].key, search_key) == 0) {
            // Found duplicate — tell the caller to use the reference
            if (compressed && compressed_cap > 0) {
                snprintf(compressed, compressed_cap, "%s", search_key);
            }
            pia->table[i].ref_count++;
            return pia->table[i].key;
        }
    }

    // New output — register it
    if (pia->count < PIA_MAX_INTERN_TABLE) {
        uint32_t idx = pia->count++;
        size_t vlen = len < PIA_INTERN_VAL_MAX ? len : PIA_INTERN_VAL_MAX - 1;
        memcpy(pia->table[idx].val, tool_output, vlen);
        pia->table[idx].val[vlen] = '\0';
        memcpy(pia->table[idx].key, search_key, strlen(search_key) + 1);
        pia->table[idx].ref_count = 1;
        pia->table[idx].saved_bytes = (uint32_t)vlen;
    }

    return NULL;  // new — no dedup
}

// ── Prefix-Stable Incremental Compression ────────────────────────────────────

int pia_compress_next(pia_session_t* pia, const char* next_msg, char* out, size_t cap) {
    if (!pia || !next_msg || !out || cap == 0) return 0;

    // Each message is independently compressed.
    // The intern table persists across calls — this is what gives us
    // cross-message compression.
    //
    // Prefix stability (for Anthropic prompt caching):
    //   compress(msgs[:k]) == compress(msgs)[:k]
    //
    // This holds because interning only ADDS entries to the table, never
    // removes or reorders them. msg 1 may reference §1, msg 2 may reference
    // §2, but msg 1 NEVER references §2.

    pia->msg_counter++;
    return pia_compress(pia, next_msg, out, cap);
}

// ── Stats ────────────────────────────────────────────────────────────────────

float pia_ratio(pia_session_t* pia) {
    if (!pia || pia->total_input_bytes == 0) return 1.0f;
    uint64_t compressed = pia->total_input_bytes > pia->total_saved_bytes
                          ? pia->total_input_bytes - pia->total_saved_bytes
                          : 1;
    return (float)compressed / (float)pia->total_input_bytes;
}

uint64_t pia_saved(pia_session_t* pia) {
    return pia ? pia->total_saved_bytes : 0;
}
