#!/usr/bin/env bash
# test_bypass_nudge.sh — W8: bypass-cost nudge at the moment of bypass (ADR-0040).
# In watch mode (JASH_GUARD_BYPASS=1) a non-strict `bash -c` bypass must emit
# ONE per-session cost line naming the inner command, on real stderr, without
# touching the bypassed command's stdout/rc. Cooldown suppresses repeats in the
# same HOME; JASH_NUDGE=0 and JASH_ADOPT=0 silence the nudge (WARNING stays).
# Verified via SCRIPT FILE per the golden rule, never `jash -c` under a jash parent.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
# Watch mode on, absorption off (the nudge targets the NON-absorbed bypass path),
# banner/memo off for determinism.
export JASH_SIGIL=1 JASH_ADOPT=1 JASH_BRIEF=0 JASH_MEMO=0 JASH_GUARD_BYPASS=1 JASH_GUARD_BYPASS_STRICT=0 JASH_ABSORB=0

# run HOMEDIR BODY [VAR=val ...] — sets OUT/RC/ERR.
run(){
  local h="$1" body="$2"; shift 2
  mkdir -p "$h"; rm -rf "$TMP/wd"; mkdir -p "$TMP/wd"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$h" env "$@" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

echo "test_bypass_nudge (W8):"

# 1. First bypass in a fresh HOME: cost line fires, names the inner command,
#    stdout/rc untouched.
H1="$TMP/h1"
run "$H1" 'bash -c "echo hi"'
if [ "$OUT" = "hi" ] && [ "$RC" -eq 0 ]; then ok "bypass stdout/rc untouched"; else no "bypass stdout/rc" "OUT=$OUT RC=$RC"; fi
case "$ERR" in
  *"bypass cost #1"*"echo hi"*) ok "cost nudge fired naming inner command";;
  *) no "cost nudge missing/incomplete" "$ERR";;
esac
case "$ERR" in
  *"[agent-guard] WARNING"*) ok "watch-mode WARNING still present";;
  *) no "watch-mode WARNING missing" "$ERR";;
esac

# 2. Second bypass, same HOME: cooldown suppresses the repeat cost line.
run "$H1" 'bash -c "echo hi2"'
case "$ERR" in
  *"bypass cost"*) no "cooldown failed: second cost line" "$ERR";;
  *) ok "cooldown suppressed repeat nudge";;
esac
if [ "$OUT" = "hi2" ]; then ok "second bypass output untouched"; else no "second bypass output" "OUT=$OUT"; fi

# 3. JASH_NUDGE=0: nudge silenced, WARNING stays.
run "$TMP/h3" 'bash -c "echo hi3"' JASH_NUDGE=0
case "$ERR" in
  *"bypass cost"*) no "JASH_NUDGE=0 did not silence nudge" "$ERR";;
  *) ok "JASH_NUDGE=0 silences nudge";;
esac
case "$ERR" in
  *"[agent-guard] WARNING"*) ok "WARNING unaffected by JASH_NUDGE=0";;
  *) no "WARNING missing under JASH_NUDGE=0" "$ERR";;
esac

# 4. JASH_ADOPT=0: adoption machinery off silences the nudge too.
run "$TMP/h4" 'bash -c "echo hi4"' JASH_ADOPT=0
case "$ERR" in
  *"bypass cost"*) no "JASH_ADOPT=0 did not silence nudge" "$ERR";;
  *) ok "JASH_ADOPT=0 silences nudge";;
esac

echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
