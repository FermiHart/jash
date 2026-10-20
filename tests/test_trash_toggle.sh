#!/usr/bin/env bash
# test_trash_toggle.sh — `trash on|off|auto|status`: user-controlled destructive-op
# protection (ADR-0023 E2 / BUG-027). Explicit toggle > env > sigil default.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
# This gate isolates the trash policy. An inherited strict repository guard is
# an independent control and would correctly block its in-repo deletion case.
export JASH_GUARD_STRICT=0 JASH_GUARD=0 JASH_ADOPT=0
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

echo "== trash protection toggle (ADR-0023 E2 / BUG-027) =="
TH="$(mktemp -d "$ROOT/.trash-toggle.XXXXXX")"; mkdir -p "$TH/.jash"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"

# 1. explicit JASH_SIGIL=0 → recording off → protection OFF (the real opt-out)
out="$(JASH_SIGIL=0 HOME="$TH" "$J" -c 'trash status' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'protection: OFF' && ok 'JASH_SIGIL=0 → protection OFF' || no 'explicit off' "$out"

# 2. recording on (explicit or auto for -c) → protection ON. Protection follows the
#    RUNTIME recording level, so a non-interactive -c with JASH_SIGIL UNSET also
#    auto-enables it (the safety floor) — verify both.
out="$(JASH_SIGIL=1 HOME="$TH" "$J" -c 'trash status' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'protection: ON' && ok 'default ON when JASH_SIGIL>=1' || no 'default ON' "$out"
out="$(env -u JASH_SIGIL HOME="$TH" "$J" -c 'trash status' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'protection: ON' && ok 'auto-ON for -c when sigil unset (floor)' || no 'auto-on floor' "$out"

# 3. `trash off` overrides sigil-on -> rm permanent
W="$TH/work-off"; mkdir -p "$W"; echo X > "$W/f.txt"
JASH_SIGIL=1 HOME="$TH" "$J" -c 'trash off' >/dev/null 2>&1
JASH_SIGIL=1 HOME="$TH" "$J" -c "cd $W; rm f.txt" >/dev/null 2>&1
[ ! -e "$W/f.txt" ] && ok 'trash off → rm permanent (overrides sigil)' || no 'trash off' 'f.txt survived'
if [ -d /home ] && ! JASH_SIGIL=0 HOME="$TH" "$J" -c 'rm --dry-run /home' >/dev/null 2>&1; then
    ok 'critical root remains blocked'
else
    no 'critical root guard' '/home was not preserved or the guard returned success'
fi
out="$(JASH_SIGIL=1 HOME="$TH" "$J" -c 'trash status' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'source: explicit' && ok 'status reports explicit source' || no 'explicit source' "$out"

# 4. `trash on` overrides sigil-off -> rm recoverable + undo restores
W2="$TH/work-on"; mkdir -p "$W2"; echo Y > "$W2/g.txt"
env -u JASH_SIGIL HOME="$TH" "$J" -c 'trash on' >/dev/null 2>&1
S="$TH/u.jsh"; { echo "cd $W2"; echo "rm g.txt"; echo "test ! -e g.txt || exit 97"; echo "undo"; } > "$S"
env -u JASH_SIGIL HOME="$TH" "$J" "$S" >/dev/null 2>&1
[ -f "$W2/g.txt" ] && ok 'trash on → rm recoverable + undo (overrides sigil off)' || no 'trash on' 'g.txt not restored'

# 5. `trash auto` clears explicit -> back to default policy
env -u JASH_SIGIL HOME="$TH" "$J" -c 'trash auto' >/dev/null 2>&1
out="$(env -u JASH_SIGIL HOME="$TH" "$J" -c 'trash status' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'source: default' && ok 'trash auto → reverts to default' || no 'trash auto' "$out"

rm -rf "$TH" "$W" "$W2"
echo "== trash-toggle: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
