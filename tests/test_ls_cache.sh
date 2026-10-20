#!/usr/bin/env bash
# test_ls_cache.sh — BUG-035: the ls result cache must key on path + FLAGS, not
# path alone. Before the fix, `ls` and `ls -l` in the same dir within the 5s TTL
# returned EACH OTHER's cached output (stale/wrong/empty in interactive use).
# `--color` forces the cache active even off-tty; running both in ONE jash process
# (same static cache) exercises the collision.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
D="$ROOT/.lscache"; rm -rf "$D"; mkdir -p "$D"; : > "$D/AAA.txt"; : > "$D/BBB.txt"

echo "== ls cache keys on flags (BUG-035) =="
# one process: grid (ls) then long (ls -l), cache active via --color
S="$D/s.jsh"; printf 'cd %s\nls --color\nls -l --color\n' "$D" > "$S"
out="$("$J" "$S" 2>/dev/null | strip)"
# the -l view must contain a long-format line; must NOT have been served the grid cache
printf '%s' "$out" | grep -qE '\-rw.*AAA' && ok 'ls -l shows long format (no grid-cache collision)' || no 'long after grid' "$out"

# reverse order: long first, then grid — grid must not be served the long cache
S2="$D/s2.jsh"; printf 'cd %s\nls -l --color\nls --color\n' "$D" > "$S2"
out2="$("$J" "$S2" 2>/dev/null | strip)"
# last command is plain ls (grid); its tail line should list names without the -rw long prefix
tail2="$(printf '%s' "$out2" | grep AAA | tail -1)"
printf '%s' "$tail2" | grep -q 'AAA' && ! printf '%s' "$tail2" | grep -q -- '-rw' && ok 'ls (grid) not served the long cache' || no 'grid after long' "$tail2"

# both flag-views still list the real files (no empty)
printf '%s' "$out" | grep -q 'AAA' && printf '%s' "$out" | grep -q 'BBB' && ok 'no empty output (files present)' || no 'empty' "$out"

rm -rf "$D"
echo "== ls-cache: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
