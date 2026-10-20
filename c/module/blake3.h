/* blake3.h — Sovereign BLAKE3 (freestanding port from Beyond)
 * Heritage: TinaLinux (F E R M I ∞ H A R T)
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 *
 * Ported for jash userspace: sys/types.h → stdint.h + stddef.h
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BLAKE3_OUT_LEN     32
#define BLAKE3_KEY_LEN     32
#define BLAKE3_BLOCK_LEN   64
#define BLAKE3_CHUNK_LEN   1024
#define BLAKE3_MAX_DEPTH   54

struct blake3_chunk_state {
    uint32_t cv[8];
    uint64_t chunk_counter;
    uint8_t  buf[BLAKE3_BLOCK_LEN];
    uint8_t  buf_len;
    uint8_t  blocks_compressed;
    uint32_t flags;
};

struct blake3 {
    uint32_t key[8];
    struct blake3_chunk_state chunk;
    uint8_t  cv_stack_len;
    uint8_t  cv_stack[(BLAKE3_MAX_DEPTH + 1) * BLAKE3_OUT_LEN];
};

void blake3_init(struct blake3 *self);
void blake3_update(struct blake3 *self, const void *input, uint64_t len);
void blake3_final(const struct blake3 *self, uint8_t *out, uint64_t out_len);
void blake3_hash(const void *input, uint64_t len, uint8_t out[32]);

/* Keyed hash — MAC / KDF / ratchet */
void blake3_keyed_init(struct blake3 *self, const uint8_t key[32]);
