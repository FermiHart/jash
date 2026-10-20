#!/usr/bin/env bash
# test_herestring.sh — here-string `cmd <<< word` (BUG-033). Preprocessed into a
# temp file + `< file` so both redirect engines (builtin + pipeline) work; quotes
# removed, expansion deferred (env/prior-line/cmdsub/arith all resolve). Verified
# == bash via SCRIPT FILE. Known residual: same-LINE `V=x; cmd <<< "$V"` (the
# deferred expansion runs before the same-line assignment) — not covered here.
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

echo "== here-string <<< == bash (BUG-033) =="
chk 'literal to cat'      'cat <<< "hello world"'
chk 'into read'           'read x <<< "hi"; echo "got=$x"'
chk 'read two vars'       'read a b <<< "one two"; echo "$a|$b"'
chk 'into grep'           'grep ell <<< "hello"'
chk 'env var'             'cat <<< "$HOME"'
chk 'prior-line var'      $'V=hi\ncat <<< "$V there"'
chk 'command sub'         'cat <<< "$(printf "a\nb")"'
chk 'arithmetic'          'cat <<< "$(( 6 * 7 ))"'
chk 'single-quote literal' "cat <<< '\$V stays'"
chk 'pipe after'          'cat <<< "abc" | tr a-z A-Z'
chk 'read multi prior'    $'W=world\nread x y <<< "hello $W"\necho "$x-$y"'

rm -rf "$D"
echo "== herestring: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
