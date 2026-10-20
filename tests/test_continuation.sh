#!/usr/bin/env bash
# test_continuation.sh — POSIX backslash-newline line continuation in scripts/-c.
# `echo one \<NL>two` must join to `echo one two` (== bash); single quotes keep it
# literal; continuation works in double quotes and across a pipeline. (jash kept a
# literal backslash-newline and never joined.) Verified via SCRIPT FILE.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — jash=[$2] bash=[$3]"; }
D="$(mktemp -d)"

chk(){ # label  file
  local j b; j="$("$J" "$2" 2>/dev/null)"; b="$(/bin/bash "$2" 2>/dev/null)"
  [ "$j" = "$b" ] && ok "$1" || no "$1" "$j" "$b"
}

echo "== backslash-newline continuation == bash =="
printf 'echo one \\\ntwo\n'      > "$D/a"; chk 'unquoted joins'        "$D/a"
printf 'echo "a \\\nb"\n'        > "$D/b"; chk 'joins in double-quote' "$D/b"
printf "echo 'a \\\\\nb'\n"      > "$D/c"; chk 'literal in single-q'   "$D/c"
printf 'echo hello | \\\ncat\n'  > "$D/d"; chk 'joins across pipe'     "$D/d"
printf 'echo a \\\nb \\\nc\n'    > "$D/e"; chk 'multi-line join'       "$D/e"

rm -rf "$D"
echo "== continuation: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
