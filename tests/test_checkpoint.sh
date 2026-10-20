#!/usr/bin/env bash
# test_checkpoint.sh — checkpoint/rewind: time-travel over the session timeline
# (ADR-0023 E2). Rewind reuses the undo engine; coverage = trash/rm, mv, export,
# unset, cd. Isolated via temp HOME so the real ~/.jash is untouched.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

echo "== checkpoint/rewind (ADR-0023 E2) =="

# ---- single-process: full cycle reverts every journaled mutation ----
TH="$(mktemp -d)"; W="$TH/.cptest_w"; mkdir -p "$TH/.jash" "$W/sub"
echo A > "$W/alpha.txt"; echo B > "$W/beta.txt"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
S="$TH/s.jsh"
{
  echo "cd $W"
  echo "checkpoint base"
  echo "export CPFOO=bar"
  echo "mv alpha.txt alpha.moved"
  echo "rm beta.txt"
  echo "cd sub"
  echo "rewind base"
  echo "echo CPFOO_IS=[\$CPFOO]"
  echo "pwd"
} > "$S"
out="$(JASH_SIGIL=1 HOME="$TH" "$J" "$S" 2>&1 | strip)"

printf '%s' "$out" | grep -q 'CPFOO_IS=\[\]'              && ok 'env (export) reverted'        || no 'env (export) reverted' "$out"
[ -f "$W/alpha.txt" ] && [ ! -e "$W/alpha.moved" ]        && ok 'mv reverted'                  || no 'mv reverted' "ls: $(ls "$W")"
[ -f "$W/beta.txt" ]                                       && ok 'rm reverted (recoverable)'    || no 'rm reverted' "beta missing"
printf '%s' "$out" | tail -1 | grep -q "^$W\$"            && ok 'cwd restored to checkpoint'   || no 'cwd restored' "$(printf '%s' "$out" | tail -1)"
printf '%s' "$out" | grep -q 'reverted, 0 failed'         && ok 'rewind summary, 0 failed'     || no 'rewind summary' "$out"

# ---- checkpoint list shows the marker + depth ----
out="$(JASH_SIGIL=1 HOME="$TH" "$J" -c 'checkpoint here; checkpoint list' 2>&1 | strip)"
printf '%s' "$out" | grep -q 'here'                       && ok 'checkpoint list shows marker'  || no 'checkpoint list' "$out"

# ---- rewind to unknown name -> rc 1 ----
JASH_SIGIL=1 HOME="$TH" "$J" -c 'rewind no_such_cp' >/dev/null 2>&1
[ $? -eq 1 ]                                               && ok 'rewind unknown name -> rc 1'  || no 'rewind unknown -> rc1' "rc=$?"

# ---- rewind --session rolls everything back ----
TH2="$(mktemp -d)"; W2="$TH2/.cptest_w2"; mkdir -p "$TH2/.jash" "$W2"; echo Z > "$W2/z.txt"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH2"
S2="$TH2/s.jsh"; { echo "cd $W2"; echo "export SESSV=1"; echo "rm z.txt"; echo "rewind --session"; echo "echo SESSV_IS=[\$SESSV]"; } > "$S2"
out="$(JASH_SIGIL=1 HOME="$TH2" "$J" "$S2" 2>&1 | strip)"
{ [ -f "$W2/z.txt" ] && printf '%s' "$out" | grep -q 'SESSV_IS=\[\]'; } && ok 'rewind --session reverts all' || no 'rewind --session' "$out / z=$([ -f "$W2/z.txt" ] && echo y || echo n)"

# ---- rewind --dry previews without reverting; checkpoint diff lists mutations ----
THD="$(mktemp -d)"; WD="$THD/.cptest_dry"; mkdir -p "$THD/.jash" "$WD"; echo A > "$WD/a.txt"
bash "$ROOT/tests/setup_sigil_modules.sh" "$THD"
SD="$THD/d.jsh"; { echo "cd $WD"; echo "checkpoint c0"; echo "mv a.txt a.moved"; echo "rewind --dry c0"; } > "$SD"
outd="$(JASH_SIGIL=1 HOME="$THD" "$J" "$SD" 2>&1 | strip)"
{ printf '%s' "$outd" | grep -q 'dry run' && [ -e "$WD/a.moved" ] && [ ! -f "$WD/a.txt" ]; } \
  && ok 'rewind --dry previews, does not revert' || no 'rewind --dry' "$outd"
SD2="$THD/d2.jsh"; { echo "cd $WD"; echo "checkpoint c1"; echo "export DIFFV=1"; echo "checkpoint diff c1"; } > "$SD2"
outd2="$(JASH_SIGIL=1 HOME="$THD" "$J" "$SD2" 2>&1 | strip)"
printf '%s' "$outd2" | grep -q 'env-set' && ok 'checkpoint diff lists mutations' || no 'checkpoint diff' "$outd2"
rm -rf "$WD" "$THD"

# ---- cross -c persistence: checkpoint in one process, rewind in another ----
TH3="$(mktemp -d)"; W3="$TH3/.cptest_w3"; mkdir -p "$TH3/.jash" "$W3"; echo K > "$W3/k.txt"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH3"
env JASH_SIGIL=1 JASH_SESSION=cp HOME="$TH3" "$J" -c "cd $W3; checkpoint cross; mv k.txt k.moved" >/dev/null 2>&1
env JASH_SIGIL=1 JASH_SESSION=cp HOME="$TH3" "$J" -c "rewind cross" >/dev/null 2>&1
{ [ -f "$W3/k.txt" ] && [ ! -e "$W3/k.moved" ]; } && ok 'cross -c: checkpoint+rewind persist' || no 'cross -c persist' "ls: $(ls "$W3")"

rm -rf "$W" "$W2" "$W3" "$TH" "$TH2" "$TH3"
echo "== checkpoint: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
