#!/usr/bin/env bash
# ci_adoption_gate.sh — W7: CI safety gate for the adoption machinery (ADR-0040).
# The banner promise "parity-gated in CI" lands here. Gates, in order:
#   1. bash-parity suite must be green at EVERY adopt level (0..3) — the
#      adoption machinery may never change shell semantics at any dial setting.
#   2. A/B harness (test_ab_adoption.sh) must be green — adoption changes
#      metrics/stderr only, never stdout/rc, and the kill-switch restores
#      baseline exactly.
# Exit nonzero on any gate failure. Intended entry-point for CI and pre-push.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FAILED=0

echo "=== CI adoption gate ==="

# Gate 1: parity sweep across adopt levels. Isolated HOME per level so the
# sweep neither reads nor pollutes the developer's ~/.jash state.
for lvl in 0 1 2 3; do
  h="$(mktemp -d)"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  if out="$(HOME="$h" JASH_SIGIL=1 JASH_ADOPT="$lvl" JASH_BRIEF=0 JASH_MEMO=0 \
        JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0 \
        bash "$ROOT/tests/test_bash_parity.sh" 2>&1)"; then
    echo "  ✓ parity green @ JASH_ADOPT=$lvl ($(printf '%s' "$out" | grep -o '[0-9]* passed' | head -1))"
  else
    FAILED=1
    echo "  ✗ parity BROKE @ JASH_ADOPT=$lvl"
    printf '%s\n' "$out" | grep '✗' | head -10 | sed 's/^/      /'
  fi
  rm -rf "$h"
done

# Gate 2: A/B harness (safety + efficacy + kill-switch).
if out="$(JASH_SIGIL=1 bash "$ROOT/tests/test_ab_adoption.sh" 2>&1)"; then
  echo "  ✓ A/B harness green ($(printf '%s' "$out" | grep -o '[0-9]* passed' | head -1))"
else
  FAILED=1
  echo "  ✗ A/B harness FAILED"
  printf '%s\n' "$out" | grep '✗' | head -10 | sed 's/^/      /'
fi

# Gate 3 (launch, ADR-0041): the installed binary must be byte- and
# behavior-identical to the gated tree build. Opt-in — during development the
# tree legitimately runs ahead of the install. Enforce at launch/release time:
#   JASH_LAUNCH_GATE=1 git push   (or JASH_LAUNCH_GATE=1 bash tests/ci_adoption_gate.sh)
if [ "${JASH_LAUNCH_GATE:-0}" = "1" ]; then
  if out="$(bash "$ROOT/tests/test_install_parity.sh" 2>&1)"; then
    echo "  ✓ launch gate green ($(printf '%s' "$out" | grep -o '[0-9]* passed' | head -1))"
  else
    FAILED=1
    echo "  ✗ launch gate FAILED (installed binary drifted from gated tree)"
    printf '%s\n' "$out" | grep '✗' | head -10 | sed 's/^/      /'
  fi
else
  echo "  - launch gate skipped (JASH_LAUNCH_GATE=1 enforces ADR-0041 install parity)"
fi

if [ "$FAILED" -eq 0 ]; then
  echo "=== adoption gate: ALL GREEN ==="
else
  echo "=== adoption gate: FAILED ==="
fi
exit "$FAILED"
