// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef SIGIL_ED25519_H
#define SIGIL_ED25519_H

#include <stdint.h>

/* ed25519.c — Ed25519 signatures (RFC 8032) + internal SHA-512.
 *
 * Public-domain TweetNaCl-derived implementation, used for the sovereign
 * key-custody / signing use case (PITCH §4 / P4). Deterministic, no
 * floating point (kernel builds -mno-sse). Validated against reference
 * vectors generated from an independent RFC 8032 implementation.
 */

#define ED25519_PUBKEY   32
#define ED25519_SECKEY   64   /* expanded: seed(32) || pubkey(32) */
#define ED25519_SIG      64
#define ED25519_SEED     32
/* JINV-014 — o teto tem que caber nas mensagens REAIS que assinamos.
 *
 * Era 1024, mas os dois call sites de sigil_module.c assinam `body[2048]` e
 * um PAE de ~2560 bytes. Acima do teto, ed25519_sign faz `return -1` ANTES de
 * escrever em sig[] — e ambos os chamadores IGNORAVAM o retorno, gravando 64
 * bytes de lixo de pilha como "assinatura". O usuario recebia "proof emitted"
 * com uma prova que nunca verificaria (auditoria 2026-07-27, achado 2.5).
 *
 * 4096 cobre body + PAE com folga. Custo: buf[64+4096] na pilha das duas
 * funcoes — 4 KiB, irrelevante. E os call sites agora CHECAM o retorno.
 */
#define ED25519_MAX_MSG  4096

/* Derive a keypair deterministically from a 32-byte seed.
 * sk = seed || pubkey (64 bytes), pk = pubkey (32 bytes). */
void ed25519_keypair_from_seed(const uint8_t seed[ED25519_SEED],
                               uint8_t pk[ED25519_PUBKEY],
                               uint8_t sk[ED25519_SECKEY]);

/* Detached signature over msg (len <= ED25519_MAX_MSG). Returns 0. */
int  ed25519_sign(uint8_t sig[ED25519_SIG],
                  const uint8_t *msg, uint32_t len,
                  const uint8_t sk[ED25519_SECKEY]);

/* Verify a detached signature. Returns 0 if valid, -1 otherwise. */
int  ed25519_verify(const uint8_t sig[ED25519_SIG],
                    const uint8_t *msg, uint32_t len,
                    const uint8_t pk[ED25519_PUBKEY]);

/* SHA-512 one-shot (exposed; used internally and by callers). */
void sha512(const void *data, uint32_t len, uint8_t out[64]);

/* ─── Ed25519 group arithmetic (for Pedersen commitments / HAP) ─────── */

/* Extended point representation: (X, Y, Z, T) in projective coordinates. */
typedef int64_t ed25519_gf[16];
typedef ed25519_gf ed25519_extpoint[4];

/* Multiply the Ed25519 base point g by a 32-byte scalar.
 * Result stored as compressed point (32 bytes). */
void ed25519_scalarbase(const uint8_t scalar[32], uint8_t point_out[32]);

/* Multiply an arbitrary point (32-byte compressed) by a 32-byte scalar.
 * Result stored as compressed point (32 bytes). */
void ed25519_scalarmult(uint8_t point_out[32],
                        const uint8_t point_in[32],
                        const uint8_t scalar[32]);

/* Add two compressed Ed25519 points.
 * Result stored as compressed point (32 bytes).
 * Returns 0 on success, -1 if either point is invalid. */
int ed25519_point_add(uint8_t result[32],
                      const uint8_t p1[32], const uint8_t p2[32]);

#endif
