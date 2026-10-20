#!/usr/bin/env bash
# test_governor.sh — context governor (ADR-0023 E5): JASH_LLM_BUDGET caps an
# external command's output, holds the overflow in ~/.jash/overflow/, and the
# message tells the agent to retrieve it via `recall`. Closes the producer
# (output_budget) ↔ consumer (recall) loop.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"; mkdir -p "$TH/.jash"
BIG="$TH/big.txt"; for i in $(seq 1 60); do echo "line-$i-pad-pad-pad-pad"; done > "$BIG"

echo "== context governor (ADR-0023 E5) =="

out="$(JASH_LLM_BUDGET=200 "$J" -c "/bin/cat $BIG" 2>&1 | strip)"
printf '%s' "$out" | grep -q 'context-governor' && ok 'over-budget output is capped + flagged' || no 'governor msg' "$out"
printf '%s' "$out" | grep -q 'recall' && ok 'message points to recall (loop closed)' || no 'recall hint' "$out"

# the held overflow is retrievable via recall
held="$(JASH_LLM_BUDGET=200 "$J" -c "recall tail 2" 2>&1 | strip)"
printf '%s' "$held" | grep -q 'line-60' && ok 'recall retrieves the held output' || no 'recall retrieve' "$held"

# under budget: no governor noise
small="$(JASH_LLM_BUDGET=100000 "$J" -c "/bin/echo hello-small" 2>&1 | strip)"
printf '%s' "$small" | grep -q 'hello-small' && ! printf '%s' "$small" | grep -q 'context-governor' && ok 'under budget → no governor noise' || no 'under budget' "$small"

rm -rf "$TH"
echo "== governor: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
