#!/usr/bin/env bash
# test_exec_json.sh — structured, literal command input (ROADMAP Wave 7b).
# `jash --exec-json '{"argv":[...]}'` runs argv literally: no word-splitting,
# no globbing, no $-expansion, no quote round-trip.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — got [$2]"; }
run(){ "$J" --exec-json "$1" 2>/dev/null; }   # stdout only (status line is on stderr)

echo "== --exec-json literal command input =="

o=$(run '{"argv":["echo","a b   c","d"]}')
[ "$o" = "a b   c d" ] && ok "spaces preserved literally (no word-split)" || no "spaces" "$o"

o=$(run '{"argv":["echo","*.c"]}')
[ "$o" = "*.c" ] && ok "glob char stays literal" || no "glob" "$o"

o=$(run '{"argv":["echo","$HOME"]}')
[ "$o" = '$HOME' ] && ok "\$ stays literal (no expansion)" || no "dollar" "$o"

o=$(run '{"argv":["printf","[%s]","x y"]}')
[ "$o" = "[x y]" ] && ok "builtin gets literal arg with space" || no "builtin" "$o"

# invalid JSON → rc 2, no execution
"$J" --exec-json 'garbage' >/dev/null 2>&1; rc=$?
[ "$rc" = "2" ] && ok "invalid JSON → rc 2" || no "invalid-rc" "$rc"

echo "== exec-json: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
