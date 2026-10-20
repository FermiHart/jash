#!/usr/bin/env bash
# tests/test_agent_guard.sh — ADR-0026 Tier 3: agent anti-bypass guard.
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$PROJECT_ROOT/build/jash"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$PROJECT_ROOT"

# Use a temp metrics file so tests don't pollute the user's real metrics.
export HOME="$TMP"
mkdir -p "$HOME/.jash"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. status shows zero metrics when empty.
out="$TMP/status1.out"
$JASH -c 'agent-guard status' > "$out" 2>/dev/null
if grep -q "commands recorded" "$out" && grep -q "0% adoption" "$out"; then
    ok "agent-guard status shows empty metrics"
else
    err "agent-guard status empty metrics"
fi

# 2. warning mode allows bypass but records it.
if JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=0 \
   $JASH -c 'bash -c echo ok' > "$TMP/ag_warn.txt" 2>&1; then
    if grep -q "WARNING: agent bypassed jash" "$TMP/ag_warn.txt"; then
        ok "bypass warning emitted in watch mode"
    else
        err "bypass warning missing"
    fi
else
    err "bypass warning mode blocked command"
fi

# 3. strict mode blocks bypass.
if JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=1 $JASH -c 'bash -c echo ok' > "$TMP/ag_strict.txt" 2>&1; then
    err "strict mode allowed bypass"
else
    if grep -q "BLOCKED: agent attempted to bypass jash" "$TMP/ag_strict.txt"; then
        ok "strict mode blocks bypass"
    else
        err "strict mode failed without diagnostic"
    fi
fi

# 4. python3 -c bypass is also detected.
if command -v python3 >/dev/null 2>&1; then
    if JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=0 \
       $JASH -c 'python3 -c print(1)' > "$TMP/ag_py.txt" 2>&1; then
        if grep -q "WARNING: agent bypassed jash via python" "$TMP/ag_py.txt"; then
            ok "python -c bypass detected"
        else
            err "python bypass warning missing"
        fi
    else
        err "python bypass watch mode blocked"
    fi
else
    echo "  (python3 not available — skipping python bypass test)"
fi

# 5. jash-native command is counted as native, not bypass.
if JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=0 \
   $JASH -c 'speculate echo native' > "$TMP/ag_native.txt" 2>&1; then
    ok "jash-native command runs under guard"
else
    err "jash-native command blocked by guard"
fi

# 6. status aggregates correctly after the above.
out="$TMP/status2.out"
$JASH -c 'agent-guard status' > "$out" 2>/dev/null
if grep -q "bypass attempts" "$out"; then
    ok "agent-guard status aggregates metrics"
else
    err "agent-guard status missing metrics"
fi

# 7. The diagnostic promises that an explicit =0 overrides persisted strict
# mode. This is also what lets test harnesses isolate themselves from HOME.
$JASH -c 'agent-guard strict' >/dev/null 2>&1
if JASH_GUARD_BYPASS=1 /usr/bin/env -u JASH_GUARD_BYPASS_STRICT \
   $JASH -c 'bash -c "echo must-not-run"' > "$TMP/ag_persisted.txt" 2>&1; then
    err "persisted strict mode did not block without override"
else
    ok "persisted strict mode blocks without override"
fi
if override_out="$(JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=0 \
   $JASH -c 'bash -c "echo override-ok"' 2>&1)" &&
   grep -q 'override-ok' <<< "$override_out"; then
    ok "explicit strict=0 overrides persisted mode"
else
    err "explicit strict=0 did not override persisted mode"
fi
for malformed in '' typo; do
  if JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT="$malformed" \
     $JASH -c 'bash -c "echo must-not-run"' > "$TMP/ag_malformed.txt" 2>&1; then
    err "malformed strict override '$malformed' disabled persisted protection"
  else
    ok "malformed strict override '$malformed' preserves persisted protection"
  fi
done
if override_out="$(JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=false \
   $JASH -c 'bash -c "echo false-ok"' 2>&1)" &&
   grep -q 'false-ok' <<< "$override_out"; then
    ok "explicit strict=false overrides persisted mode"
else
    err "explicit strict=false did not override persisted mode"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  agent-guard RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ agent-guard tests PASSED"
    exit 0
else
    echo "  ✗ agent-guard tests FAILED"
    exit 1
fi
