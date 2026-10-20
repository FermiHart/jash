# jash Verifiable Execution — Public Specification

**Version:** draft-1 (covers `jashproof/1`, `JMODv01`, libjef 0.1)
**Status:** living document — formats are versioned; breaking changes bump the version tag.

jash can produce a **tamper-evident, cryptographically chained record of every
command it executes**. This spec defines the on-disk formats and the
verification procedure so that *independent* tooling — not just jash — can read
and check the artifacts.

> Design principle (ADR-0003): *the verification engine is the product.* The
> primitives live in [`libjef`](libjef/README.md), a dependency-free C library;
> the shell is one frontend.

---

## 1. Sigil levels

The recorder is a 4-level dial (`sigil <off|log|sign|seal>` or `JASH_SIGIL=<0-3>`):

| Level | Name | Effect |
|------:|------|--------|
| 0 | off  | no recording — zero overhead, byte-identical hot path |
| 1 | log  | append every command to the repository-scoped BLAKE3 hash chain |
| 2 | sign | LOG + emit a session proof (`~/.jash/session.jashproof`) at exit |
| 3 | seal | SIGN + adversarial battery (4 attacks refuted) before a sealed restore |

When `JASH_SIGIL>=1`, jash auto-loads `sigil.modjash` and routes **every**
command through the attestation hook (the `-c` fast paths are bypassed so no
command escapes the chain).

---

## 2. Chain format — `~/.jash/chain-<repo>.log`

Outside a repository, the chain path falls back to `~/.jash/chain.log`.

Append-only. One line per executed command:

```
<seq> <sec>.<nsec> <hash> tag=<tag> rc=<rc> cwd=<cwd> repo=<repo> dur=<ms> sess=<session> cmd=<escaped command>
```

| Field | Meaning |
|-------|---------|
| `seq` | monotonic counter from 0 |
| `sec.nsec` | wall-clock timestamp (CLOCK_REALTIME) |
| `hash` | 64-hex BLAKE3 chain hash (see below) |
| `tag` | 64-hex forward-secure ratchet MAC |
| `rc` | command exit code |
| `cwd`, `repo`, `dur`, `sess` | display context; not part of the chain hash |
| `cmd` | command escaped before hashing and writing (`\\`, `\n`, `\r`) |

A `SESSION_END uptime=<ms>ms` sentinel line is appended when the shell exits.

### Chain hash

```
hₙ = BLAKE3( hₙ₋₁ ‖ payload )
payload = ts_ns(8 bytes, big-endian) ‖ rc(4 bytes, little-endian) ‖ cmd
h₋₁ = 32 zero bytes
```

### Forward-secure ratchet tag

```
tagₙ = BLAKE3_keyed( kₙ , hₙ )
kₙ₊₁ = BLAKE3( kₙ ‖ "r" )          ; kₙ is then erased
```

The ratchet key seed `k₀` is stored at `~/.jash/ratchet-<repo>.key` (mode
0600) for a repository-scoped chain. Outside a repository, the fallback is
`~/.jash/ratchet.key`.
Forward security: compromising `kₙ` does not let an attacker forge tags for
entries `< n` (each key is derived one-way from the previous).

The ledger entry is flushed and synchronized before the next ratchet
generation is atomically renamed into place. A failed ratchet publication
therefore leaves a visible extra entry that verification rejects; it never
acknowledges an entry whose position witness was not committed.

### Interrupted tail recovery

`sigil quarantine apply --json` may salvage only a suffix made entirely of
physical-line fragments after the final structured entry. The exact suffix is
copied into `~/.jash/quarantine/` with a SHA-256 manifest, then the active file
is truncated and synchronized at the retained byte offset. The valid prefix
and scoped ratchet remain byte-identical. A fragment followed by another
structured entry is middle corruption and remains fail-closed; automatic
salvage is refused.

### Ledger/witness mismatch recovery

