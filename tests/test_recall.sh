#!/usr/bin/env bash
# test_recall.sh — `recall` chain (command/outcome) memory facet (ADR-0023 E1).
# The agent asks the shell "did I run X? did it work?" against the attested chain.
# Isolated via a temp HOME so the real ~/.jash/chain.log is untouched.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — got: [$2]"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TH="$(mktemp -d)"; mkdir -p "$TH/.jash"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
export HOME="$TH" JASH_SIGIL=1
"$J" -c 'echo alpha'            >/dev/null 2>&1
"$J" -c 'ls /no_such_dir_zzz'  >/dev/null 2>&1
"$J" -c 'echo beta build'      >/dev/null 2>&1
"$J" -c 'false'                >/dev/null 2>&1

echo "== recall chain facet (ADR-0023 E1) =="

# 1. newest-first: most recent real command appears before older ones
out="$("$J" -c 'recall cmds' 2>/dev/null | strip)"
first="$(printf '%s\n' "$out" | grep -nm1 'beta build' | cut -d: -f1)"
alph="$(printf '%s\n' "$out" | grep -nm1 'echo alpha' | cut -d: -f1)"
if [ -n "$first" ] && [ -n "$alph" ] && [ "$first" -lt "$alph" ]; then ok 'newest-first ordering'; else no 'newest-first ordering' "$out"; fi

# 2. substring filter via `did`
out="$("$J" -c 'recall did alpha' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'echo alpha' && ! printf '%s' "$out" | grep -q 'beta build'; then ok 'did <pat> filters'; else no 'did <pat> filters' "$out"; fi

# 3. --failed shows only rc!=0
out="$("$J" -c 'recall cmds --failed' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'no_such_dir_zzz' && ! printf '%s' "$out" | grep -q 'echo alpha'; then ok '--failed shows only failures'; else no '--failed shows only failures' "$out"; fi

# 4. --last N limits the number of rows
n="$("$J" -c 'recall cmds --last 2' 2>/dev/null | strip | grep -c '^#')"
if [ "$n" = "2" ]; then ok '--last N limits rows'; else no '--last N limits rows' "rows=$n"; fi

# 5. FAIL/ok annotation present
out="$("$J" -c 'recall did false' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'FAIL'; then ok 'failure annotated FAIL'; else no 'failure annotated FAIL' "$out"; fi

# 6. no match -> rc 1 + message
out="$("$J" -c 'recall did ZZZ_NOPE' 2>/dev/null | strip)"; rc=$?
if printf '%s' "$out" | grep -q 'nothing matching'; then ok 'no-match message'; else no 'no-match message' "$out"; fi

# 7. no chain at all -> guidance, rc 1
EMPTY="$(mktemp -d)"
out="$(HOME="$EMPTY" JASH_SIGIL=0 "$J" -c 'recall cmds' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'no recorded history'; then ok 'empty-chain guidance'; else no 'empty-chain guidance' "$out"; fi
rmdir "$EMPTY" 2>/dev/null

# 8. F4 regression: `recall list` (output-overflow facet) still routes, not to chain
out="$("$J" -c 'recall list' 2>/dev/null | strip)"
if ! printf '%s' "$out" | grep -q 'no recorded history'; then ok 'F4 recall list unbroken'; else no 'F4 recall list unbroken' "$out"; fi

# 9. recall stats aggregates runs + failures
out="$("$J" -c 'recall stats' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'recall stats' && printf '%s' "$out" | grep -q 'most run'; then ok 'recall stats aggregates'; else no 'recall stats' "$out"; fi

# 10. recall failed == recall cmds --failed (only failures)
out="$("$J" -c 'recall failed' 2>/dev/null | strip)"
if printf '%s' "$out" | grep -q 'FAIL' && ! printf '%s' "$out" | grep -q 'echo alpha'; then ok 'recall failed shortcut'; else no 'recall failed' "$out"; fi

# 11. recall cmds --json emits JSONL
out="$("$J" -c 'recall cmds --json --last 5' 2>/dev/null)"
if printf '%s' "$out" | grep -q '"rc":' && printf '%s' "$out" | grep -q '"cmd":'; then ok 'recall --json JSONL'; else no 'recall --json' "$out"; fi

# 12. recall adoption — attested BI metric (differentiator usage %)
out="$("$J" -c 'recall adoption --json' 2>/dev/null)"
if printf '%s' "$out" | grep -q '"adoption_pct":' && printf '%s' "$out" | grep -q '"total":'; then ok 'recall adoption --json (BI metric)'; else no 'recall adoption' "$out"; fi

# A project digest is a Jash differential too, not just `recall`/`expect`.
"$J" -c 'digest --brief --no-cache' >/dev/null 2>&1
out="$("$J" -c 'recall adoption --json' 2>/dev/null)"
if printf '%s' "$out" | grep -q '"digest":1'; then ok 'project digest counted in adoption'; else no 'digest adoption' "$out"; fi

rm -rf "$TH"
echo "== recall: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
