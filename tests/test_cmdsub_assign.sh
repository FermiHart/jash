#!/usr/bin/env bash
# test_cmdsub_assign.sh — `VAR=$(...)` must NOT field-split its RHS (POSIX), so
# command-substitution output keeps embedded newlines (BUG-031). The common agent
# idiom `out=$(cmd); echo "$out"` (quoted) must reproduce the captured text.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — jash=[$2] bash=[$3]"; }
D="$(mktemp -d)"

chk(){ printf '%s' "$2" > "$D/p.jsh"
  local j b; j="$("$J" "$D/p.jsh" 2>/dev/null)"; b="$(/bin/bash "$D/p.jsh" 2>/dev/null)"
  [ "$j" = "$b" ] && ok "$1" || no "$1" "$j" "$b"; }

echo "== cmdsub assignment keeps newlines (BUG-031) =="
chk 'quoted expansion'   'V=$(printf "a\nb"); echo "[$V]"'
chk 'wc -l of multiline' 'V=$(printf "l1\nl2\nl3"); echo "$V" | wc -l | tr -d " "'
chk 'trailing trimmed'   'V=$(printf "x\n\n\n"); echo "[$V]"'
chk 'direct quoted sub'  'echo "[$(printf "a\nb")]"'
chk 'single line still'  'V=$(echo hi); echo "[$V]"'
chk 'spaces preserved'   'V=$(printf "a b  c"); echo "[$V]"'

rm -rf "$D"
echo "== cmdsub-assign: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