The structural status scan deliberately does not re-hash the complete ledger or
compare its entry count with the forward-secure ratchet. The explicit
`sigil quarantine apply --json` operation performs that full verification. A
conclusive `hash_mismatch`, `log_truncated`, or `entries_forged` verdict moves
the complete active chain and its scoped ratchet into quarantine with their
exact failure reason and SHA-256 manifest, then creates a fresh pair. It never
edits the surviving prefix or adjusts the witness to make verification pass.
Transient snapshot, lock, open, or read failures do not trigger quarantine.

### Verifying the chain

Recompute `hₙ` from `hₙ₋₁` and the recorded `(ts, rc, cmd)` for every line; any
mismatch means tampering. `sigil chain verify` does this and additionally
flags timestamp rollback. Tamper of any byte in any `cmd`/`rc`/order is
detected (see `tests/test_sigil.sh`).

---

## 3. Proof format — `~/.jash/session.jashproof`

A text attestation over a whole session, emitted at exit (level >= sign) or via
`sigil proof emit`:

```
jashproof/1
session_id: <16-hex>           ; BLAKE3(start_ts)[0:8]
start_ts: <sec>.<nsec>
end_ts: <sec>.<nsec>
user: <username>
host: <hostname>
chain_file: <path>
chain_entries: <N>             ; non-sentinel lines
chain_tip: <64-hex>            ; final BLAKE3 chain hash
pubkey: <64-hex>               ; Ed25519 public key (self-describing)
sig_algo: ed25519
sig: <128-hex>                 ; Ed25519 signature over all preceding bytes
```

`sig = Ed25519_sign(secret_key, body)`. The signing identity is a persistent
Ed25519 seed at `~/.jash/keys/ed25519.seed` (mode 0600); the matching **public
key is embedded in the proof**. `sigil proof verify` parses the pubkey + sig and
runs `Ed25519_verify` — **it needs only the public key, no secret** — then
checks `chain_tip` against the live chain.

> **Third-party-verifiable.** Unlike a symmetric MAC, anyone can verify a
> `sig_algo: ed25519` proof with only the public key — including against the
> key holder (non-repudiation). For real trust, a verifier should **pin** the
> expected pubkey out-of-band (the proof is self-describing, so it states which
> key signed it; it does not by itself bind that key to an identity).
>
> Legacy `sig_algo: blake3-keyed` proofs (symmetric, pre-W4) still verify via
> the session key — `sigil proof verify` dispatches on `sig_algo`.

### DSSE / in-toto envelope

`sigil proof dsse` emits the proof wrapped in a **DSSE** (Dead-Simple Signing
Envelope — the in-toto/sigstore standard) at `~/.jash/session.jashproof.dsse`:

```json
{
  "payloadType": "application/vnd.jash.jashproof.v1+text",
  "payload": "<base64 of the proof body>",
  "signatures": [ { "keyid": "<ed25519 pubkey hex>", "sig": "<base64 sig>" } ]
}
```

The Ed25519 signature is over the **DSSE PAE** (`"DSSEv1" SP len(type) SP type
SP len(payload) SP payload`), so any standard DSSE verifier accepts it.
`sigil proof verify <file.dsse>` auto-detects the envelope (leading `{`),
rebuilds the PAE, and verifies with the embedded public key only.

The Ed25519 implementation is a public-domain TweetNaCl port (RFC 8032),
vendored in [`libjef`](libjef/) and exercised by `tests/test_libjef.sh`.

---

## 4. Module format — `.modjash` (`JMODv01`)

`[ header 64B ][ manifest ][ body (PIC .dylib/.so) ][ optional signature ]`

Header (little-endian, packed):

| Off | Size | Field |
|----:|-----:|-------|
| 0 | 8 | magic `"JMODv01\0"` |
| 8 | 4 | abi_version (=1) |
| 12 | 4 | flags (`SIGNED`=1, `OPTIONAL`=2, `AUTOLOAD`=4) |
| 16 | 32 | BLAKE3 of body (zeros = unverified in M1) |
| 48 | 4 | manifest_len |
| 52 | 4 | body_len |
| 56 | 4 | sig_len (0 = none) |
| 60 | 4 | reserved |

