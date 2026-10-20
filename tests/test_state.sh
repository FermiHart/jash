#!/usr/bin/env bash
# tests/test_state.sh — ADR-0026 Tier 0: live system state mirror.
set -e

JASH="${JASH:-./build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. JSON mode emits valid envelope.
out="$TMP/state.json"
$JASH -c 'state --json' > "$out" 2>/dev/null
if python3 -m json.tool "$out" >/dev/null 2>&1; then
    ok "state --json is valid JSON"
else
    err "state --json is not valid JSON"
fi

# 2. JSON contains cwd.
if grep -q '"cwd"' "$out"; then
    ok "state JSON contains cwd"
else
    err "state JSON missing cwd"
fi

# 3. JSON contains git branch if in a repo.
if grep -q '"branch"' "$out"; then
    ok "state JSON contains git branch"
else
    err "state JSON missing git branch"
fi

# 4. Human mode emits cwd.
out="$TMP/state_human.out"
$JASH -c 'state' > "$out" 2>/dev/null
if grep -q "cwd:" "$out"; then
    ok "human state contains cwd"
else
    err "human state missing cwd"
fi

# 5. Intent is surfaced when set.
out="$TMP/state_intent.json"
$JASH -c 'intent "testing state"; state --json' > "$out" 2>/dev/null
if grep -q '"intent"' "$out" && grep -q 'testing state' "$out"; then
    ok "state JSON surfaces intent"
else
    err "state JSON missing intent"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  state RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ state tests PASSED"
    exit 0
else
    echo "  ✗ state tests FAILED"
    exit 1
fi
