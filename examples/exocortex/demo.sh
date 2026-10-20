#!/usr/bin/env bash
# examples/exocortex/demo.sh — a runnable end-to-end walkthrough of the jash
# Exocortex toolkit (Docs/EXOCORTEX.md). Self-contained: isolates HOME + a scratch
# workdir so it touches nothing real. Doubles as a smoke test (asserts outcomes).
#
#   bash examples/exocortex/demo.sh        # narrated demo + PASS/FAIL summary
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"; [ -x "$J" ] || J="$(command -v jash)"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
say(){ echo; echo "── $1"; }

TH="$(mktemp -d)"; export HOME="$TH" JASH_SIGIL=1
W="$TH/work"; mkdir -p "$W"; echo "v1" > "$W/config.txt"; echo "build output" > "$W/app.bin"

say "1. MEMORY — recall what was run (the chain remembers across tool-calls)"
"$J" -c "echo first task" >/dev/null 2>&1
"$J" -c "ls /definitely-not-here" >/dev/null 2>&1
"$J" -c "recall failed" 2>&1 | strip | grep -q 'definitely-not-here' && ok "recall surfaces the failed command" || no "recall failed" "miss"

say "2. FORESIGHT — explain a destructive plan WITHOUT running it"
out=$(env -u JASH_SIGIL HOME="$TH" "$J" -c "explain \"rm $W/config.txt && cc -o app main.c\"" 2>&1 | strip)
printf '%s' "$out" | grep -q 'DELETE' && printf '%s' "$out" | grep -q 'external' && ok "explain shows DELETE + external (not executed)" || no "explain" "$out"
[ -f "$W/config.txt" ] && ok "explain did NOT touch the file" || no "explain side-effect" "file gone!"

say "3. SAFETY NET — checkpoint, do risky work, then rewind"
S="$TH/scn.jsh"
{ echo "cd $W"; echo "checkpoint base"; echo "export DEPLOY=prod"; echo "mv config.txt config.bak"; echo "rm app.bin"; echo "checkpoint diff base"; echo "rewind base"; } > "$S"
"$J" "$S" >/dev/null 2>&1
{ [ -f "$W/config.txt" ] && [ ! -e "$W/config.bak" ] && [ -f "$W/app.bin" ]; } \
  && ok "rewind restored env + mv + rm" || no "rewind" "state not restored"

say "4. CERTAINTY — expect holds you to your mental model"
"$J" -c "expect file $W/config.txt" >/dev/null 2>&1 && ok "expect file (true) → rc0" || no "expect true" "rc!=0"
"$J" -c "expect file $W/ghost.txt" >/dev/null 2>&1 && no "expect false" "rc0!" || ok "expect file (false) → rc1 (loud stop)"

say "5. CONTEXT GOVERNOR — huge output is capped + held for recall"
big="$W/big.log"; for i in $(seq 1 80); do echo "log-line-$i-padding-padding"; done > "$big"
gov=$(JASH_LLM_BUDGET=200 "$J" -c "/bin/cat $big" 2>&1 | strip)
printf '%s' "$gov" | grep -q 'context-governor' && ok "over-budget output capped + points to recall" || no "governor" "$gov"

say "6. COORDINATION — mesh: a multi-model fleet + a task board"
"$J" -c "mesh register --provider anthropic --model opus-4.8 --id opus" >/dev/null 2>&1
"$J" -c "mesh register --provider deepseek --model flash --id ds" >/dev/null 2>&1
"$J" -c 'mesh init "ship feature"' >/dev/null 2>&1
"$J" -c 'mesh add "heavy reasoning" --tag reasoning' >/dev/null 2>&1
"$J" -c 'mesh add "grunt edit" --tag mechanical --needs 1' >/dev/null 2>&1
claim=$("$J" -c "mesh next --id ds --json" 2>&1 | strip)
printf '%s' "$claim" | grep -q '"id":1' && ok "mesh: worker claimed the ready task" || no "mesh next" "$claim"
"$J" -c "mesh workers --json" 2>&1 | strip | grep -q '"model":"opus-4.8"' && ok "mesh: fleet visible (feeds 'LLMs online' UI)" || no "mesh workers" "miss"

rm -rf "$TH"
echo
echo "══ exocortex demo: $PASS passed, $FAIL failed ══"
[ "$FAIL" -eq 0 ]
