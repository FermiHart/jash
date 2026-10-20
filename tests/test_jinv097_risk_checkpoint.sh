#!/usr/bin/env bash
# JINV-097 — risk-gated auto-checkpoint (ADR-0050 W12).
# Before W12 only `rm` armed an auto-checkpoint. Now the shell scores EVERY
# top-level line with the same engine as `explain --risk` (single source,
# builtin_explain.c) and arms `auto` when critical (≥80) — BEFORE executing.
# The gate: critical line arms (and prints why), safe line does not, the
# kill-switch and the meta-command exemption hold.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=1

run(){
  local body="$1"; shift
  mkdir -p "$TMP/h" "$TMP/wd"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$TMP/h"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$TMP/h" env "$@" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}
cps(){ # checkpoints file content (auto entry present?)
  run 'checkpoint list'
  printf '%s' "$OUT"
}

echo "JINV-097 (risk-gated auto-checkpoint):"

# 1. critical line (dd to a device = 80) arms `auto` BEFORE executing.
rm -rf "$TMP/h"; run 'dd if=/dev/zero of=/dev/null bs=1 count=1'
case "$ERR" in
  *"critical line (risk 80%"*"auto-checkpoint armed"*)
    ok "critical line arms auto-checkpoint with reason";;
  *) no "critical hook" "$ERR";; esac
cps | grep -q "auto" && ok "auto checkpoint exists after critical line" \
    || no "auto missing" "$OUT"

# 2. safe line: no notice, no NEW auto entry beyond what rm itself does.
rm -rf "$TMP/h"; run 'echo safe > s.txt'
case "$ERR" in *"critical line"*) no "safe line self-armed" "$ERR";;
  *) ok "safe line: no notice";; esac

# 3. kill-switch JASH_RISK_CHECKPOINT=0: critical line runs silently.
rm -rf "$TMP/h"; run 'dd if=/dev/zero of=/dev/null bs=1 count=1' JASH_RISK_CHECKPOINT=0
case "$ERR" in *"critical line"*) no "kill-switch failed" "$ERR";;
  *) ok "JASH_RISK_CHECKPOINT=0 silences the hook";; esac
cps | grep -q "auto" && no "kill-switch still armed" "$OUT" \
    || ok "kill-switch: no auto checkpoint"

# 4. meta-command exemption: `explain --risk` of a critical line does NOT arm.
rm -rf "$TMP/h"; run 'explain --risk "dd if=/dev/zero of=/dev/null"'
case "$ERR" in *"auto-checkpoint armed"*) no "meta self-triggered" "$ERR";;
  *) ok "meta-commands never self-trigger";; esac

# 5. single source: hook score == explain --risk score for the same line.
run 'explain --json --risk "dd if=/dev/zero of=/dev/null"'
case "$OUT" in *'"score":80'*) ok "same 80 from the shared engine";;
  *) no "engine drift" "$OUT";; esac

# 6. the armed checkpoint is REAL: mutate, then rewind auto reverts.
rm -rf "$TMP/h"
run 'dd if=/dev/zero of=/dev/null bs=1 count=1
echo doomed > d.txt
rewind auto'
case "$OUT" in *"1 reverted"*) ok "rewind auto reverts after W12 arming";;
  *) no "rewind auto" "$OUT";; esac
[ ! -e "$TMP/wd/d.txt" ] && ok "mutation reverted" || no "file still there" "d.txt exists"

echo ""
echo "=== JINV-097: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
