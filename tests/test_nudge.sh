#!/usr/bin/env bash
# test_nudge.sh — proactive adoption nudge: on command failure jash tips toward
# recall/expect — once per cooldown, only in recording/agent context, never when
# JASH_NUDGE=0. Conservative by design (the popup lesson: aggressive = harmful).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
export JASH_SIGIL=1
cnt(){ printf '%s' "$1" | grep -ic 'tip:'; }

echo "== adoption nudge =="
out="$("$J" -c 'ls /nope-xyz' 2>&1 | strip)"
[ "$(cnt "$out")" = 1 ] && ok 'first failure → nudge toward recall/expect' || no 'first nudge' "$out"
out="$("$J" -c 'ls /nope-2' 2>&1 | strip)"
[ "$(cnt "$out")" = 0 ] && ok 'second failure within cooldown → silent' || no 'cooldown' "$out"
rm -rf "$TH/.jash/nudges"
out="$(JASH_NUDGE=0 "$J" -c 'ls /nope-3' 2>&1 | strip)"
[ "$(cnt "$out")" = 0 ] && ok 'JASH_NUDGE=0 → opt-out honored' || no 'opt-out' "$out"
rm -rf "$TH/.jash/nudges"
out="$(JASH_SIGIL=0 "$J" -c 'ls /nope-4' 2>&1 | strip)"
[ "$(cnt "$out")" = 0 ] && ok 'no recording → no nudge (human not surprised)' || no 'no-record' "$out"
out="$("$J" -c 'echo ok' 2>&1 | strip)"
[ "$(cnt "$out")" = 0 ] && ok 'success → no nudge' || no 'success' "$out"

rm -rf "$TH"
echo "== nudge: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
