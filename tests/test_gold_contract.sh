#!/usr/bin/env bash
# tests/test_gold_contract.sh — ADR-0044 Wave A0: the shared gold contract.
#
# gold.c is the one place every gold builtin reaches jash's in-process ledgers
# (delta snapshot, budget-cap, json escaper) behind a single gate. There is no
# `gold` builtin to call directly, so we exercise the contract THROUGH its first
# consumer (ls --delta) and assert the gate semantics + the shared snapshot
# store that any future gold builtin will reuse.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_gold.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT

# Admission is an actual launch in the private fixture, not an executable bit.
can_run() { env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [[ -n "${JASH:-}" ]]; then
    can_run "$JASH" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    JASH=
    for candidate in "$PROJECT_ROOT/build/jash-release" "$PROJECT_ROOT/build/jash" "$PROJECT_ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then JASH="$candidate"; break; fi
    done
    if [[ -z "$JASH" ]]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

mkdir -p "$T/wd"

pass=0; fail=0
ok() { pass=$((pass+1)); echo "  ✓ $1"; }
no() { fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }

BASE=(env -i HOME="$T" CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="gold-$$" TERM=dumb
      JASH_BRIEF=0 JASH_SIGIL=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_DRIFT=0
      JASH_NUDGE=0 JASH_PREFETCH=0 JASH_CHANGES=0)
run() { (cd "$T/wd" && "${BASE[@]}" "$@" "$JASH" -c "$1") 2>/dev/null; }

: > "$T/wd/a"; : > "$T/wd/b"

# ── 1. the shared DELTA store lives under gold-delta/<builtin>/ ───────────────
(cd "$T/wd" && "${BASE[@]}" "$JASH" -c 'ls --delta') >/dev/null 2>&1
snap="$(find "$T/.jash/gold-delta/ls" -name '*.snap' 2>/dev/null | head -1)"
if [[ -n "$snap" ]]; then ok "gold DELTA snapshot at gold-delta/ls/<key>.snap"
else no "gold DELTA snapshot at gold-delta/ls/<key>.snap"; fi

# ── 2. snapshot carries the versioned gold header (shared format) ────────────
# JGOLD2 is length-aware: a filename containing LF, TAB or non-UTF8 bytes is
# one exact entry. The checked-reader gate separately exercises strict JGOLD1
# migration. Do not put this binary snapshot in a shell text substitution.
if [[ -n "$snap" ]] && python3 - "$snap" <<'PYMAGIC'
import sys
with open(sys.argv[1], 'rb') as stream:
    assert stream.read(8) == b'#JGOLD2\0'
PYMAGIC
then
    ok "snapshot uses the byte-preserving #JGOLD2 header"
else no "snapshot uses the byte-preserving #JGOLD2 header"; fi

# ── 3. the gate: JASH_GOLD=0 disables enrichment (explicit flag degrades) ────
out_on="$( (cd "$T/wd" && "${BASE[@]}" "$JASH" -c 'ls --delta') 2>/dev/null )"
out_off="$( (cd "$T/wd" && "${BASE[@]}" JASH_GOLD=0 "$JASH" -c 'ls --delta') 2>/dev/null )"
if grep -q '\[delta\]' <<<"$out_on" && ! grep -q '\[delta\]' <<<"$out_off"; then
    ok "gate: JASH_GOLD=0 disables the gold path"
else no "gate: JASH_GOLD=0 disables the gold path"; fi

# ── 4. session-scoped keying: a different session id starts a fresh baseline ──
(cd "$T/wd" && "${BASE[@]}" "$JASH" -c 'ls --delta') >/dev/null 2>&1   # arm sess A
: > "$T/wd/c"
outB="$( (cd "$T/wd" && env -i HOME="$T" CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="OTHER-$$" \
          TERM=dumb JASH_BRIEF=0 JASH_SIGIL=0 JASH_MEMO=0 JASH_LOOPGUARD=off \
          JASH_DRIFT=0 JASH_NUDGE=0 JASH_PREFETCH=0 JASH_CHANGES=0 \
          "$JASH" -c 'ls --delta') 2>/dev/null )"
if grep -q 'baseline armed' <<<"$outB"; then
    ok "session-scoped keys: new session → fresh baseline (no cross-session leak)"
else no "session-scoped keys: new session → fresh baseline" "$outB"; fi

# ── 5. distinct dirs never collide on disk (key includes abs dir) ────────────
mkdir -p "$T/wd/sub"; : > "$T/wd/sub/x"
(cd "$T/wd/sub" && "${BASE[@]}" "$JASH" -c 'ls --delta') >/dev/null 2>&1
nsnap="$(find "$T/.jash/gold-delta/ls" -name '*.snap' 2>/dev/null | wc -l | tr -d ' ')"
if [[ "$nsnap" -ge 2 ]]; then ok "distinct dirs → distinct snapshots ($nsnap)"
else no "distinct dirs → distinct snapshots" "$nsnap"; fi

echo "── gold contract (ADR-0044 A0) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
