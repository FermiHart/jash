#!/usr/bin/env bash
# verify.sh — verify the example artifacts with jef_verify (links only libjef).
# Real session must PASS; tampered copies must be REJECTED.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
V="$ROOT/build/jef_verify"

if [ ! -x "$V" ]; then
    echo "building jef_verify (standalone, links only libjef)…"
    cmake -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || true
    cmake --build "$ROOT/build" --target jef_verify >/dev/null 2>&1 || {
        echo "could not build jef_verify"; exit 1; }
fi

EX="$HERE/session-01"; T="$HERE/tampered"
pass=0; fail=0
expect(){ # want_rc label cmd...
    local want="$1" label="$2"; shift 2
    "$@" >/dev/null 2>&1; local rc=$?
    if [ "$rc" = "$want" ]; then pass=$((pass+1)); echo "  ✓ $label"; else
        fail=$((fail+1)); echo "  ✗ $label (rc=$rc, wanted $want)"; fi
}

echo "== real session (must verify) =="
expect 0 "chain intact"        "$V" chain "$EX/chain.log"
expect 0 "ed25519 proof valid" "$V" proof "$EX/session.jashproof"
expect 0 "DSSE proof valid"    "$V" proof "$EX/session.jashproof.dsse"

echo "== tampered (must be rejected) =="
expect 1 "tampered chain BROKEN"   "$V" chain "$T/chain.log"
expect 1 "tampered proof INVALID"  "$V" proof "$T/session.jashproof.bad"

echo "== verify.sh: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
