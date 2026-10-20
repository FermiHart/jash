# libjef — Jash Execution Fabric

The verification engine of jash, factored out as a standalone, dependency-free
C library so **any** program can embed tamper-evident execution attestation —
not just the shell. (ADR-0003 §2.1: *"libjef primeiro, shell como frontend — a
engine de verificação é o produto."*)

## What it provides (v0.1)

Pure-C11, freestanding, zero external dependencies:

| Primitive | Use |
|-----------|-----|
| SHA-256 + HMAC-SHA256 | symmetric integrity / keyed MAC |
| BLAKE3 (hash / keyed / XOF) | fast hashing |
| hash chaining (`*_chain`) | append-only audit chain: `hₙ = H(hₙ₋₁ ‖ payload)` |
| forward-secure ratchet | per-step key advance, one-way |
| Ed25519 (RFC 8032) | sovereign asymmetric signing (sign/verify/keygen) |
| **independent verifier** | `jef_verify_chain` / `jef_verify_proof` — check a jash chain or Ed25519/DSSE proof with ONLY libjef, no secret (see [`jef_verify.h`](jef_verify.h)) |

## Third-party verification (the product thesis)

`jef_verify` is a standalone CLI — links **only** libjef, no jash — that verifies
real jash attestation artifacts:

```bash
cmake --build build --target jef_verify
build/jef_verify chain ~/.jash/chain.log              # recompute the BLAKE3 chain
build/jef_verify proof ~/.jash/session.jashproof      # Ed25519, pubkey-only
build/jef_verify proof ~/.jash/session.jashproof.dsse # DSSE envelope
# or: bash tests/test_libjef_verify.sh  (jash produces, jef_verify checks)
```

This proves "anyone can verify" concretely: an independent binary validates
jash's chain and signatures with no shared secret.

Header: [`jef.h`](jef.h). The v0 surface is the `jash_*` crypto symbols (the
jash modules resolve these by name via `dlopen`); a `jef_*` rename ships with
the format-layer extraction (W3b).

## Build & test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jef        # -> build/libjef.a
cmake --build build --target jef_test   # standalone unit test
./build/jef_test                        # 9/9, incl. SHA-256("abc") known vector
# or: bash tests/test_libjef.sh
```

`libjef.a` is **excluded from the default build** — `cmake --build build`
(jash) does not build it. jash itself keeps compiling these sources directly so
dynamically-loaded modules still resolve the crypto symbols; libjef is the
independent, reusable deliverable.

## Roadmap

- **W3b** — move the chain/proof FORMAT layer (`chain_append`, `chain_verify`,
  `proof_emit/verify`) out of `modules/sigil/sigil_module.c` into libjef,
  parameterized by a config struct (chain dir + level) — the `jef_chain_*` /
  `jef_proof_*` API. Precondition: decouple `jash_sigil_get()` / `$HOME`.
- **W4** — `jef_ed25519_*`: sovereign asymmetric signing (third-party-verifiable
  `.jashproof`) + DSSE/in-toto envelope.
