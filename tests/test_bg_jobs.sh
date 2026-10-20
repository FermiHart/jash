#!/usr/bin/env bash
# tests/test_bg_jobs.sh — ADR-0042 F6: cross-`-c` async jobs (`jash --bg`).
#
# `jash --bg -c '<cmd>'` returns immediately with {"job":N}; the command runs
# detached (setsid) with full sigil attestation, its output to
# ~/.jash/jobs/<id>.out. The record transitions running→done; `wait <id>` blocks
# for it; a later `-c` envelope reports "jobs_done":[N] exactly once; `jobs out`
# pages the output.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"          # so sigil.modjash resolves via ./modules/sigil/
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
if ! JASH_SIGIL=0 JASH_BRIEF=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; then
  JASH="$PROJECT_ROOT/build-speed/jash-release"
fi
if ! JASH_SIGIL=0 JASH_BRIEF=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; then
  echo "SKIP: no runnable jash binary"
  exit 0
fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# SIGIL=2 so the backgrounded command is chained (criterion 6); MEMO=0 keeps a
# parity-style harness free of the memo output-optimization confound. $H is the
# active HOME — criteria 4 and 6 use their own so a prior criterion's jobs can't
# pollute the jobs_done count or the (single-writer) attestation chain.
H="$T/home"
run() { env -i HOME="$H" PATH="$PATH" TERM=dumb \
            JASH_SIGIL=2 JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 "$@"; }
jid_of() { sed -n 's/.*"job":\([0-9]*\).*/\1/p'; }
fresh_home() {
  H="$T/$1"
  mkdir -p "$H"
  bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$H"
  run "$JASH" -c true >/dev/null 2>&1
}

run "$JASH" -c true >/dev/null 2>&1   # warm ~/.jash setup

# ── 1. --bg returns immediately with {"job":N}; job is still running ──────────
out=$(run "$JASH" --bg -c 'sleep 3')
jid=$(printf '%s' "$out" | jid_of)
case "$out" in
  *'"job":'*) ok "--bg prints {\"job\":$jid}";;
  *) err "--bg output ('$out')";;
esac
# Parent returned before the 3s job finished ⇒ non-blocking.
list=$(run "$JASH" -c 'jobs --json')
case "$list" in
  *"\"id\":$jid"*"\"state\":\"running\""*) ok "parent returned while job still running (non-blocking)";;
  *) err "job not running after --bg ('$list')";;
esac
# This criterion proves detachment, but the test must not leave that detached
# process alive while later criteria replace `jid` with their own jobs.
run "$JASH" -c "wait $jid" >/dev/null

# ── 2. record transitions running→done with correct rc ───────────────────────
out=$(run "$JASH" --bg -c 'sleep 1; exit 7')
jid=$(printf '%s' "$out" | jid_of)
mid=$(run "$JASH" -c 'jobs --json')
[[ "$mid" == *"\"id\":$jid"*"\"state\":\"running\""* ]] \
    && ok "mid-flight record is running" || err "mid-flight not running ('$mid')"
run "$JASH" -c "wait $jid" >/dev/null; wrc=$?
[[ $wrc -eq 7 ]] && ok "wait returns the job's exit code (7)" || err "wait rc ($wrc)"
donej=$(run "$JASH" -c 'jobs --json')
if printf '%s' "$donej" | python3 -c '
import json, sys
rows = [row for row in json.load(sys.stdin) if row["id"] == int(sys.argv[1])]
assert len(rows) == 1 and rows[0]["state"] == "done" and rows[0]["rc"] == 7
' "$jid"; then
    ok "record transitioned to done (rc 7)"
else
    err "not done ('$donej')"
fi

# ── 3. wait blocks and returns the completion record ─────────────────────────
out=$(run "$JASH" --bg -c 'sleep 1; echo x')
jid=$(printf '%s' "$out" | jid_of)
rec=$(run "$JASH" -c "wait $jid")
case "$rec" in
  *'"state":"done"'*"\"id\":$jid"*|*"\"id\":$jid"*'"state":"done"'*)
      ok "wait blocks then prints the done record";;
  *) err "wait record ('$rec')";;
esac

# ── 4. next -c envelope carries jobs_done exactly once (own HOME) ─────────────
fresh_home h4
out=$(run "$JASH" --bg -c 'true')
jid=$(printf '%s' "$out" | jid_of)
run "$JASH" -c "wait $jid" >/dev/null
run JASH_AGENT=1 "$JASH" -c true 2>"$T/e1" >/dev/null
run JASH_AGENT=1 "$JASH" -c true 2>"$T/e2" >/dev/null
if grep -q "\"jobs_done\":\[$jid\]" "$T/e1"; then ok "envelope reports jobs_done:[$jid]"
else err "jobs_done missing ($(grep -o '\"jobs_done\":\[[^]]*\]' "$T/e1"))"; fi
if grep -q '"jobs_done"' "$T/e2"; then err "jobs_done repeated on 2nd -c"
else ok "jobs_done is one-shot (absent on 2nd -c)"; fi

# ── 5. output readable via `jobs out` ────────────────────────────────────────
out=$(run "$JASH" --bg -c 'echo HELLO-OUT-MARKER')
jid=$(printf '%s' "$out" | jid_of)
run "$JASH" -c "wait $jid" >/dev/null
o=$(run "$JASH" -c "jobs out $jid")
[[ "$o" == *HELLO-OUT-MARKER* ]] && ok "jobs out pages the job's stdout" \
                                 || err "jobs out ('$o')"

# ── 6. backgrounded command is attested (sigil chain verify) — own HOME so the
#      single-writer chain isn't interleaved by earlier criteria's jobs ────────
fresh_home h6
out=$(run "$JASH" --bg -c 'echo CHAIN-MARKER')
jid=$(printf '%s' "$out" | jid_of)
run "$JASH" -c "wait $jid" >/dev/null
v=$(run "$JASH" -c 'sigil chain verify' 2>&1)
# ADR-0051 W6: inside a repo the chain is per-repo (chain-<key>.log); the
# historic global chain.log only exists outside one. Match either.
if grep -q "CHAIN-MARKER" "$H"/.jash/chain*.log 2>/dev/null \
   && printf '%s' "$v" | grep -qiE 'intact|valid|verified'; then
  ok "backgrounded command is in the attested chain (verify passes)"
else
  err "attestation (chain='$(grep -c CHAIN-MARKER "$H"/.jash/chain*.log 2>/dev/null)' verify='$v')"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  bg jobs (ADR-0042 F6) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ bg-jobs tests PASSED"; exit 0; } \
                  || { echo "  ✗ bg-jobs tests FAILED"; exit 1; }
