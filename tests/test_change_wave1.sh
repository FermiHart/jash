#!/usr/bin/env bash
# tests/test_change_wave1.sh — ADR-0003 Wave 1: change folder + schema engine
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# ── help ─────────────────────────────────────────────────────────────────────
out="$TMP/help.out"
$JASH -c 'change' > "$out" 2>&1
if grep -q "change schema init" "$out" && grep -q "change propose" "$out"; then
    ok "change help shows wave 1 commands"
else
    err "change help missing commands"
    cat "$out"
fi

# ── schema init creates builtin schema ───────────────────────────────────────
out="$TMP/schema-init.out"
$JASH -c 'change schema init spec-driven' > "$out" 2>&1
if [ -f "$TMP/.changes/schemas/spec-driven/schema.yaml" ] && grep -q "name: spec-driven" "$TMP/.changes/schemas/spec-driven/schema.yaml"; then
    ok "change schema init creates spec-driven schema"
else
    err "change schema init did not create schema"
    cat "$out"
fi

# ── schema init rejects unknown builtin ──────────────────────────────────────
out="$TMP/schema-unknown.out"
$JASH -c 'change schema init not-real' > "$out" 2>&1
if grep -q "unknown builtin schema" "$out"; then
    ok "change schema init rejects unknown builtin schema"
else
    err "change schema init did not reject unknown schema"
    cat "$out"
fi

# ── schema fork renames source schema ────────────────────────────────────────
out="$TMP/schema-fork.out"
$JASH -c 'change schema fork spec-driven bugfix' > "$out" 2>&1
if [ -f "$TMP/.changes/schemas/bugfix/schema.yaml" ] && grep -q "name: bugfix" "$TMP/.changes/schemas/bugfix/schema.yaml"; then
    ok "change schema fork creates renamed schema"
else
    err "change schema fork did not create renamed schema"
    cat "$out"
fi

# ── status lists schemas and no changes ──────────────────────────────────────
out="$TMP/status-empty.out"
$JASH -c 'change status' > "$out" 2>&1
if grep -q "spec-driven" "$out" && grep -q "bugfix" "$out" && grep -q "Active changes:" "$out"; then
    ok "change status lists schemas before changes"
else
    err "change status did not list schemas"
    cat "$out"
fi

# ── propose creates change scaffold ──────────────────────────────────────────
out="$TMP/propose.out"
$JASH -c 'change propose add-dark-mode' > "$out" 2>&1
if [ -d "$TMP/.changes/add-dark-mode" ] && \
   [ -f "$TMP/.changes/add-dark-mode/.change.yaml" ]; then
    ok "change propose creates change scaffold"
else
    err "change propose did not create change scaffold"
    cat "$out"
    find "$TMP/.changes" -type f
fi

# ── change metadata is valid ─────────────────────────────────────────────────
if grep -q "name: add-dark-mode" "$TMP/.changes/add-dark-mode/.change.yaml" && \
   grep -q "schema: spec-driven" "$TMP/.changes/add-dark-mode/.change.yaml" && \
   grep -q "status: proposed" "$TMP/.changes/add-dark-mode/.change.yaml"; then
    ok "change metadata records name/schema/status"
else
    err "change metadata incomplete"
    cat "$TMP/.changes/add-dark-mode/.change.yaml"
fi

# ── status lists active change ───────────────────────────────────────────────
out="$TMP/status-active.out"
$JASH -c 'change status' > "$out" 2>&1
if grep -q "add-dark-mode" "$out" && grep -q "status=proposed" "$out"; then
    ok "change status lists active change"
else
    err "change status did not list active change"
    cat "$out"
fi

# ── propose with custom schema ───────────────────────────────────────────────
out="$TMP/propose-bugfix.out"
$JASH -c 'change propose fix-login --schema bugfix' > "$out" 2>&1
if [ -d "$TMP/.changes/fix-login" ] && grep -q "schema: bugfix" "$TMP/.changes/fix-login/.change.yaml"; then
    ok "change propose accepts --schema"
else
    err "change propose --schema failed"
    cat "$out"
fi

# ── propose rejects duplicate name ───────────────────────────────────────────
out="$TMP/propose-dup.out"
$JASH -c 'change propose add-dark-mode' > "$out" 2>&1
if grep -q "already exists" "$out"; then
    ok "change propose rejects duplicate change name"
else
    err "change propose did not reject duplicate name"
    cat "$out"
fi

# ── propose rejects unexpected argument ──────────────────────────────────────
out="$TMP/propose-invalid.out"
$JASH -c 'change propose bad name!' > "$out" 2>&1
if grep -q "unexpected argument" "$out"; then
    ok "change propose rejects unexpected argument"
else
    err "change propose did not reject unexpected argument"
    cat "$out"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 1 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 1 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 1 tests FAILED"
    exit 1
fi
