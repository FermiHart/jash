#!/usr/bin/env bash
# tests/test_change_wave3.sh — ADR-0003 Wave 3: delta spec sync + archive
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
$JASH -c 'change propose add-auth && change ff add-auth' >/dev/null 2>&1

# write a real delta spec
mkdir -p "$TMP/.changes/add-auth/specs"
cat > "$TMP/.changes/add-auth/specs/auth.md" <<'EOF'
# Delta Spec: auth

## ADDED Requirements

### Requirement: User Login
The system SHALL authenticate users with username and password.

#### Scenario: Valid credentials
- GIVEN a registered user
- WHEN they submit valid credentials
- THEN they receive a session token

## MODIFIED Requirements

## REMOVED Requirements
EOF

# ── sync merges ADDED requirement into main spec ─────────────────────────────
out="$TMP/sync.out"
$JASH -c 'change sync add-auth' > "$out" 2>&1
if [ -f "$TMP/specs/auth.md" ] && grep -q "User Login" "$TMP/specs/auth.md" && grep -q "Valid credentials" "$TMP/specs/auth.md"; then
    ok "change sync merges ADDED requirements"
else
    err "change sync did not merge ADDED requirements"
    cat "$out"
    cat "$TMP/specs/auth.md" 2>/dev/null
fi

# ── sync detects ADDED duplicate as conflict ─────────────────────────────────
out="$TMP/sync-conflict.out"
$JASH -c 'change sync add-auth' > "$out" 2>&1
if grep -q "conflict" "$out"; then
    ok "change sync detects duplicate ADDED requirement"
else
    err "change sync did not detect duplicate ADDED requirement"
    cat "$out"
fi

# ── archive moves change to archive folder ───────────────────────────────────
out="$TMP/archive.out"
$JASH -c 'change archive add-auth' > "$out" 2>&1
if [ ! -d "$TMP/.changes/add-auth" ] && [ -d "$TMP/.changes/archive/" ] && ls "$TMP/.changes/archive/" | grep -q "add-auth"; then
    ok "change archive moves change folder to archive"
else
    err "change archive did not move folder"
    cat "$out"
    find "$TMP/.changes" -maxdepth 2 -type d
fi

# ── archive removes change from active list ──────────────────────────────────
out="$TMP/status-after.out"
$JASH -c 'change status' > "$out" 2>&1
if ! grep -q "add-auth" "$out"; then
    ok "change archive removes change from active list"
else
    err "change archive still shows change as active"
    cat "$out"
fi

# ── modify existing requirement ──────────────────────────────────────────────
$JASH -c 'change propose update-timeout && change ff update-timeout' >/dev/null 2>&1
mkdir -p "$TMP/.changes/update-timeout/specs"
cat > "$TMP/.changes/update-timeout/specs/auth.md" <<'EOF'
# Delta Spec: update-timeout

## MODIFIED Requirements

### Requirement: User Login
The system SHALL authenticate users and expire sessions after 15 minutes.
EOF
$JASH -c 'change sync update-timeout' >/dev/null 2>&1
if grep -q "15 minutes" "$TMP/specs/auth.md"; then
    ok "change sync applies MODIFIED requirements"
else
    err "change sync did not apply MODIFIED requirements"
    cat "$TMP/specs/auth.md"
fi

# ── remove existing requirement ───────────────────────────────────────────────
$JASH -c 'change propose remove-login && change ff remove-login' >/dev/null 2>&1
mkdir -p "$TMP/.changes/remove-login/specs"
cat > "$TMP/.changes/remove-login/specs/auth.md" <<'EOF'
# Delta Spec: remove-login

## REMOVED Requirements

### Requirement: User Login
EOF
$JASH -c 'change sync remove-login' >/dev/null 2>&1
if ! grep -q "User Login" "$TMP/specs/auth.md"; then
    ok "change sync applies REMOVED requirements"
else
    err "change sync did not remove requirement"
    cat "$TMP/specs/auth.md"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  change wave 3 RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ change wave 3 tests PASSED"
    exit 0
else
    echo "  ✗ change wave 3 tests FAILED"
    exit 1
fi
