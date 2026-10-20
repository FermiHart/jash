// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ADR-0003 S0: minimal crypto primitives
//
// SHA-256: portable C core and optional runtime-gated CPU acceleration.
// No external dependencies; public-domain algorithm.
// BLAKE3 replaces this in S2 when crypto.modjash is wired.

#ifndef JASH_MODULE_CRYPTO_H
#define JASH_MODULE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* ── SHA-256 ───────────────────────────────────────────────────────── */

#define JASH_SHA256_DIGEST_LEN  32
#define JASH_SHA256_HEX_LEN     65   /* 64 hex chars + NUL */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} jash_sha256_ctx_t;

void jash_sha256_init  (jash_sha256_ctx_t* ctx);
void jash_sha256_update(jash_sha256_ctx_t* ctx, const uint8_t* data, size_t len);
void jash_sha256_final (jash_sha256_ctx_t* ctx, uint8_t digest[JASH_SHA256_DIGEST_LEN]);

/* One-shot convenience */
void jash_sha256(const void* data, size_t len,
                 uint8_t digest[JASH_SHA256_DIGEST_LEN]);

/* Hex-encode digest → 64-char NUL-terminated string */
void jash_sha256_hex(const uint8_t digest[JASH_SHA256_DIGEST_LEN],
                     char out[JASH_SHA256_HEX_LEN]);

/* Chain helper: SHA256(prev_hash[32] || extra bytes) */
void jash_sha256_chain(const uint8_t prev[JASH_SHA256_DIGEST_LEN],
                       const void*   data, size_t len,
                       uint8_t       out[JASH_SHA256_DIGEST_LEN]);

/* ── HMAC-SHA256 ───────────────────────────────────────────────────── */

void jash_hmac_sha256(const uint8_t* key,  size_t key_len,
                      const uint8_t* data, size_t data_len,
                      uint8_t        mac[JASH_SHA256_DIGEST_LEN]);

/* ── Key management ────────────────────────────────────────────────── */

/* Load or generate the 32-byte session key at ~/.jash/session.key.
 * Returns 0 on success, -1 on failure. */
int jash_session_key_load(uint8_t key[JASH_SHA256_DIGEST_LEN]);

/* ── BLAKE3 (S7) ───────────────────────────────────────────────────── */

#include <module/blake3.h>

#define JASH_BLAKE3_DIGEST_LEN  32
#define JASH_BLAKE3_HEX_LEN     65   /* 64 hex chars + NUL */

/* One-shot hash */
void jash_blake3(const void* data, size_t len, uint8_t out[32]);

/* Hex-encode a 32-byte digest → 64-char NUL-terminated string */
void jash_blake3_hex(const uint8_t digest[32], char out[JASH_BLAKE3_HEX_LEN]);

/* Keyed hash (BLAKE3 in keyed mode — replaces HMAC-SHA256 for proof/ratchet) */
void jash_blake3_keyed(const uint8_t key[32], const void* data, size_t len,
                       uint8_t out[32]);

/* Chain step: BLAKE3(prev[32] || payload[len]).
 * Matches logseal chain_step with jash payload layout. */
void jash_blake3_chain(const uint8_t prev[32],
                       const void* payload, size_t len,
                       uint8_t out[32]);

/* Ratchet advance: next_key = BLAKE3(cur_key || "r"), erase cur_key. */
void jash_blake3_ratchet(uint8_t key[32]);

#endif /* JASH_MODULE_CRYPTO_H */
