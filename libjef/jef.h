// SPDX-License-Identifier: LicenseRef-SPL-1.0
// libjef — Jash Execution Fabric: sovereign attestation crypto.
//
// libjef is the verification engine of jash, factored out so ANY C program can
// embed a tamper-evident execution record without depending on the shell:
//   - SHA-256 + HMAC-SHA256          (symmetric integrity / keyed MAC)
//   - BLAKE3 (hash / keyed / XOF)    (fast hashing)
//   - hash chaining + forward-secure ratchet  (append-only audit chain)
//
// Pure C11, freestanding, zero external dependencies. The chain/proof FORMAT
// layer and asymmetric (Ed25519) signing land in later waves (ADR-0003 S2);
// this header is the stable primitive surface.
//
// Build:   cmake --build <build> --target jef        # libjef.a
// Test:    cmake --build <build> --target jef_test && ./<build>/jef_test
//
// NOTE: v0 exposes the primitives under their jash_* names (the jash modules
// resolve these symbols by name via dlopen), so the public API IS the
// jash_crypto / blake3 surface below. A jef_* rename is reserved for the
// format-layer extraction (W3b) to avoid breaking module symbol resolution.

#ifndef LIBJEF_JEF_H
#define LIBJEF_JEF_H

#include <module/jash_crypto.h>   /* SHA-256, HMAC-SHA256, session key, BLAKE3 */
#include <module/blake3.h>        /* BLAKE3 incremental / keyed / ratchet      */
#include "ed25519.h"              /* Ed25519 (RFC 8032) sign/verify + SHA-512   */

#define JEF_VERSION_MAJOR 0
#define JEF_VERSION_MINOR 1
#define JEF_VERSION_STRING "0.1"

#endif /* LIBJEF_JEF_H */
