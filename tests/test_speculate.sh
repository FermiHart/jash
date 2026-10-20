#!/usr/bin/env bash
# tests/test_speculate.sh — ADR-0026 Tier 1: speculative execution sandbox.
set -e

JASH="${JASH:-./build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. Creating a file in sandbox does not touch the original tree.
out="$TMP/spec1.out"
$JASH -c 'speculate touch __spec_test_file__.txt' > "$out" 2>/dev/null
if grep -q "+ __spec_test_file__.txt" "$out" && [ ! -e __spec_test_file__.txt ]; then
    ok "sandbox file creation does not mutate original tree"
else
    err "sandbox leaked file to original tree or no diff"
fi

# 2. Deleting a file in sandbox does not delete original.
out="$TMP/spec2.out"
$JASH -c 'speculate rm CLAUDE.md' > "$out" 2>/dev/null
if grep -q -- "- CLAUDE.md" "$out" && [ -e CLAUDE.md ]; then
    ok "sandbox deletion does not delete original file"
else
    err "sandbox deletion leaked or no diff"
fi

# 3. JSON mode emits valid JSON.
out="$TMP/spec3.json"
$JASH -c 'speculate --json touch __spec_json__.txt' > "$out" 2>/dev/null
if python3 -m json.tool "$out" >/dev/null 2>&1 && grep -q '"rc"' "$out"; then
    ok "speculate --json emits valid JSON"
else
    err "speculate --json invalid"
fi

# 4. Empty command returns error.
if $JASH -c 'speculate' >/dev/null 2>&1; then
    err "empty speculate should fail"
else
    ok "empty speculate fails"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  speculate RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ speculate tests PASSED"
    exit 0
else
    echo "  ✗ speculate tests FAILED"
    exit 1
fi
