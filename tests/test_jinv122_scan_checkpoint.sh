#!/usr/bin/env bash
# JINV-122: the authenticated scan checkpoint must be SEMANTICALLY INVISIBLE.
#
# The checkpoint (chain-<repo>.log.scan) exists only to stop startup from being
# O(history). It is a cache, so the only property that matters is that it never
# changes an answer: every query must return exactly what a full re-scan
# returns, and a checkpoint that LIES must be rejected, not believed.
#
# Two hard-won facts about testing this subsystem, both of which produced false
# alarms before being understood:
#
#   1. Almost every jash invocation APPENDS an entry and ADVANCES the ratchet.
#      `sigil quarantine status --json` is the exception: it neither appends nor
#      advances, which is why it is the oracle used here. `sigil chain verify`
#      DOES append -- do not use it as a repeated probe.
#   2. Restoring a chain file without also restoring the ratchet key makes the
#      ratchet look ahead of the chain, so the truncation detector correctly
#      screams LOG TRUNCADO at the harness itself. Snapshot ~/.jash whole.

set -u
cd "$(dirname "$0")/.." || exit 2
PROJECT_ROOT="$PWD"
T=$(mktemp -d "${TMPDIR:-/tmp}/jinv122.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
can_run() { env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [ "${JASH+x}" = x ]; then
    can_run "$JASH" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    JASH=
    for candidate in "$PROJECT_ROOT/build/jash" "$PROJECT_ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then JASH="$candidate"; break; fi
    done
    if [ -z "$JASH" ]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
MOD="$PROJECT_ROOT/modules/sigil/sigil.modjash"
[ -f "$MOD" ] || { echo "SKIP: $MOD missing"; exit 77; }

pass=0 fail=0
ok()  { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H="$T/home"; R="$T/repo"
mkdir -p "$H/.jash/modules" "$R" || exit 1
git -C "$R" init -q || { echo "FAIL: could not initialize repository fixture"; exit 1; }
bash "$PWD/tests/setup_sigil_modules.sh" "$H" || exit 1

# $1 = command; checkpoint ENABLED
run() { (cd "$R" && HOME="$H" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
    JASH_LOOPGUARD=off "$JASH" -c "$1" 2>&1); }
# $1 = command; checkpoint DISABLED -- the ground truth, a full re-scan
truth() { (cd "$R" && HOME="$H" JASH_SIGIL=2 JASH_NUDGE=0 JASH_BRIEF=0 \
    JASH_LOOPGUARD=off JASH_SIGIL_CKPT=0 "$JASH" -c "$1" 2>&1); }

Q='sigil quarantine status --json'

for i in 1 2 3 4 5; do
    run "echo e$i" >/dev/null || { echo "FAIL: could not seed entry $i"; exit 1; }
done
# Do not let a missing scoped ledger become a SKIP, or choose one arbitrarily.
chains=()
for candidate in "$H"/.jash/chain*.log; do
    [ -f "$candidate" ] && chains+=("$candidate")
done
[ "${#chains[@]}" -eq 1 ] || {
    echo "FAIL: expected one scoped chain, found ${#chains[@]}"; exit 1;
}
CHAIN=${chains[0]}
case "$CHAIN" in
    */chain-*.log) ;;
    *) echo "FAIL: expected repository-scoped chain, got $CHAIN"; exit 1 ;;
esac
[ -s "$CHAIN" ] || { echo "FAIL: scoped chain is empty"; exit 1; }
SCAN="$CHAIN.scan"

# --- G1: the checkpoint exists and is not world-readable --------------------
run "$Q" >/dev/null
if [ -f "$SCAN" ]; then ok "G1a checkpoint written"
else bad "G1a checkpoint written" "$SCAN absent"; fi

# See test_jinv111: blindly trying BSD then GNU stat misfires on GNU, which prints to
# stdout before failing. Select the dialect instead of chaining.
if stat -c '%a' . >/dev/null 2>&1; then
    mode=$(stat -c '%a' "$SCAN" 2>/dev/null)      # GNU coreutils
else
    mode=$(stat -f '%Lp' "$SCAN" 2>/dev/null)     # BSD / macOS
fi
if [ "$mode" = "600" ]; then ok "G1b checkpoint is 0600"
else bad "G1b checkpoint is 0600" "mode=$mode"; fi

# --- G2/G3: cache == full re-scan, before and after growth ------------------
a_cache=$(run "$Q"); a_truth=$(truth "$Q")
if [ "$a_cache" = "$a_truth" ]; then ok "G2 cache == full re-scan"
else bad "G2 cache == full re-scan" "cache=$a_cache truth=$a_truth"; fi

for i in 6 7 8; do run "echo e$i" >/dev/null; done
b_cache=$(run "$Q"); b_truth=$(truth "$Q")
if [ "$b_cache" = "$b_truth" ]; then ok "G3a delta fold == full re-scan"
else bad "G3a delta fold == full re-scan" "cache=$b_cache truth=$b_truth"; fi
if [ "$b_cache" != "$a_cache" ]; then ok "G3b growth actually observed"
else bad "G3b growth actually observed" "still $b_cache after 3 new entries"; fi

# The oracle must not perturb what it measures, or every gate below is circular.
before=$(wc -l < "$CHAIN"); run "$Q" >/dev/null; after=$(wc -l < "$CHAIN")
if [ "$before" = "$after" ]; then ok "G3c oracle is non-perturbing"
else bad "G3c oracle is non-perturbing" "chain grew $before -> $after"; fi

cp "$SCAN" "$T/scan.good"

# --- G4: a checkpoint that LIES must be rejected, not believed --------------
# Each mutation forges a false value. If the checkpoint were trusted blindly,
# the lie would surface in the answer. Truth is recomputed for every case.
mutate() { # $1 = sed expr, $2 = label
    sed "$1" "$T/scan.good" > "$SCAN"
    got=$(run "$Q")
    want=$(truth "$Q")
    if [ "$got" = "$want" ]; then ok "G4 $2 rejected"
    else bad "G4 $2 rejected" "leaked: $got (truth: $want)"; fi
}
mutate 's/^s7_entries [0-9]*/s7_entries 999/'   'forged entry count'
mutate 's/^offset [0-9]*/offset 0/'             'forged offset'
mutate 's/^seam_seq [0-9]*/seam_seq 99999/'     'forged seam seq'
mutate 's/^tag_lines [0-9]*/tag_lines 999/'     'forged tag count'
mutate 's/ckpt v1/ckpt v0/'                     'version downgrade'
mutate 's|^chain .*|chain /tmp/other-chain.log|' 'rebind to other chain'
mutate 's/^mac \(.\)/mac f/'                    'forged MAC'

# --- G5: after rejecting a forgery the checkpoint heals ---------------------
sed 's/^s7_entries [0-9]*/s7_entries 999/' "$T/scan.good" > "$SCAN"
run "$Q" >/dev/null
if grep -q '^s7_entries 999' "$SCAN"; then
    bad "G5 forged checkpoint is healed" "lie still on disk"
elif [ "$(run "$Q")" = "$(truth "$Q")" ]; then
    ok "G5 forged checkpoint is healed"
else
    bad "G5 forged checkpoint is healed" "post-heal answer diverges"
fi

# --- G6: the VERDICT never consumes the cache ------------------------------
# A cache may accelerate tallies; it must never be able to affect a verdict.
# Snapshot the whole ~/.jash so chain and ratchet stay consistent (see header).
cp -R "$H/.jash" "$T/snap"
restore() { rm -rf "$H/.jash"; cp -R "$T/snap" "$H/.jash"; }

restore; v_ok=$(run 'sigil chain verify'); rc_ok=$?
restore; sed 's/^s7_entries [0-9]*/s7_entries 999/' "$T/scan.good" > "$SCAN"
v_bad=$(run 'sigil chain verify'); rc_bad=$?
restore; rm -f "$SCAN"
v_none=$(run 'sigil chain verify'); rc_none=$?

if [ "$v_ok" = "$v_none" ] && [ "$rc_ok" = "$rc_none" ]; then
    ok "G6a verdict identical with and without checkpoint"
else
    bad "G6a verdict identical with and without checkpoint" "$v_ok / $v_none"
fi
if [ "$v_bad" = "$v_none" ] && [ "$rc_bad" = "$rc_none" ]; then
    ok "G6b forged checkpoint cannot move the verdict"
else
    bad "G6b forged checkpoint cannot move the verdict" "$v_bad / $v_none"
fi

# --- G7: a broken chain still fails, checkpoint present or not -------------
# Guards against the cache masking real corruption on the next startup.
restore
sed '2s/cmd=echo e2/cmd=echo eX/' "$T/snap/$(basename "$CHAIN")" > "$CHAIN"
out=$(run 'sigil chain verify'); rc=$?
if [ "$rc" != "0" ] && printf '%s' "$out" | grep -q 'CHAIN BROKEN'; then
    ok "G7 tampered entry still breaks the chain"
else
    bad "G7 tampered entry still breaks the chain" "rc=$rc out=$(printf '%s' "$out" | head -1)"
fi

printf '\n=== JINV-122: %d pass, %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
