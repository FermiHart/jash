#!/usr/bin/env bash
# test_ab_adoption.sh — W7: A/B harness for the adoption machinery (ADR-0040).
# Arm A = JASH_ADOPT=0 (machinery off). Arm B = JASH_ADOPT=2 (nudge+absorb).
# Two invariants, both CI-gated:
#   SAFETY:   adoption must NEVER change functional behavior — stdout and rc
#             of the same task script are byte-identical across arms.
#   EFFICACY: arm B must actually absorb (`bash -c` → jash) and record it in
#             the metrics (shell-upgraded > 0); arm A must record nothing.
# Verified via SCRIPT FILE per the golden rule, never `jash -c` under a jash parent.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
# Deterministic arms: banner off, memo off (repeat reads must return bytes).
export JASH_SIGIL=1 JASH_BRIEF=0 JASH_MEMO=0 JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0

# run LEVEL HOMEDIR SCRIPT — sets OUT/RC/ERR. Fresh cwd per call, shared HOME
# per arm so metrics accumulate where the arm expects them.
run(){
  local lvl="$1" h="$2" body="$3"
  mkdir -p "$h" "$TMP/wd"; rm -rf "$TMP/wd"; mkdir -p "$TMP/wd"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  printf '%s' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$h" JASH_ADOPT="$lvl" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

# Representative agent task: fs work, pipeline, capture, loop, arithmetic,
# and a `bash -c` one-liner (the absorption target).
TASK='mkdir -p w && printf "a\nb\nc\n" > w/f.txt
n=$(grep -c . w/f.txt)
echo "lines=$n"
v=$(bash -c "echo cap=$((2+3))")
echo "got=[$v]"
bash -c "echo inner-ok"
for i in 1 2; do echo "it$i"; done
false; echo "rc-prop=$?"'

echo "== A/B adoption harness (A: JASH_ADOPT=0 · B: JASH_ADOPT=2) =="

HA="$TMP/homeA"; HB="$TMP/homeB"

# ---- SAFETY: functional equality -------------------------------------------
run 0 "$HA" "$TASK"; A_OUT="$OUT"; A_RC="$RC"; A_ERR="$ERR"
run 2 "$HB" "$TASK"; B_OUT="$OUT"; B_RC="$RC"; B_ERR="$ERR"
[ "$A_OUT" = "$B_OUT" ] && ok 'stdout identical across arms' \
  || no 'stdout identical across arms' "A=[$A_OUT]" "B=[$B_OUT]"
[ "$A_RC" = "$B_RC" ] && ok "rc identical across arms ($A_RC)" \
  || no 'rc identical across arms' "A=$A_RC B=$B_RC"
case "$B_OUT" in *'[jash adopt]'*) no 'notice never leaks into stdout/captures' "B=[$B_OUT]";;
  *) ok 'notice never leaks into stdout/captures';; esac

# ---- EFFICACY: arm B absorbs, arm A does not -------------------------------
case "$B_ERR" in *'[jash adopt]'*absorbed*) ok 'arm B absorbs bash -c (stderr notice)';;
  *) no 'arm B absorbs bash -c (stderr notice)' "err=[$B_ERR]";; esac
case "$A_ERR" in *'[jash adopt]'*) no 'arm A stays silent' "err=[$A_ERR]";;
  *) ok 'arm A stays silent';; esac

# Metrics: shell-upgraded counter per arm (fresh HOMEs above).
upg(){ # HOMEDIR LEVEL -> echoes shell-upgraded count
  printf '%s' 'agent-guard status' > "$TMP/wd/s.jsh"
  (cd "$TMP/wd" && HOME="$1" JASH_ADOPT="$2" "$J" s.jsh 2>/dev/null) | strip \
    | grep -o '[0-9][0-9]* shell-upgraded' | grep -o '[0-9]*' || echo 0
}
A_UP="$(upg "$HA" 0)"; B_UP="$(upg "$HB" 2)"
[ "${B_UP:-0}" -ge 1 ] && ok "arm B metrics: shell-upgraded=$B_UP (≥1)" \
  || no 'arm B metrics: shell-upgraded ≥1' "got=$B_UP"
[ "${A_UP:-0}" -eq 0 ] && ok 'arm A metrics: shell-upgraded=0' \
  || no 'arm A metrics: shell-upgraded=0' "got=$A_UP"

# ---- Kill-switch: JASH_ABSORB=0 restores baseline at level 2 ---------------
mkdir -p "$TMP/homeK"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TMP/homeK"
printf '%s' "$TASK" > "$TMP/wd/t.jsh"
K_OUT="$(cd "$TMP/wd" && HOME="$TMP/homeK" JASH_ADOPT=2 JASH_ABSORB=0 "$J" t.jsh 2>"$TMP/e")"; K_RC=$?
K_ERR="$(strip < "$TMP/e")"
[ "$K_OUT" = "$A_OUT" ] && [ "$K_RC" = "$A_RC" ] && ok 'JASH_ABSORB=0 kill-switch == baseline' \
  || no 'JASH_ABSORB=0 kill-switch == baseline' "K=[$K_OUT] rc=$K_RC" "A=[$A_OUT] rc=$A_RC"
case "$K_ERR" in *absorbed*) no 'kill-switch emits no absorb notice' "err=[$K_ERR]";;
  *) ok 'kill-switch emits no absorb notice';; esac

echo "== ab-adoption: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
