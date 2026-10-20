#!/usr/bin/env bash
# test_quoted_cmd.sh — POSIX quote removal on the COMMAND WORD (BUG-028).
# `"echo"`, `'echo'`, `"$CMD"`, `"ec"ho`, and the same inside a pipeline segment
# must resolve to the unquoted command, == bash. (jash kept the literal quotes
# → spurious `"echo": command not found`.) Verified via SCRIPT FILE, not `-c`.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — jash=[$2] bash=[$3]"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
D="$(mktemp -d)"

chk(){ # label  script
  printf '%s\n' "$2" > "$D/p.jsh"
  local j b
  j="$("$J" "$D/p.jsh" 2>/dev/null | strip | head -1)"
  b="$(/bin/bash "$D/p.jsh" 2>/dev/null | head -1)"
  [ "$j" = "$b" ] && ok "$1" || no "$1" "$j" "$b"
}

echo "== quoted command word == bash (BUG-028) =="
chk 'double-quoted literal'  '"echo" hi'
chk 'single-quoted literal'  "'echo' hi"
chk 'quoted var expansion'   'CMD=echo; "$CMD" hi'
chk 'partially quoted'       '"ec"ho hi'
chk 'pipeline quoted cmd'    'echo a | "grep" a'
chk 'pipeline quoted var'    'GREP=grep; echo x | "$GREP" x'
chk 'quoted cmd + quoted arg' '"echo" "two words"'
chk 'unquoted still works'   'echo plain'

rm -rf "$D"
echo "== quoted-cmd: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
