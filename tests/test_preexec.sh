#!/usr/bin/env bash
# test_preexec.sh — ADR-0040 W3: pre-exec absorption engine + destructive floor.
# `bash -c 'STR'` at adoption level >= 2 is rewritten into a re-exec of jash
# itself (recording/guard/undo regained, process semantics preserved); level 3
# additionally pins rm & peers to the recoverable path (JASH_TRASH floor).
# Also covers BUG-150: JASH_TRASH=off used to parse as ON ('o' prefix match).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Binary: $JASH override wins; otherwise the NEWEST of build/ vs build-speed/
# (a stale build/jash silently invalidates every assertion below — BUG-151).
J="${JASH:-}"
if [ -z "$J" ]; then
  A="$ROOT/build/jash"; B="$ROOT/build-speed/jash"
  if [ -x "$A" ] && [ -x "$B" ]; then
    if [ "$A" -nt "$B" ]; then J="$A"; else J="$B"; fi
  elif [ -x "$A" ]; then J="$A"; else J="$B"; fi
fi
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"; mkdir -p "$TH/.jash/modules"
cp "$ROOT/modules/sigil/sigil.modjash" "$TH/.jash/modules/" 2>/dev/null
W="$TH/work"; mkdir -p "$W"
unset JASH_SIGIL JASH_ADOPT JASH_TRASH JASH_ABSORB 2>/dev/null

echo "== W3 pre-exec absorption =="

# 1. level 2: bash -c absorbed — payload output intact + notice on stderr
out=$(JASH_ADOPT=2 "$J" -c 'bash -c "echo hi"' 2>"$TH/e1")
[ "$out" = "hi" ] && grep -q 'bash -c absorbed' "$TH/e1" \
  && ok 'level 2 absorbs bash -c (output intact, notice emitted)' \
  || no 'absorb' "out=$out err=$(cat "$TH/e1")"

# 2. absorbed child regains recording (chain.log grows via jash re-exec)
rm -f "$TH/.jash/chain.log"
JASH_ADOPT=2 "$J" -c 'bash -c "/bin/echo recorded"' >/dev/null 2>&1
grep -q 'echo recorded' "$TH/.jash/chain.log" 2>/dev/null \
  && ok 'absorbed child records to chain.log (superpower regained)' \
  || no 'child-record' 'chain.log missing the absorbed command'

# 3. positional args ($0) NOT absorbed — falls through to the real shell
out=$(JASH_ADOPT=2 "$J" -c 'bash -c "echo \$0" foo' 2>"$TH/e3")
[ "$out" = "foo" ] && ! grep -q absorbed "$TH/e3" \
  && ok 'trailing positionals fall through untouched ($0 semantics kept)' \
  || no 'positional' "out=$out"

# 4. JASH_ABSORB=0 escape hatch
JASH_ADOPT=2 JASH_ABSORB=0 "$J" -c 'bash -c "echo off"' 2>"$TH/e4" >/dev/null
! grep -q absorbed "$TH/e4" && ok 'JASH_ABSORB=0 disables the engine' \
  || no 'escape hatch' 'absorbed anyway'

# 5. level 1 (inference ceiling): never absorbs
JASH_ADOPT=1 "$J" -c 'bash -c "echo lvl1"' 2>"$TH/e5" >/dev/null
! grep -q absorbed "$TH/e5" && ok 'level 1 never rewrites (nudge ceiling)' \
  || no 'level gate' 'absorbed at level 1'

# 6. strict guard synergy: absorbed escape runs instead of rc 126
out=$(JASH_ADOPT=2 JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=1 \
      "$J" -c 'bash -c "echo unblocked"' 2>/dev/null); rc=$?
[ "$out" = "unblocked" ] && [ $rc -eq 0 ] \
  && ok 'strict mode upgrades instead of blocking (bypass neutralized)' \
  || no 'strict synergy' "out=$out rc=$rc"

# 7. upgraded metric recorded (agent_metrics.tsv gains the column count)
JASH_ADOPT=2 "$J" -c 'bash -c "echo m"' >/dev/null 2>&1
up=$(JASH_ADOPT=2 "$J" -c 'adopt' 2>/dev/null | strip | grep -ci upgrad)
[ "${up:-0}" -ge 1 ] && ok 'adopt dashboard reports shell-assisted upgrades' \
  || no 'metric' 'no upgraded line in adopt output'

echo "== W3 destructive floor (level 3) =="

floor_case() { # $1=label $2=adopt $3=trash $4=expect restored|gone
  echo critical > "$W/x.txt"
  JASH_ADOPT=$2 JASH_TRASH=$3 "$J" -c "cd $W; rm x.txt" >/dev/null 2>&1
  JASH_ADOPT=$2 JASH_TRASH=$3 "$J" -c "cd $W; rewind auto" >/dev/null 2>&1
  local got=gone; [ -f "$W/x.txt" ] && got=restored
  rm -f "$W/x.txt"
  [ "$got" = "$4" ] && ok "$1" || no "$1" "got=$got expected=$4"
}
floor_case 'ADOPT=3 + JASH_TRASH=0: floor wins, rm recoverable'   3 0   restored
floor_case 'ADOPT=3 + JASH_TRASH=off: floor wins (BUG-150 spelling)' 3 off restored
floor_case 'ADOPT=1 + JASH_TRASH=0: bash-parity permanent delete' 1 0   gone
floor_case 'ADOPT=1 + JASH_TRASH=off: off means OFF (BUG-150)'    1 off gone
floor_case 'ADOPT=1 + JASH_TRASH=on: env on still protects'       1 on  restored

echo
echo "PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ]
