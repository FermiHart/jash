#!/usr/bin/env bash
# test_sigil.sh — verifiable execution chain (ADR-0003 / ROADMAP W1.1)
# Asserts that JASH_SIGIL>=1 produces a real BLAKE3 chain + HMAC proof, that the
# chain verifies, and that tampering is detected. Runs each shell invocation in
# its own process (the post_cmd hook writes the chain at top-level eval end, so
# verification must happen in a SEPARATE process that resumes the chain).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"
[ -x "$J" ] || J="$ROOT/build-speed/jash-release"

# Isolated HOME so we never touch the real ~/.jash. Module resolves via the dev
# tree (./modules/sigil/sigil.modjash) since we cd to repo root.
TMP_HOME="$(mktemp -d)"
trap 'rm -rf "$TMP_HOME"' EXIT
bash "$ROOT/tests/setup_sigil_modules.sh" "$TMP_HOME"
cd "$ROOT"

PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; }

run(){ HOME="$TMP_HOME" JASH_SIGIL=2 "$J" -c "$1" 2>&1 | grep -vE '^\[jash'; }

echo "== sigil verifiable-execution chain =="

# 1. A session writes the chain on exit.
run 'echo one; echo two; echo three' >/dev/null
# ADR-0051 W6: inside a repo the chain is per-repo (chain-<key>.log); outside,
# the historic chain.log. Match whichever this cwd produced.
CHAIN="$(ls "$TMP_HOME"/.jash/chain*.log 2>/dev/null | head -1)"
if [ -n "$CHAIN" ] && [ -f "$CHAIN" ]; then ok "chain.log created"; else no "chain.log created"; fi

# 2. Chain lines carry a BLAKE3 hash + ratchet tag.
if grep -q 'tag=' "$CHAIN" 2>/dev/null; then ok "chain entries carry ratchet tag"; else no "ratchet tag present"; fi

# 3. Fresh process verifies the chain.
V="$(run 'sigil chain verify')"
if echo "$V" | grep -qiE 'valid|ok|verified'; then ok "sigil chain verify passes"; else no "sigil chain verify ($V)"; fi

# 4. Tamper detection: flip a byte in a cmd= field, expect verify to fail.
cp "$CHAIN" "$CHAIN.bak"
# corrupt the first data line's command text
awk 'NR==1{sub(/cmd=.*/,"cmd=TAMPERED")}1' "$CHAIN.bak" > "$CHAIN"
VT="$(run 'sigil chain verify'; echo "rc=$?")"
if echo "$VT" | grep -qiE 'fail|tamper|mismatch|invalid|broken'; then ok "tamper detected"; else no "tamper NOT detected ($VT)"; fi
cp "$CHAIN.bak" "$CHAIN"   # restore good chain

# 5. Proof emit + verify round-trip.
run 'sigil proof emit' >/dev/null
PV="$(run 'sigil proof verify')"
if echo "$PV" | grep -qiE 'valid|✓'; then ok "proof emit+verify round-trip"; else no "proof verify ($PV)"; fi

# 6. session.jashproof exists.
if [ -f "$TMP_HOME/.jash/session.jashproof" ]; then ok "session.jashproof present"; else no "session.jashproof present"; fi

# --- Wave 4: sovereign Ed25519 proof (third-party-verifiable) ---
PROOF="$TMP_HOME/.jash/session.jashproof"
if grep -q 'sig_algo: ed25519' "$PROOF" && grep -q 'pubkey: ' "$PROOF"; then
    ok "proof is ed25519 with self-describing pubkey"
else no "proof ed25519/pubkey"; fi
# The killer property: verify with ONLY the public key — delete the secret seed.
rm -f "$TMP_HOME/.jash/keys/ed25519.seed" "$TMP_HOME/.jash/session.key"
PVS="$(run 'sigil proof verify')"
if echo "$PVS" | grep -qiE 'valid|✓'; then ok "verifies with pubkey only (secret deleted)"; else no "pubkey-only verify ($PVS)"; fi
# Tamper the signed body -> must fail.
sed -i.bak 's/^user: /user: HACKED/' "$PROOF" 2>/dev/null
PVT="$(run 'sigil proof verify')"
if echo "$PVT" | grep -qiE 'INVALID|✗'; then ok "tampered proof rejected (ed25519)"; else no "tamper not rejected ($PVT)"; fi
cp "$PROOF.bak" "$PROOF" 2>/dev/null

# --- W4-intoto: DSSE envelope (in-toto/sigstore interop) ---
DSSE="$TMP_HOME/.jash/session.jashproof.dsse"
run 'sigil proof dsse' >/dev/null
if grep -q '"payloadType"' "$DSSE" 2>/dev/null && grep -q '"signatures"' "$DSSE" 2>/dev/null; then
    ok "DSSE envelope emitted"; else no "DSSE emit"; fi
DV="$(run "sigil proof verify $DSSE")"
if echo "$DV" | grep -qiE 'valid|✓'; then ok "DSSE verifies (pubkey only)"; else no "DSSE verify ($DV)"; fi
sed -i.bak 's/"payload": "./"payload": "Z/' "$DSSE" 2>/dev/null
DVT="$(run "sigil proof verify $DSSE")"
if echo "$DVT" | grep -qiE 'INVALID|✗'; then ok "tampered DSSE rejected"; else no "DSSE tamper not rejected ($DVT)"; fi

# --- Wave 2: single-command chaining + log UX (history/diff/bisect) ---
# 6a. A SINGLE simple command must be chained (fast-path bypass fix).
before=$(grep -c 'cmd=' "$CHAIN" 2>/dev/null || echo 0)
run 'echo SINGLEMARK' >/dev/null
if run 'sigil history' | grep -q 'SINGLEMARK'; then ok "single command chained (no fast-path bypass)"; else no "single command chained"; fi

# 6b. A failure is recorded and bisect finds it.
run 'false' >/dev/null
B="$(run 'sigil bisect')"
if echo "$B" | grep -qiE 'first failure|rc=1'; then ok "bisect finds first failure"; else no "bisect ($B)"; fi

# 6c. diff shows a command/rc delta between two seqs (strip ANSI before matching).
D="$(run 'sigil diff 0 1' | sed $'s/\x1b\\[[0-9;]*m//g')"
if echo "$D" | grep -qE '^[-+] '; then ok "diff shows command delta"; else no "diff ($D)"; fi

# 7. Zero-overhead when off: no chain at JASH_SIGIL=0 (explicit, so the ambient
#    JASH_SIGIL from .claude/settings.json can't leak into the test).
rm -f "$CHAIN"
HOME="$TMP_HOME" JASH_SIGIL=0 "$J" -c 'echo nochain' >/dev/null 2>&1
if [ ! -f "$CHAIN" ]; then ok "no chain when sigil off"; else no "chain written when off (bad)"; fi

echo "== sigil: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
