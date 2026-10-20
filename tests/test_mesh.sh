#!/usr/bin/env bash
# test_mesh.sh — multi-model swarm substrate (ADR-0024). Fleet registry + task
# board with atomic claim, lease-locks, deps gating, blackboard — all --json.
# Each mesh command is passed as ONE -c string so quoted descriptions survive.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"; mkdir -p "$TH/.jash"
M(){ "$J" -c "mesh $1" 2>/dev/null | strip; }   # $1 = full mesh arg string (quotes intact)

echo "== mesh swarm substrate (ADR-0024) =="

# fleet
id=$(M 'register --provider anthropic --model opus-4.8 --id opus1')
[ "$id" = "opus1" ] && ok 'register returns id' || no 'register' "[$id]"
M 'register --provider deepseek --model deepseek-flash --id ds1' >/dev/null
out=$(M 'workers --json')
{ printf '%s' "$out" | grep -q '"agent_id":"opus1"' && printf '%s' "$out" | grep -q '"model":"deepseek-flash"'; } && ok 'workers --json lists fleet' || no 'workers json' "$out"

M 'heartbeat --id opus1 --status working --headroom 75' >/dev/null
printf '%s' "$(M 'workers --json')" | grep -q '"status":"working","headroom":"75"' && ok 'heartbeat updates status/headroom' || no 'heartbeat' "$(M 'workers --json')"

# board + quoted desc
M 'init "build AST"' >/dev/null
M 'add "extract tokenizer from source" --tag mechanical' >/dev/null
M 'add "design AST nodes" --tag reasoning --needs 1' >/dev/null
M 'add "verify the AST" --tag verify --needs 2' >/dev/null
out=$(M 'board --json')
printf '%s' "$out" | grep -q 'extract tokenizer from source' && ok 'quoted desc preserved' || no 'desc' "$out"

# deps gating + atomic claim
c1=$(M 'next --id ds1 --json')
printf '%s' "$c1" | grep -q '"id":1' && ok 'next claims ready task #1' || no 'next #1' "$c1"
c2=$(M 'next --id ds1 --json')
printf '%s' "$c2" | grep -q '^null' && ok 'next blocked: #2 needs #1' || no 'deps gate' "$c2"

# completion unblocks dependent (only the claim owner may close the task)
M 'done 1 --id ds1 --artifact src/lex.c' >/dev/null
c3=$(M 'next --id opus1 --json')
printf '%s' "$c3" | grep -q '"id":2' && ok 'done #1 unblocks #2' || no 'unblock' "$c3"

# worker registry reflects assignment
printf '%s' "$(M 'workers --json')" | grep -q '"task":"2"' && ok 'fleet shows worker→task' || no 'fleet task' "$(M 'workers --json')"

# fail marks failed
M 'add "flaky step"' >/dev/null
fid=$(M 'board --json' | grep -o '"id":[0-9]*' | tail -1 | grep -o '[0-9]*')
M "fail $fid" >/dev/null
printf '%s' "$(M 'board --json')" | grep -q "\"id\":$fid,\"state\":\"failed\"" && ok 'fail marks failed' || no 'fail' "$(M 'board --json')"

# blackboard
M 'post "tokenizer uses arena alloc" --id ds1' >/dev/null
printf '%s' "$(M 'board' )" | grep -q 'arena alloc' && ok 'blackboard post shows in board' || no 'post' "$(M 'board')"

# no-args help, no crash
printf '%s' "$(M '')" | grep -q 'swarm substrate' && ok 'help on no args' || no 'help' "$(M '')"

rm -rf "$TH"
echo "== mesh: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
