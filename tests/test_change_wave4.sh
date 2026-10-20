#!/usr/bin/env bash
# tests/test_change_wave4.sh — ADR-0003 Wave 4: apply/archive/rewind integration
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# setup
$JASH -c 'change schema init spec-driven' >/dev/null 2>&1
$JASH -c 'change propose add-dark-mode && change ff add-dark-mode' >/dev/null 2>&1

# ── apply requires all artifacts ready ───────────────────────────────────────
out="$TMP/apply.out"
$JASH -c 'change apply add-dark-mode' > "$out" 2>&1
if grep -q "is now applying" "$out" && grep -q "checkpoint" "$out"; then
    ok "change apply checkpoints and marks applying"
else
    err "change apply did not checkpoint/mark applying"
    cat "$out"
fi

# ── apply rejects incomplete change ──────────────────────────────────────────
$JASH -c 'change propose incomplete' >/dev/null 2>&1
out="$TMP/apply-incomplete.out"
$JASH -c 'change apply incomplete' > "$out" 2>&1
if grep -q "is not ready" "$out"; then
    ok "change apply rejects incomplete change"
else
    err "change apply did not reject incomplete change"
    cat "$out"
fi

# ── archive creates checkpoint and moves change ──────────────────────────────
out="$TMP/archive.out"
$JASH -c 'change archive add-dark-mode' > "$out" 2>&1
if grep -q "checkpoint" "$out" && grep -q "archived" "$out" && [ ! -d "$TMP/.changes/add-dark-mode" ]; then
    ok "change archive checkpoints and moves change"
else
    err "change archive did not checkpoint/move"
    cat "$out"
fi

# ── rewind restores archived change to active ────────────────────────────────
archive_name=$(ls "$TMP/.changes/archive/" | head -1)
out="$TMP/rewind.out"
$JASH -c "change rewind $archive_name" > "$out" 2>&1
if [ -d "$TMP/.changes/add-dark-mode" ] && ! ls "$TMP/.changes/archive/" 2>/dev/null | grep -q "add-dark-mode"; then
    ok "change rewind restores archived change"
else
    err "change rewind did not restore archive"
    cat "$out"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 4 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 4 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 4 tests FAILED"
    exit 1
fi
