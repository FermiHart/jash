#!/usr/bin/env bash
# tests/test_budget_builtin.sh — hard output-budget enforcement for builtins.
set -e

JASH="${JASH:-./build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. digest respects JASH_LLM_BUDGET and emits governor message.
out="$TMP/digest.out"
JASH_LLM_BUDGET=1500 $JASH -c 'digest --brief' > "$out" 2>/dev/null
if grep -q "\[context-governor\]" "$out"; then
    ok "digest emits governor message when capped"
else
    err "digest did not emit governor message"
fi

# 2. Structured state returns a complete failure document, never a cut prefix.
out="$TMP/state.out"
state_rc=0
JASH_LLM_BUDGET=64 $JASH -c 'state --json' > "$out" 2>/dev/null || state_rc=$?
if [ "$state_rc" -eq 75 ] && python3 - "$out" <<'PY'
import json, sys
value = json.load(open(sys.argv[1]))
assert value["complete"] is False
assert value["error"]["code"] == "output_budget_exceeded"
assert value["error"]["required_bytes"] > value["error"]["budget_bytes"]
PY
then
    ok "state reports structured budget exhaustion"
else
    err "state failed to preserve the JSON budget contract"
fi

# 3. context map respects JASH_LLM_BUDGET.
out="$TMP/context.out"
JASH_LLM_BUDGET=1000 $JASH -c 'context map' > "$out" 2>/dev/null
if grep -q "\[context-governor\]" "$out"; then
    ok "context map emits governor message when capped"
else
    err "context map did not emit governor message"
fi

# 4. Without budget, output is larger than the cap used above.
out="$TMP/digest_full.out"
$JASH -c 'digest --brief' > "$out" 2>/dev/null
if [ "$(wc -c < "$out")" -gt 1500 ]; then
    ok "digest without budget produces full output"
else
    err "digest output unexpectedly small without budget"
fi

# 5. Overflow file is created.
rm -f "$HOME/.jash/overflow"/*-digest.txt
JASH_LLM_BUDGET=1000 $JASH -c 'digest --brief' > /dev/null 2>/dev/null
if ls "$HOME/.jash/overflow"/*-digest.txt >/dev/null 2>&1; then
    ok "digest creates overflow file"
else
    err "digest did not create overflow file"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  builtin budget RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ builtin budget tests PASSED"
    exit 0
else
    echo "  ✗ builtin budget tests FAILED"
    exit 1
fi
