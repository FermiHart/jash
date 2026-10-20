#!/usr/bin/env bash
# tests/test_change_wave2.sh — ADR-0003 Wave 2: artifact lifecycle
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
$JASH -c 'change schema init bugfix' >/dev/null 2>&1
$JASH -c 'change propose add-dark-mode' >/dev/null 2>&1
$JASH -c 'change propose fix-login --schema bugfix' >/dev/null 2>&1

# ── continue creates next ready artifact in dependency order ─────────────────
out="$TMP/continue1.out"
$JASH -c 'change continue add-dark-mode' > "$out" 2>&1
if grep -q "created artifact 'proposal'" "$out"; then
    ok "change continue creates proposal first"
else
    err "change continue did not create proposal"
    cat "$out"
fi

# ── continue respects dependencies ───────────────────────────────────────────
out="$TMP/continue2.out"
$JASH -c 'change continue add-dark-mode' > "$out" 2>&1
if grep -q "created artifact" "$out"; then
    ok "change continue creates second artifact"
else
    err "change continue did not create second artifact"
    cat "$out"
fi

# ── continue errors when nothing is ready ────────────────────────────────────
$JASH -c 'change continue add-dark-mode' >/dev/null 2>&1
$JASH -c 'change continue add-dark-mode' >/dev/null 2>&1
out="$TMP/continue-done.out"
$JASH -c 'change continue add-dark-mode' > "$out" 2>&1
if grep -q "has all artifacts" "$out"; then
    ok "change continue detects completed change"
else
    err "change continue did not detect completed change"
    cat "$out"
fi

# ── ff creates all artifacts at once ─────────────────────────────────────────
out="$TMP/ff.out"
$JASH -c 'change ff fix-login' > "$out" 2>&1
if grep -q "fast-forwarded" "$out" && \
   [ -f "$TMP/.changes/fix-login/reproduce.md" ] && \
   [ -f "$TMP/.changes/fix-login/fix.md" ] && \
   [ -f "$TMP/.changes/fix-login/verify.md" ]; then
    ok "change ff creates all bugfix artifacts"
else
    err "change ff failed"
    cat "$out"
fi

# ── status shows per-artifact progress ───────────────────────────────────────
out="$TMP/status.out"
$JASH -c 'change status' > "$out" 2>&1
if grep -q "add-dark-mode" "$out" && grep -q "fix-login" "$out" && \
   grep -q "artifacts=4/4" "$out" && grep -q "artifacts=3/3" "$out"; then
    ok "change status shows per-change artifact progress"
else
    err "change status missing artifact progress"
    cat "$out"
fi

# ── status resolves single active change automatically ───────────────────────
out="$TMP/status-one.out"
cd "$TMP" && rm -rf .changes/add-dark-mode .changes/fix-login
$JASH -c 'change propose only-change' >/dev/null 2>&1
out="$TMP/continue-auto.out"
$JASH -c 'change continue' > "$out" 2>&1
if grep -q "created artifact 'proposal'" "$out"; then
    ok "change continue auto-resolves single active change"
else
    err "change continue did not auto-resolve single change"
    cat "$out"
fi

# ── continue requires explicit name when multiple active ─────────────────────
$JASH -c 'change propose second-change' >/dev/null 2>&1
out="$TMP/continue-multi.out"
$JASH -c 'change continue' > "$out" 2>&1
if grep -q "multiple active changes" "$out"; then
    ok "change continue requires name with multiple changes"
else
    err "change continue did not require name with multiple changes"
    cat "$out"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 2 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 2 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 2 tests FAILED"
    exit 1
fi
