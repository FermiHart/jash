#!/usr/bin/env bash
# tests/test_change_wave5.sh — ADR-0003 Wave 5: deterministic change verify
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
$JASH -c 'change propose verify-demo && change ff verify-demo' >/dev/null 2>&1

# ── incomplete tasks fail verification ──────────────────────────────────────
out="$TMP/verify-incomplete.out"
if $JASH -c 'change verify verify-demo' > "$out" 2>&1; then
    err "change verify unexpectedly passed incomplete tasks"
    cat "$out"
else
    if grep -q "not ready to archive" "$out" && grep -q "tasks: 0/5 complete" "$out"; then
        ok "change verify fails incomplete tasks"
    else
        err "change verify did not report incomplete tasks"
        cat "$out"
    fi
fi

# ── completed tasks pass when no build/test target exists ───────────────────
perl -0pi -e 's/- \[ \]/- [x]/g' "$TMP/.changes/verify-demo/tasks.md"
out="$TMP/verify-complete.out"
if $JASH -c 'change verify verify-demo' > "$out" 2>&1; then
    if grep -q "ready to archive" "$out" && grep -q "tasks: 5/5 complete" "$out"; then
        ok "change verify passes completed tasks without build target"
    else
        err "change verify passed but missing readiness report"
        cat "$out"
    fi
else
    err "change verify failed completed tasks"
    cat "$out"
fi

# ── failing test target blocks verification ─────────────────────────────────
cat > tests.sh <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x tests.sh
mkdir -p tests
cat > tests/test_suite.sh <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x tests/test_suite.sh

out="$TMP/verify-failbuild.out"
if $JASH -c 'change verify verify-demo' > "$out" 2>&1; then
    err "change verify unexpectedly passed failing test suite"
    cat "$out"
else
    if grep -q "build/test failed" "$out"; then
        ok "change verify fails on failing test suite"
    else
        err "change verify did not report failing test suite"
        cat "$out"
    fi
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 5 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 5 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 5 tests FAILED"
    exit 1
fi
