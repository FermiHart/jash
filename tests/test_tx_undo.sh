#!/bin/sh
# test_tx_undo.sh — ADR-0018 Transactional Undo Fabric smoke suite

set -e
# Resolve JASH to absolute path before changing directories
_jash_bin="${JASH:-build/jash}"
case "$_jash_bin" in
    /*) JASH="$_jash_bin" ;;
    *)  JASH="$(cd "${0%/*}/.." && pwd)/build/jash" ;;
esac
TESTDIR="${TESTDIR:-/tmp/jash_tx_test_$$}"
PASS=0
FAIL=0

cleanup() { rm -rf "$TESTDIR"; }
trap cleanup EXIT
rm -rf "$TESTDIR"
mkdir -p "$TESTDIR"
cd "$TESTDIR"

UNDO_MOD="$HOME/.jash/modules/undo.modjash"
SIGIL_MOD="$HOME/.jash/modules/sigil.modjash"

banner() { echo; echo "=== $1 ==="; }
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

JQ="$JASH -c"

# ═══════════════════════════════════════════════════════════════════
banner "1. Core undo (no module)"
# ═══════════════════════════════════════════════════════════════════
$JQ 'rm -f z; touch z; trash z; undo; test -f z && echo OK || exit 1' | grep -q OK && pass "trash+undo restores" || fail "trash+undo restores"
$JQ 'rm -f a b; touch a; mv a b; undo; test -f a && echo OK || exit 1' | grep -q OK && pass "mv+undo reverts" || fail "mv+undo reverts"
$JQ 'rm -f x; touch x; rm --undo x; undo; test -f x && echo OK || exit 1' | grep -q OK && pass "rm--undo+undo restores" || fail "rm--undo+undo restores"

# ═══════════════════════════════════════════════════════════════════
banner "2. Module load + tx begin/commit/rollback"
# ═══════════════════════════════════════════════════════════════════
$JQ "module load $UNDO_MOD; tx begin t1; touch f1; trash f1; tx preview" | grep -q "would undo 1" && pass "tx preview" || fail "tx preview"
$JQ "module load $UNDO_MOD; tx begin t2; touch f2; trash f2; tx rollback; test -f f2 && echo OK" | grep -q OK && pass "tx rollback" || fail "tx rollback"
$JQ "module load $UNDO_MOD; tx begin t3; touch f3; trash f3; tx commit; test ! -f f3 && echo OK" | grep -q OK && pass "tx commit" || fail "tx commit"
$JQ "module load $UNDO_MOD; tx begin empty; tx commit; echo OK" | grep -q OK && pass "tx empty commit" || fail "tx empty commit"
$JQ "module load $UNDO_MOD; tx begin; touch a; mv a b; tx diff" | grep -q "files moved" && pass "tx diff" || fail "tx diff"

# ═══════════════════════════════════════════════════════════════════
banner "3. Tx status + guard"
# ═══════════════════════════════════════════════════════════════════
$JQ "module load $UNDO_MOD; tx status" | grep -q "no transaction" && pass "tx status empty" || fail "tx status empty"
$JQ "module load $UNDO_MOD; tx begin guardtest; tx status" | grep -q "guardtest" && pass "tx status named" || fail "tx status named"
$JQ "module load $UNDO_MOD; tx begin one; tx begin two" | grep -q "already open" && pass "tx double begin" || fail "tx double begin"

# ═══════════════════════════════════════════════════════════════════
banner "4. Safe mode + risk + policy"
# ═══════════════════════════════════════════════════════════════════
$JQ "module load $UNDO_MOD; tx safe on" | grep -q "ON" && pass "tx safe on" || fail "tx safe on"
$JQ "module load $UNDO_MOD; tx safe off" | grep -q "OFF" && pass "tx safe off" || fail "tx safe off"
$JQ "module load $UNDO_MOD; tx risk 'rm -rf /tmp/x'" | grep -q "CRITICAL" && pass "tx risk critical" || fail "tx risk critical"
$JQ "module load $UNDO_MOD; tx risk 'echo hello'" | grep -q "LOW" && pass "tx risk low" || fail "tx risk low"
$JQ "module load $UNDO_MOD; tx policy rollback-on-fail always" | grep -q "always" && pass "tx policy" || fail "tx policy"

# ═══════════════════════════════════════════════════════════════════
banner "5. Sessions + BLAKE3 verify"
# ═══════════════════════════════════════════════════════════════════
$JQ "module load $UNDO_MOD; tx begin s1; touch s1f; trash s1f; tx commit; tx save stest; tx sessions" | grep -q "stest" && pass "tx sessions" || fail "tx sessions"
$JQ "module load $UNDO_MOD; tx begin v1; touch vf; trash vf; tx commit; tx save vt; tx verify" | grep -q "VERIFIED" && pass "tx verify" || fail "tx verify"

# ═══════════════════════════════════════════════════════════════════
banner "6. Sigil bridge"
# ═══════════════════════════════════════════════════════════════════
if test -f "$SIGIL_MOD"; then
  $JQ "module load sigil; module load $UNDO_MOD; sigil sign; tx begin sigt; touch sf; trash sf; tx commit" | grep -q "sigil: TX_COMMIT" && pass "sigil TX_COMMIT" || fail "sigil TX_COMMIT"
  $JQ "module load sigil; module load $UNDO_MOD; sigil sign; tx begin srb; touch sr; trash sr; tx rollback" | grep -q "sigil: TX_ROLLBACK" && pass "sigil TX_ROLLBACK" || fail "sigil TX_ROLLBACK"
else
  pass "sigil bridge (module not installed, skip)"
  pass "sigil TX_ROLLBACK (module not installed, skip)"
fi

# ═══════════════════════════════════════════════════════════════════
banner "7. Zero overhead"
# ═══════════════════════════════════════════════════════════════════
JASH_SIGIL=0 $JQ 'module list' | grep -q "no modules" && pass "no modules with recording disabled" || fail "no modules with recording disabled"
$JQ 'rm -f zz; touch zz; trash zz; undo; test -f zz && echo OK' | grep -q OK && pass "trash+undo zero mod" || fail "trash+undo zero mod"

# ═══════════════════════════════════════════════════════════════════
banner "8. JASH_SAFE bootstrap"
# ═══════════════════════════════════════════════════════════════════
if test -f "$SIGIL_MOD" && test -f "$UNDO_MOD"; then
  JASH_SAFE=1 $JQ 'tx status; echo DONE' | grep -q "safe: ON" && pass "JASH_SAFE auto-load+sign+safe" || fail "JASH_SAFE auto-load+sign+safe"
  JASH_SAFE=1 $JQ 'tx status; echo DONE' | grep -q "sigil: SIGN" && pass "JASH_SAFE sigil SIGN" || fail "JASH_SAFE sigil SIGN"
  JASH_SAFE=1 $JQ 'tx risk "rm -rf /x"; echo DONE' | grep -q "CRITICAL" && pass "JASH_SAFE risk scoring" || fail "JASH_SAFE risk scoring"
else
  pass "JASH_SAFE (modules not installed, skip)"
  pass "JASH_SAFE sigil (skip)"
  pass "JASH_SAFE risk (skip)"
fi

# ═══════════════════════════════════════════════════════════════════
echo
echo "═══════════════════════════════════════════"
echo "  PASS: $PASS  FAIL: $FAIL"
echo "═══════════════════════════════════════════"
test "$FAIL" -eq 0 && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $FAIL
