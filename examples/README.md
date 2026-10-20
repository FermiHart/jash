# jash verifiable execution — examples

**Verify, don't trust.** This directory contains a *real* captured jash agent
session and shows how **anyone** can cryptographically verify it — with only the
public key, no secret, and (with `jef_verify`) **without even installing jash**.

This is the concrete form of the thesis: *the verification engine is the
product*. The format is open ([`../SPEC.md`](../SPEC.md)); jash is one producer
and [`libjef`](../libjef/) is one verifier — and so could you be.

---

## The 30-second demo

```bash
# Build the standalone verifier (links ONLY libjef — no jash):
cmake -B ../build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build ../build --target jef_verify   # -> ../build/jef_verify

V=../build/jef_verify

# 1) Verify a real agent session — chain integrity, then the signature:
$V chain session-01/chain.log               # chain OK — 6 entries verified
$V proof session-01/session.jashproof       # proof VALID (ed25519, pubkey-only)
$V proof session-01/session.jashproof.dsse  # proof VALID (DSSE / in-toto envelope)

# 2) Tamper-evidence — we changed one word; verification REJECTS it:
$V chain tampered/chain.log                 # chain BROKEN          (rc 1)
$V proof tampered/session.jashproof.bad     # proof INVALID         (rc 1)
```

Or just run [`./verify.sh`](verify.sh) (builds + runs all of the above).

---

## What's here

| File | What it is |
|------|-----------|
| `session-01/chain.log` | the BLAKE3 hash-chain of a real 6-command session (forward-secure ratchet tags) |
| `session-01/session.jashproof` | the Ed25519 session proof (text form, self-describing `pubkey:`) |
| `session-01/session.jashproof.dsse` | the same proof as a **DSSE / in-toto** envelope |
| `session-01/pubkey.hex` | the signer's **public** key (the secret seed was discarded) |
| `tampered/` | the same artifacts with one byte changed — to prove rejection |
| `verify.sh` | one-shot: build `jef_verify` + verify real (pass) and tampered (fail) |
| `verify_minimal.py` | a ~70-line **reference** verifier in pure Python (follows `SPEC.md`) |

---

## What each check proves

| Check | Proves |
|-------|--------|
| `chain` verifies | every command is bound into a tamper-evident hash chain: `hₙ = BLAKE3(hₙ₋₁ ‖ ts ‖ rc ‖ cmd)` |
| `proof` verifies with **only `pubkey.hex`** | **third-party / non-repudiable**: anyone with the public key confirms the signer produced exactly this session — no shared secret |
| tampered → rejected | flipping a single byte (in a command, exit code, or proof field) is caught |
| `jef_verify` needs no jash | the artifacts are **sovereign and portable** — verifiable by any tool that implements the open format |

---

## Verify it yourself, your way

You don't need our tools. The formats are fully specified in
[`../SPEC.md`](../SPEC.md). [`verify_minimal.py`](verify_minimal.py) is a
from-scratch reference that re-derives the chain and checks the Ed25519
signature using standard libraries (`pip install blake3 pynacl`) — proof that
the format is interoperable, not jash-specific.

> **Honesty note:** the canonical, CI-tested verifier is `jef_verify`
> (`tests/test_libjef_verify.sh`). `verify_minimal.py` is a spec reference for
> other languages; run it with the two libraries above.
