#!/usr/bin/env bash
# tests/test_change_wave6.sh — ADR-0003 Wave 6: goal + recall integration
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

$JASH -c 'change schema init spec-driven' >/dev/null 2>&1
$JASH -c 'change propose goal-demo --goal goal-123' >/dev/null 2>&1

# ── goal persists in change metadata ────────────────────────────────────────
if grep -q "goal: goal-123" "$TMP/.changes/goal-demo/.change.yaml"; then
    ok "change propose --goal persists goal uuid"
else
    err "change propose --goal did not persist goal uuid"
    cat "$TMP/.changes/goal-demo/.change.yaml"
fi

# ── status shows goal linkage ───────────────────────────────────────────────
out="$TMP/status.out"
$JASH -c 'change status' > "$out" 2>&1
if grep -q "goal=goal-123" "$out"; then
    ok "change status shows goal linkage"
else
    err "change status did not show goal linkage"
    cat "$out"
fi

# ── recall emits metadata and artifact readiness ────────────────────────────
out="$TMP/recall.out"
$JASH -c 'change recall goal-demo' > "$out" 2>&1
if grep -q "change recall" "$out" && grep -q "goal-123" "$out" && grep -q "proposal" "$out"; then
    ok "change recall emits metadata, goal, and artifacts"
else
    err "change recall output incomplete"
    cat "$out"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 6 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 6 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 6 tests FAILED"
    exit 1
fi
