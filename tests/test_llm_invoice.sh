#!/usr/bin/env bash
# tests/test_llm_invoice.sh — ADR-0033 Wave 3: token economics
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$PROJECT_ROOT"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# Use an isolated data dir so tests don't pollute the user's invoice.
export HOME="$TMP"
mkdir -p "$HOME/.jash"

# ── status baseline ──────────────────────────────────────────────────────────
out="$TMP/status.out"
$JASH -c 'llm-invoice status' > "$out" 2>&1
if grep -q "token economics" "$out" && grep -q "total saved:" "$out"; then
    ok "llm-invoice status shows lifetime counters"
else
    err "llm-invoice status baseline failed"
    cat "$out"
fi

# ── session lifecycle persists across processes ──────────────────────────────
$JASH -c 'llm-invoice begin "test session"' >/dev/null 2>&1
out="$TMP/active.out"
$JASH -c 'llm-invoice status' > "$out" 2>&1
if grep -q "active session" "$out" && grep -q "test session" "$out"; then
    ok "llm-invoice begin persists active session"
else
    err "llm-invoice begin did not persist session"
    cat "$out"
fi

# Structured state queries have read-only local observation semantics. They
# must not update the active invoice or create one as an implicit side effect.
# Test both existing and absent ledgers, including metadata, before exercising
# the deliberately retained accounting hook of the legacy text command.
if python3 - "$JASH" "$TMP" <<'PY'
import os
from pathlib import Path
import subprocess
import sys

binary, root = sys.argv[1], Path(sys.argv[2])
fresh = root / 'query-only-home'
fresh.mkdir(mode=0o700)

def ledger(home):
    result = {}
    for name in ('invoice.bin', 'invoice_session.bin'):
        path = home / '.jash' / name
        if not path.exists():
            result[name] = None
            continue
        info = path.stat()
        result[name] = (path.read_bytes(), info.st_dev, info.st_ino, info.st_size,
                        info.st_mtime_ns, info.st_ctime_ns)
    return result

for home in (root, fresh):
    original = ledger(home)
    environment = dict(os.environ, HOME=str(home), JASH_BRAIN='0', JASH_SIGIL='0')
    for selector in ('--json', '--data', '--data-v2'):
        response = subprocess.run([binary, '-c', 'state ' + selector + ' --no-git'],
                                  env=environment, capture_output=True, timeout=20)
        assert response.returncode == 0, (selector, response.returncode, response.stderr)
        assert ledger(home) == original, (selector, 'structured query changed invoice ledger')
PY
then
    ok "structured state queries preserve active and absent invoice ledgers"
else
    err "structured state query wrote an invoice ledger"
fi

# ── legacy builtin hooks accumulate saved tokens ─────────────────────────────
# The text path retains the historical accounting hook. Reintroducing ledger
# writes into the structured query would violate its published read authority.
$JASH -c 'state' >/dev/null 2>&1
$JASH -c 'digest --brief' >/dev/null 2>&1
$JASH -c 'recall adoption' >/dev/null 2>&1
$JASH -c 'agent-guard status' >/dev/null 2>&1
$JASH -c 'brain status' >/dev/null 2>&1

out="$TMP/close.out"
$JASH -c 'llm-invoice close' > "$out" 2>&1
if grep -q "state" "$out" && grep -q "digest" "$out" && grep -q "recall" "$out"; then
    ok "llm-invoice close rolls up builtin token savings"
else
    err "llm-invoice close missing builtin savings"
    cat "$out"
fi

# ── lifetime counters match closed session ───────────────────────────────────
out="$TMP/lifetime.out"
$JASH -c 'llm-invoice status' > "$out" 2>&1
if grep -q "total saved:" "$out" && grep -q "naive input:" "$out" && grep -q "jash input:" "$out"; then
    ok "llm-invoice status shows lifetime after close"
else
    err "llm-invoice lifetime status failed"
    cat "$out"
fi

# ── reset clears lifetime counters ───────────────────────────────────────────
$JASH -c 'llm-invoice reset' >/dev/null 2>&1
out="$TMP/reset.out"
$JASH -c 'llm-invoice status' > "$out" 2>&1
if grep -q "total saved:  0 tokens" "$out" && grep -q "naive input:  0 tokens" "$out"; then
    ok "llm-invoice reset clears lifetime counters"
else
    err "llm-invoice reset failed"
    cat "$out"
fi

# ── no active session after close ────────────────────────────────────────────
out="$TMP/nosession.out"
$JASH -c 'llm-invoice close' > "$out" 2>&1
if grep -q "no active session" "$out"; then
    ok "llm-invoice close without active session reports error"
else
    err "llm-invoice close without session did not report error"
    cat "$out"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  llm-invoice RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ llm-invoice tests PASSED"
    exit 0
else
    echo "  ✗ llm-invoice tests FAILED"
    exit 1
fi
