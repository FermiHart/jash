#!/usr/bin/env bash
# test_loopguard.sh — anti-loop circuit breaker (ADR-0042 F3).
# "Similar" = same argv[0] + Jaccard(remaining tokens) >= 0.7.
# 3 similar failures → warn, 5 → block, success resets, off disables.
set -u
set -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\[[0-9;]*m//g'; }

fresh_home(){ TH=$(mktemp -d); mkdir -p "$TH/.jash"; bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"; echo "$TH"; }

# Similar failing commands: same argv[0], args differ by 1 of 6 → Jaccard=0.71
SIMILAR='nonexistent_cmd a b c d e f
nonexistent_cmd a b c d e g
nonexistent_cmd a b c d e h
nonexistent_cmd a b c d e i
nonexistent_cmd a b c d e j'

# ---- F3.1: warn at 3rd similar failure (4th command triggers streak>=3) ----
{
  TH=$(fresh_home)
  OUT=$(echo "$SIMILAR" | HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_LOOPGUARD=warn "$J" 2>&1 | strip)
  if echo "$OUT" | grep -q "loopguard.*WARNING\|loopguard.*warn"; then
    ok "warn appears at 3rd similar failure"
  else
    no "warn at 3rd" "no WARNING in output"
  fi
}

# ---- F3.1b: block at 5th similar failure (block mode) ----
{
  TH=$(fresh_home)
  OUT=$(printf 'nonexistent_blk a b c d e f\nnonexistent_blk a b c d e g\nnonexistent_blk a b c d e h\nnonexistent_blk a b c d e i\nnonexistent_blk a b c d e j\nnonexistent_blk a b c d e k\n' \
    | HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_LOOPGUARD=block "$J" 2>&1 | strip)
  if echo "$OUT" | grep -q "loopguard.*BLOCKED\|loopguard.*block"; then
    ok "block fires at 5th similar failure"
  else
    no "block at 5th" "no BLOCKED in output"
  fi
}

# ---- F3.2: dissimilar failures don't trigger ----
{
  TH=$(fresh_home)
  OUT=$(printf 'nonexistent_alpha a b c d e f\nnonexistent_beta a b c d e f\nnonexistent_gamma a b c d e f\nnonexistent_delta a b c d e f\nnonexistent_epsilon a b c d e f\n' \
    | HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_LOOPGUARD=warn "$J" 2>&1 | strip)
  if echo "$OUT" | grep -q "loopguard"; then
    no "dissimilar failures" "loopguard triggered on dissimilar commands"
  else
    ok "dissimilar failures don't trigger loopguard"
  fi
}

# ---- F3.3: success resets the counter ----
{
  TH=$(fresh_home)
  OUT=$(printf 'nonexistent_r a b c d e f\nnonexistent_r a b c d e g\ntrue\nnonexistent_r a b c d e h\nnonexistent_r a b c d e i\nnonexistent_r a b c d e j\n' \
    | HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_LOOPGUARD=warn "$J" 2>&1 | strip)
  if echo "$OUT" | grep -q "loopguard"; then
    no "success resets" "loopguard triggered despite success between failures"
  else
    ok "success between failures resets the counter"
  fi
}

# ---- F3.4: JASH_LOOPGUARD=off disables ----
{
  TH=$(fresh_home)
  OUT=$(printf 'nonexistent_off a b c d e f\nnonexistent_off a b c d e g\nnonexistent_off a b c d e h\nnonexistent_off a b c d e i\nnonexistent_off a b c d e j\n' \
    | HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_LOOPGUARD=off "$J" 2>&1 | strip)
  if echo "$OUT" | grep -q "loopguard"; then
    no "off disables" "loopguard triggered despite JASH_LOOPGUARD=off"
  else
    ok "JASH_LOOPGUARD=off disables loopguard"
  fi
}

echo ""
echo "== loopguard (ADR-0042 F3): $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ] || exit 1