Manifest: `key=value` lines (`name`, `version`, `caps`, `description`, …).
`caps` is a hex bitmap of declared capabilities; the loader refuses a module
whose runtime `caps_used` exceeds its declared `caps` (capability gate). The
body exports one symbol `jmod_ops` (`jmod_ops_t`: init/shutdown/dispatch +
pre/post-cmd/exit/mutation hooks). See `c/module/modjash.h`.

---

## 5. libjef API (0.1)

Pure-C11, dependency-free. Primitives: SHA-256, HMAC-SHA256, BLAKE3
(hash/keyed/xof), `*_chain` (hash chaining), `*_ratchet` (forward-secure key
advance). Header: [`libjef/jef.h`](libjef/jef.h). Build `cmake --build B
--target jef` → `libjef.a`; standalone test `tests/test_libjef.sh`.

### Concrete job specifications

`jef_verify_job_spec` (declared in `libjef/jef_verify.h`) binds exact caller-supplied
spec bytes to a pinned start receipt and its terminal receipt. CLI:

```sh
jef_verify job-spec PUBKEY SPEC START_REQUEST START_RESPONSE START_RECEIPT TERMINAL_REQUEST TERMINAL_RESPONSE TERMINAL_RECEIPT
```

The start request must contain one top-level `execution_spec_sha256` equal to
`sha256:` plus the lowercase SHA-256 of **all** spec bytes, including whitespace.
The inclusive limit is 1 MiB; the spec is opaque, so empty/binary regular files
are accepted. Both receipts, their same-job/start link, request/response bytes,
and terminal stdout/stderr CAS objects are verified. The trusted key must come
from the caller, not merely from the artifact being verified. Inputs and linked
artifacts must remain immutable during verification.

DSSE signature verification and receipt interpretation share one structural
decoder. This profile requires exactly `payloadType`, `payload`, and
`signatures`, with one signature containing exactly `keyid` and `sig`; duplicate
decoded keys, shadow fields, malformed JSON and noncanonical base64 are refused.
Valid escaped JSON member names remain supported.

Success means **bound provenance**, not successful execution or enforcement of
the opaque spec. Failed/cancelled terminal jobs can have valid provenance. The
experimental harness supports `jash.autonomy.v1` and opt-in `jash.autonomy.v2`,
and additionally validates versioned policy, signed harness observations and
candidate bytes before eligibility; see [AUTONOMY.md](Docs/AUTONOMY.md). It does
not change the session-chain format or make `speculate` a kernel sandbox.

### Aggregate Resource Contract

`jash.autonomy.v2` is enabled only by explicit `run --cgroup-root` naming an
approved canonical absolute delegated cgroup-v2 directory. Default v1 remains
without quota claims; existing bundles and publication journals retain their
verification/recovery contracts. Optional `--resources` requires this opt-in and
exactly five unique JSON integer fields, with no booleans, floats or extra keys:

| Field | Default | Inclusive Bounds |
| --- | --- | --- |
| `memory_max_bytes` | `134217728` (128 MiB) | `16777216..1099511627776` (16 MiB..1 TiB) |
| `pids_max` | `64` | `1..4096` |
| `cpu_quota_us` | `50000` | `1000..cpu_period_us` |
| `cpu_period_us` | `100000` | `1000..1000000` |
| `wall_timeout_ms` | `--timeout-ms` | `1..30000` |

The trusted daemon-startup `JASH_AGENT_CGROUP_ROOT` is pinned by directory fd,
protected from request-environment overrides and removed from job payload env.
The supplied parent must already delegate `cpu/memory/pids` to children; the
runtime does not configure shared/system delegation. Handshake configuration is
not enforcement evidence: each resource job must configure and exactly read back
its own cgroup limits, including fixed `memory.swap.max=0`, `memory.oom.group=1`,
and verify usable event/population/kill interfaces, or refuse. Workers remain gated before
payload redirections/exec until attachment, population admission and parent
acknowledgement succeed; EOF is rejection. The group reaper stays outside limits.

