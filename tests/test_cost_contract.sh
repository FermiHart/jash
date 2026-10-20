#!/usr/bin/env bash
# tests/test_cost_contract.sh — ADR-0026 Tier 2: cost + contract.
set -e

JASH="${JASH:-./build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. cost produces numeric JSON.
out="$TMP/cost.json"
$JASH -c 'cost --json echo hello' > "$out" 2>/dev/null
if python3 -m json.tool "$out" >/dev/null 2>&1 && grep -q '"total_usd"' "$out"; then
    ok "cost --json emits valid JSON"
else
    err "cost --json invalid"
fi

# 2. contract passes on matching output.
if $JASH -c 'contract --expect rc 0 --expect stdout-contains hello -- echo hello' >/dev/null 2>&1; then
    ok "contract passes on matching output"
else
    err "contract failed on matching output"
fi

# 3. contract fails on mismatch.
if $JASH -c 'contract --expect rc 0 --expect stdout-contains bye -- echo hello' >/dev/null 2>&1; then
    err "contract passed on mismatch"
else
    ok "contract fails on mismatch"
fi

# 4. contract accepts expected non-zero rc.
if $JASH -c 'contract --expect rc 1 -- false' >/dev/null 2>&1; then
    ok "contract accepts expected non-zero rc"
else
    err "contract rejected expected non-zero rc"
fi

# 5. contract enforces stderr-empty.
if $JASH -c 'contract --expect stderr-empty -- echo ok' >/dev/null 2>&1; then
    ok "contract stderr-empty passes when clean"
else
    err "contract stderr-empty false positive"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  cost/contract RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ cost/contract tests PASSED"
    exit 0
else
    echo "  ✗ cost/contract tests FAILED"
    exit 1
fi
