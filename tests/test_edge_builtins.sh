#!/usr/bin/env bash
# test_edge_builtins.sh — coverage ratchet (ADR-0044 W0, BMM bit 11):
# real behavioral checks for chown / forge / gossip so they stay OUT of
# tests/builtin_gold.txt. Includes the gossip receive off-by-one regression
# (the ring walk started one slot BEFORE the oldest message — receive
# printed an empty slot instead of the sent message).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; trap 'rm -rf "$TH"' EXIT
export HOME="$TH" JASH_SIGIL=0 JASH_NUDGE=0

echo "== edge builtins: chown, forge, gossip =="

# chown: usage contract + honest per-file errors
"$J" -c 'chown' >/dev/null 2>&1; rc=$?
[ $rc -eq 2 ] && ok 'chown no-args rc=2' || no 'chown no-args rc' "rc=$rc"
out=$("$J" -c 'chown' 2>&1 | strip)
printf '%s' "$out" | grep -q 'usage: chown' && ok 'chown usage text' || no 'chown usage' "$out"
out=$("$J" -c 'chown 0 /nope-edge-x' 2>&1); rc=$?
out=$(printf '%s' "$out" | strip)
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q "cannot chown '/nope-edge-x'" \
  && ok 'chown missing path: rc=1 + honest message' || no 'chown missing path' "rc=$rc $out"
# chown: non-numeric uid/gid rejected BEFORE any chown(2) call (was: atol
# silently parsed 'abc' as 0 — a silent privilege-targeting bug).
out=$("$J" -c 'chown abc /tmp/x' 2>&1); rc=$?
out=$(printf '%s' "$out" | strip)
[ $rc -eq 2 ] && printf '%s' "$out" | grep -q "invalid uid 'abc'" \
  && ok 'chown rejects non-numeric uid rc=2' || no 'chown bad uid' "rc=$rc $out"
out=$("$J" -c 'chown 0:xyz /tmp/x' 2>&1); rc=$?
out=$(printf '%s' "$out" | strip)
[ $rc -eq 2 ] && printf '%s' "$out" | grep -q "invalid gid 'xyz'" \
  && ok 'chown rejects non-numeric gid rc=2' || no 'chown bad gid' "rc=$rc $out"

# forge: explicitly unfinished scaffold, never a passing tautological test.
out=$("$J" -c 'forge "parser tests"' 2>/dev/null | strip)
printf '%s' "$out" | grep -q 'Description: parser tests' \
  && printf '%s' "$out" | grep -q '#error "forge scaffold: implement the test before use"' \
  && ok 'forge emits skeleton with the description' || no 'forge skeleton' "$out"
out=$("$J" -c 'forge --json "cfg"' 2>/dev/null)
printf '%s' "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["status"] == "scaffold" and d["description"] == "cfg"' \
  && ok 'forge --json describes unfinished scaffold' || no 'forge json' "$out"
"$J" -c 'forge' 2>/dev/null | strip | grep -q 'usage: forge' && ok 'forge no-args usage' || no 'forge usage' ""

# gossip: send → receive roundtrip (the off-by-one regression)
out=$("$J" -c 'gossip send "hello fleet"; gossip receive' 2>/dev/null | strip)
printf '%s' "$out" | grep -qE '^\[[0-9]+\] .+: hello fleet$' \
  && ok 'gossip receive shows the sent message' || no 'gossip roundtrip' "$out"
out=$("$J" -c 'gossip send "one"; gossip send "two"; gossip receive --json' 2>/dev/null)
printf '%s' "$out" | grep -q '"message":"one"' \
  && printf '%s' "$out" | grep -q '"message":"two"' \
  && ok 'gossip --json lists both messages' || no 'gossip json' "$out"
out=$("$J" -c 'gossip send "x"; gossip clear; gossip receive' 2>/dev/null | strip)
printf '%s' "$out" | grep -q 'cleared' && ok 'gossip clear' || no 'gossip clear' "$out"
"$J" -c 'gossip' 2>/dev/null | strip | grep -q 'usage: gossip' && ok 'gossip no-args usage' || no 'gossip usage' ""

# goal recall query is data, never a shell fragment (W2 security regression).
goal_marker="$TH/goal-recall-injected"
goal_out="$TH/goal-recall.out"
goal_empty_out=$("$J" -c 'goal recall missing' 2>"$TH/goal-recall-empty.stderr"); goal_empty_rc=$?
if [[ $goal_empty_rc -eq 0 && "$goal_empty_out" == *"no matching checkpoints"* ]]; then
    ok 'goal recall on an empty store is a clean empty result'
else
    no 'goal recall empty store' "rc=$goal_empty_rc out=$goal_empty_out"
fi
"$J" -c 'goal begin "safe fixture"' >/dev/null 2>&1
"$J" -c "goal recall \"'; touch $goal_marker; #\"" >"$goal_out" 2>&1
goal_rc=$?
if [[ ! -e "$goal_marker" && $goal_rc -eq 0 ]]; then
    ok 'goal recall treats shell metacharacters as query data'
else
    no 'goal recall command injection' "rc=$goal_rc marker=$([[ -e "$goal_marker" ]] && echo yes || echo no)"
fi

# The search must also ignore an agent-controlled PATH: no child command is
# needed for this read-only lookup.
fakebin="$TH/fakebin"; mkdir -p "$fakebin"
grep_marker="$TH/goal-grep-path-injected"
cat >"$fakebin/grep" <<'SH'
#!/bin/sh
touch "$GOAL_GREP_MARKER"
exit 1
SH
chmod 0700 "$fakebin/grep"
PATH="$fakebin:/usr/bin:/bin" GOAL_GREP_MARKER="$grep_marker" \
    "$J" -c 'goal recall safe' >/dev/null 2>&1
if [[ ! -e "$grep_marker" ]]; then
    ok 'goal recall does not execute PATH-resolved grep'
else
    no 'goal recall PATH command injection' "marker=$grep_marker"
fi

# HOME is environment data too; context bundle must not interpolate it into
# a shell command when reading recent session metadata.
context_marker="$TH/context-home-injected"
mkdir -p "$TH/.jash"
printf 'cmd=recent-one\ncmd=recent-two\n' > "$TH/.jash/session_meta.jsonl"
context_out=$("$J" -c 'context bundle' 2>"$TH/context-bundle.stderr")
if [[ "$context_out" == *"cmd=recent-one"* && "$context_out" == *"cmd=recent-two"* ]]; then
    ok 'context bundle reads recent session metadata'
else
    no 'context bundle recent metadata' "$context_out"
fi
bad_home="$TH'; touch $context_marker; #"
HOME="$bad_home" "$J" -c 'context bundle' >/dev/null 2>&1
if [[ ! -e "$context_marker" ]]; then
    ok 'context bundle treats HOME as a filesystem path, not shell syntax'
else
    no 'context bundle HOME command injection' "marker=$context_marker"
fi

mkdir -p "$TH/.jash/goals/nested"
printf 'deep-checkpoint-phrase\n' > "$TH/.jash/goals/nested/deep.log"
goal_nested_out=$("$J" -c 'goal recall deep-checkpoint-phrase' 2>"$TH/goal-recall-nested.stderr")
if [[ "$goal_nested_out" == *"goals/nested/deep.log"* ]]; then
    ok 'goal recall still searches nested checkpoint files'
else
    no 'goal recall nested coverage' "$goal_nested_out"
fi

echo "== edge builtins: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
