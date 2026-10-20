#!/usr/bin/env bash
# tests/test_context_ask.sh — ADR-0027 query-driven code digest.
set -e

JASH="${JASH:-./build/jash}"
export JASH_SIGIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$(dirname "$0")/.."

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. Basic human-mode query returns the expected top file.
out="$TMP/human.out"
$JASH -c 'context ask "undo rollback"' > "$out" 2>/dev/null
if grep -q "modules/undo/undo_module.c" "$out"; then
    ok "human mode ranks undo_module.c for 'undo rollback'"
else
    err "human mode did not rank undo_module.c"
fi

# 2. JSON mode emits valid structure.
out="$TMP/json.out"
$JASH -c 'context ask "undo rollback" --json' > "$out" 2>/dev/null
if grep -q '"context_ask"' "$out" && grep -q '"files"' "$out"; then
    ok "json mode emits context_ask envelope"
else
    err "json mode missing envelope fields"
fi

# 3. JSON output includes expected path.
if grep -Eq '"path"[[:space:]]*:[[:space:]]*"modules/undo/undo_module.c"' "$out"; then
    ok "json mode includes undo_module.c path"
else
    err "json mode missing undo_module.c path"
fi

# 4. Empty query returns error.
if $JASH -c 'context ask ""' >/dev/null 2>&1; then
    err "empty query should fail"
else
    ok "empty query fails"
fi

# 5. Query with no searchable terms fails gracefully.
if $JASH -c 'context ask "!?"' >/dev/null 2>&1; then
    err "query with no terms should fail"
else
    ok "query with no terms fails"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  context ask RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ context ask tests PASSED"
    exit 0
else
    echo "  ✗ context ask tests FAILED"
    exit 1
fi
