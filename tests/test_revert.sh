#!/usr/bin/env bash
# tests/test_revert.sh — ADR-0026 Tier 1: cross-domain undo/revert.
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$PROJECT_ROOT"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. revert without args prints help.
if $JASH -c 'revert' >/dev/null 2>&1; then
    err "revert without args should fail"
else
    ok "revert without args fails with help"
fi

# 2. revert git add in a real repo.
if command -v git >/dev/null 2>&1; then
    cd "$TMP"
    git init -q repo
    cd repo
    echo x > file.txt
    git add file.txt >/dev/null
    rev_out="$TMP/revert_add.out"
    $JASH -c 'revert git add' > "$rev_out" 2>&1
    rev_rc=$?
    diff_rc=0
    git diff --cached --quiet || diff_rc=$?
    if [ "$rev_rc" -eq 0 ] && [ "$diff_rc" -eq 0 ]; then
        ok "revert git add unstages changes"
    else
        err "revert git add did not unstage (rev_rc=$rev_rc diff_rc=$diff_rc)"
        cat "$rev_out"
    fi
    cd "$PROJECT_ROOT"
else
    echo "  (git not available — skipping git revert tests)"
fi

# 3. revert docker with missing id fails gracefully (no docker or invalid id).
out="$TMP/revert_docker.out"
$JASH -c 'revert docker nonexistent_container_xyz' > "$out" 2>&1 || true
if grep -q "docker" "$out"; then
    ok "revert docker produces diagnostic"
else
    err "revert docker produced no diagnostic"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  revert RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ revert tests PASSED"
    exit 0
else
    echo "  ✗ revert tests FAILED"
    exit 1
fi