Start and terminal responses carry `resource_backend: "cgroup-v2"`,
`resource_enforced: true` and the effective five-field `resources` object. Terminal
observations are booleans `resource_timed_out`, `resource_violation`,
`resource_empty`, `descendants_forced`, and nonnegative integers
`resource_memory_events`, `resource_pids_events`. The event counts are sticky
observations of `memory.events max` and `pids.events max`, even after handled errno
and final `rc: 0`; `resource_violation` also includes deadline expiry. Forced
descendant cleanup is separate. These fields are authenticated through the existing
receipt **response hash**, not a new DSSE payload schema. `libjef job-spec` remains
provenance-only; v2 policy verification additionally requires matching enforcement
observations and, for eligibility, no violations/timeouts/events/forced descendants
and an empty resource group, alongside existing success checks.

Trust remains in the host kernel, daemon, harness and approved delegation. Memory
limits cover aggregate cgroup charges, including charged tmpfs, not universal RSS
or disk/filesystem quotas. PID limits count processes and threads. CPU is bandwidth
(default one half CPU), not cumulative CPU time; throttling is not a violation.
The supervisor wall deadline is independent of client polling, not hard-real-time.
Cgroups alone are not a security sandbox: payloads must lack writable cgroupfs and
inherited access descriptors or same-UID migration can escape limits. See
[Aggregate Resource Mode](Docs/AUTONOMY.md#aggregate-resource-mode) for operator
requirements, exact observation semantics, compatibility and delegation-dependent
tests; exit-77 skips do not establish enforcement or production readiness.

---

## 6. Distribution

`sigil export [directory] --manifest <path>` first verifies and snapshots the
active repository-scoped chain, then packages the exact chain and caller
manifest bytes as an OCI image layout. Its DSSE payload binds the caller
manifest SHA-256/media type and the chain SHA-256/tip/count under the sovereign
Ed25519 identity reported by `sigil proof identity --json`. Invalid or
ratchet-inconsistent chains are refused before the destination is created.

Without a directory, export uses `~/.jash/oci-export/`. `sigil push <registry>`
ships the layout via `oras`; any OCI-compliant registry can store and serve it.

---

## 7. Verifier checklist (for independent tooling)

1. Parse `chain.log`; recompute `hₙ = BLAKE3(hₙ₋₁ ‖ ts_ns_be ‖ rc_le ‖ cmd)`
   from `h₋₁ = 0³²`; assert each recorded `hash` matches.
2. Assert timestamps are non-decreasing.
3. If you hold the ratchet seed, recompute `tagₙ` and the ratchet to verify MACs.
4. Parse `session.jashproof`; assert `chain_tip` equals the recomputed final
   hash; verify `sig` per `sig_algo` — for `ed25519`, `Ed25519_verify(pubkey,
   body, sig)` using only the embedded public key (pin it out-of-band for trust).

---

## 8. Background jobs — `~/.jash/jobs/` (ADR-0042 F6)

`jash --bg -c '<cmd>'` runs `<cmd>` in a detached child (`setsid`) with full sigil
attestation — the backgrounded command appears in `chain.log` like any other, so
§7's verifier checklist covers it unchanged. On-disk state:

- `~/.jash/jobs/nextid` — decimal ASCII; the next id to allocate, bumped under a
  `flock` on `~/.jash/jobs/.lock`.
- `~/.jash/jobs/<id>.json` — a single-line JSON record, written **atomically**
  (temp + `rename`). Running:
  `{"id":N,"state":"running","pid":P,"start_ts":T,"cmd":"…"}`. Finalized on child
  exit: `{"id":N,"state":"done","rc":R,"ms":M,"pid":P,"start_ts":T,"end_ts":T2,`
  `"notified":B,"cmd":"…"}`. `cmd` is JSON-escaped; `notified` flips to `true`
  once the completion has been reported in a later invocation's `JASH_AGENT`
  envelope (`"jobs_done":[N]`, emitted exactly once per job).
- `~/.jash/jobs/<id>.out` — the job's combined stdout+stderr.

`wait <id>` blocks on the record until `state:"done"` and returns `rc`; a
`running` record whose `pid` is gone is finalized as `rc:-1` (no hung wait).
`jobs --json` lists the merged records; `jobs out <id>` streams `<id>.out`.
