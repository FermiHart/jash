#!/usr/bin/env bash
# test_libjef_verify.sh — third-party verification proof.
# jash PRODUCES a chain + Ed25519 proof + DSSE envelope; the standalone
# `jef_verify` tool (links ONLY libjef, no jash) independently VERIFIES them.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"; [ -d "$BUILD" ] || BUILD="$ROOT/build-speed"
J="$BUILD/jash"; [ -x "$J" ] || J="$ROOT/build/jash"
V="$BUILD/jef_verify"
TMP_HOME="$(mktemp -d "${TMPDIR:-/tmp}/jash-libjef-verify.XXXXXX")" || exit 2
trap 'rm -rf "$TMP_HOME"' EXIT

cmake --build "$BUILD" --target jef_verify -j4 >"$TMP_HOME/build.log" 2>&1 || {
    echo "jef_verify build failed"; tail -20 "$TMP_HOME/build.log"; exit 1; }

bash "$ROOT/tests/setup_sigil_modules.sh" "$TMP_HOME"
cd "$ROOT"
run(){ HOME="$TMP_HOME" JASH_SIGIL=2 "$J" -c "$1" 2>&1 | grep -vE '^\[jash|load failed|flat namespace'; }

PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }

echo "== libjef independent verification of jash artifacts =="
run 'echo one' >/dev/null; run 'echo two' >/dev/null
run 'sigil proof emit' >/dev/null
run 'sigil proof dsse' >/dev/null

set -- "$TMP_HOME"/.jash/chain-*.log
C="$1"
P="$TMP_HOME/.jash/session.jashproof"
D="$TMP_HOME/.jash/session.jashproof.dsse"

"$V" chain "$C" >/dev/null  && ok "jef_verify: chain intact"            || no "chain" "$("$V" chain "$C")"
"$V" proof "$P" >/dev/null  && ok "jef_verify: text ed25519 proof"      || no "proof" "$("$V" proof "$P")"
"$V" proof "$D" >/dev/null  && ok "jef_verify: DSSE envelope"           || no "dsse" "$("$V" proof "$D")"

# Tamper detection (independent verifier must reject).
cp "$C" "$C.bak"; sed 's/cmd=echo one/cmd=echo EVIL/' "$C.bak" > "$C"
if "$V" chain "$C" >/dev/null; then no "tamper-chain" "accepted tampered chain"; else ok "jef_verify: rejects tampered chain"; fi
cp "$C.bak" "$C"
cp "$P" "$P.bak"; sed 's/^user: /user: EVIL/' "$P.bak" > "$P"
if "$V" proof "$P" >/dev/null; then no "tamper-proof" "accepted tampered proof"; else ok "jef_verify: rejects tampered proof"; fi

echo "== libjef-verify: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
