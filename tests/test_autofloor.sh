#!/usr/bin/env bash
# test_autofloor.sh — the AUTOMATIC floor: an agent using jash like plain bash
# (no exocortex verbs, JASH_SIGIL UNSET) still gets memory + undo for FREE.
# Recording auto-enables for non-interactive (-c) sessions; rm is protected +
# auto-checkpointed. This is what makes a bash-habit agent already better off.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH" || exit 2
W="$TH/work"; mkdir -p "$W"; echo "critical" > "$W/important.txt"
unset JASH_SIGIL    # the whole point: the agent set NOTHING

echo "== automatic floor (lazy agent == better than bash) =="

# recording auto-on for -c without JASH_SIGIL (use an external cmd → full path;
# the ultra-fast path intentionally skips trivial no-side-effect builtins to keep
# `jash -c` bash-fast, so they are not chained — only meaningful commands are).
"$J" -c "/bin/echo hello" >/dev/null 2>&1
CHAIN="$(ls "$TH"/.jash/chain*.log 2>/dev/null | head -1)"
[ -n "$CHAIN" ] && [ -s "$CHAIN" ] && ok 'recording auto-on for -c (no JASH_SIGIL set)' || no 'auto-record' 'no chain log'

# plain rm → recoverable (protected) + memory + auto-checkpoint, all free
"$J" -c "cd $W; rm important.txt" >/dev/null 2>&1
[ ! -e "$W/important.txt" ] && ok 'plain rm removed the file' || no 'rm' 'still there'
"$J" -c "recall cmds --last 5" 2>/dev/null | strip | grep -q 'rm important.txt' && ok 'memory for free (recall shows the rm)' || no 'recall' 'rm not recorded'
"$J" -c "checkpoint list" 2>/dev/null | strip | grep -qi 'auto' && ok 'auto-checkpoint dropped (no checkpoint typed)' || no 'auto-cp' 'no auto marker'

# bare rewind to the auto point restores it — undo for free, cross -c
"$J" -c "cd $W; rewind auto" >/dev/null 2>&1
[ -f "$W/important.txt" ] && ok 'rewind auto restored it (undo for free, cross -c)' || no 'rewind auto' 'not restored'

# interactive default stays OFF (no surprise logging for humans) — simulate via a tty-less but explicit off
rm -f "$TH"/.jash/chain*.log
JASH_SIGIL=0 "$J" -c "echo x" >/dev/null 2>&1
if compgen -G "$TH/.jash/chain*.log" >/dev/null; then
    no 'opt-out' 'recorded anyway'
else
    ok 'explicit JASH_SIGIL=0 still wins (opt-out honored)'
fi

rm -rf "$TH"
echo "== autofloor: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
