#!/usr/bin/env bash
# test_mem.sh — ADR-0054 mem: session memory distilled from the attested
# chain. Covers W1 (distill/store/attest/idempotency), W2 (typed
# observations + outcome-aware search), W3 (brief), W4 (verify/DRIFT),
# W5 (budget, json, honest errors).
#
# Fixture: three attested sessions via JASH_SESSION (sess= is the split key):
#   alpha — `cat alvo` fails, then succeeds after the file exists → bugfix
#   beta  — read-only verbs only → discovery
#   gamma — redirect write → change
# cwd is a temp dir outside any git repo (repokey "-"): chain.log / mem.tsv.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
[ -x "$J" ] || { echo "SKIP: no jash binary"; exit 0; }
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; FIX="$(mktemp -d)"; bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"; export HOME="$TH" JASH_SIGIL=1
M(){ (cd "$FIX" && "$J" -c "mem $1" 2>/dev/null | strip); }
# Mrc: same, but propagates jash's rc through the strip pipe
Mrc(){ (cd "$FIX" && "$J" -c "mem $1" 2>/dev/null | strip; exit "${PIPESTATUS[0]}"); }
# Merr: stdout+stderr merged (errors go to stderr), rc preserved
Merr(){ (cd "$FIX" && "$J" -c "mem $1" 2>&1 | strip; exit "${PIPESTATUS[0]}"); }
RUN(){ (cd "$FIX" && JASH_SESSION="$1" "$J" -c "$2" >/dev/null 2>&1); }
STORE="$TH/.jash/mem.tsv"
CHAIN="$TH/.jash/chain.log"

echo "== mem: distill, typed observations, brief, verify, tamper =="

# ── fixture: three attested sessions ─────────────────────────────────────────
RUN alpha "cat \"$FIX/alvo\""                 # rc=1 (missing)
: > "$FIX/alvo"
RUN alpha "cat \"$FIX/alvo\""                 # rc=0 → bugfix pair
RUN beta  "ls \"$FIX\""
RUN beta  "wc -l \"$FIX/alvo\""
RUN gamma "echo x > \"$FIX/w\""

# 1. distill: format exact, counts typed (alpha is bugfix AND read-only —
#    both observations are emitted; only change beats discovery)
out=$(M 'distill')
printf '%s' "$out" | grep -q 'distill: 3 sessions, 4 observations (1 bugfix, 2 discovery, 1 change) from 5 chain lines' \
  && ok 'distill declares sessions/observations/chain lines' || no 'distill' "$out"

# 2. store persisted with C (card) and O (observation) lines
[ -f "$STORE" ] \
  && [ "$(grep -c '^C	' "$STORE")" = "3" ] \
  && [ "$(grep -c '^O	' "$STORE")" = "4" ] \
  && ok 'store: 3 cards + 4 observations' || no 'store' "$(cat "$STORE" 2>/dev/null)"

# 3. receipt invariant: bugfix observation cites both seqs (fail < success)
awk -F'\t' '$1=="O" && $3=="bugfix" { ok = ($5 ~ /^[0-9]+$/ && $6 ~ /^[0-9]+$/ && $5 < $6) } END { exit !ok }' "$STORE" \
  && ok 'bugfix observation cites failing and passing seqs' || no 'bugfix seqs' "$(cat "$STORE")"

# 4. distill attests itself on the chain
grep -q 'cmd=mem-distill: sessions=3 obs=4' "$CHAIN" \
  && ok 'mem-distill attested on the chain' || no 'attest' "$(tail -3 "$CHAIN")"

# 5. idempotency: no chain novelty → declared no-op, no second attest
out=$(M 'distill')
n=$(grep -c 'cmd=mem-distill' "$CHAIN")
printf '%s' "$out" | grep -q '— no change' && [ "$n" = "1" ] \
  && ok 're-distill: no change declared, no re-attest' || no 'idempotency' "$out attests=$n"

# 6. session split by >30min gap even with the SAME sess=
old=$(( $(date +%s) + 4000 ))
printf '42 %d.000000000 0000000000000000000000000000000000000000000000000000000000000000 tag=x rc=0 cwd=%s repo=- dur=1 sess=alpha cmd=cat later\n' "$old" "$FIX" >> "$CHAIN"
out=$(M 'distill')
printf '%s' "$out" | grep -q 'distill: 4 sessions,' \
  && ok 'gap >30min splits a session with the same sess=' || no 'gap split' "$out"

# 7. timeline: cards chronologically with typed observations
out=$(M 'timeline')
printf '%s' "$out" | grep -q 'sess=alpha' \
  && printf '%s' "$out" | grep -qE 'bugfix +seq [0-9]+→[0-9]+' \
  && printf '%s' "$out" | grep -qE 'change +seq [0-9]+→[0-9]+' \
  && ok 'timeline: cards + observations' || no 'timeline' "$out"

# 8. search: token hit, outcome filters
out=$(M 'search alvo')
printf '%s' "$out" | grep -q 'sess=alpha' \
  && printf '%s' "$out" | grep -q 'failed (rc=1) then succeeded' \
  && ok 'search finds card + observation by token' || no 'search' "$out"
out=$(M 'search cat --failed')
printf '%s' "$out" | grep -q 'bugfix' && ! printf '%s' "$out" | grep -q 'discovery' \
  && ok 'search --failed keeps only failure outcomes' || no 'search --failed' "$out"
out=$(M 'search session --type change')
printf '%s' "$out" | grep -q 'write-shaped' && ! printf '%s' "$out" | grep -q 'bugfix' \
  && ok 'search --type filters observation type' || no 'search --type' "$out"

# 9. brief: last card, outcome, hints (pull injection)
last=$(awk -F'\t' '$1=="C"{id=$2} END{print id}' "$STORE")
out=$(M 'brief')
printf '%s' "$out" | grep -q "last session $last" \
  && printf '%s' "$out" | grep -q 'read-only session: 1 reads' \
  && printf '%s' "$out" | grep -q "hint: mem search" \
  && ok 'brief: last session card + hint' || no 'brief' "$out"

# 10. verify: every cited seq replays → VERIFIED rc=0
first=$(awk -F'\t' '$1=="C"{print $2; exit}' "$STORE")
out=$(Mrc "verify $first"); rc=$?
[ $rc -eq 0 ] && printf '%s' "$out" | grep -q "VERIFIED $first — 6 claims replayed" \
  && ok 'verify: VERIFIED with claim count' || no 'verify' "rc=$rc [$out]"

# 11. tamper the store: one seq edited → DRIFT rc=1 citing the seq
cp "$STORE" "$STORE.bak"
awk -F'\t' 'BEGIN{OFS="\t"} $1=="O" && $3=="bugfix"{$6=99999} {print}' "$STORE.bak" > "$STORE"
out=$(Mrc "verify $first"); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q "DRIFT $first — seq 99999: missing from chain" \
  && ok 'tamper → DRIFT rc=1 citing the divergent seq' || no 'tamper' "rc=$rc [$out]"
mv "$STORE.bak" "$STORE"

# 12. unknown card id is honest rc=1
out=$(Merr 'verify deadbeefdeadbeef'); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q "mem: no card 'deadbeefdeadbeef'" \
  && ok 'verify unknown card: rc=1 honest' || no 'verify unknown' "rc=$rc [$out]"

# 13. --json on distill is stable
out=$(M 'distill --json')
printf '%s' "$out" | grep -q '"sessions":4,"observations":5' \
  && printf '%s' "$out" | grep -q '"chain_lines":6' \
  && ok 'distill --json: stable keys' || no 'distill json' "$out"

# 14. budget caps with the honest footer
out=$(M 'brief --budget 60')
printf '%s' "$out" | grep -q '\[mem\] output capped at 60 bytes — refine with --json' \
  && ok '--budget footer honest' || no 'budget' "$out"

# 15. no chain → honest error rc=1 (fresh HOME)
H2="$(mktemp -d)"; bash "$ROOT/tests/setup_sigil_modules.sh" "$H2"
out=$( (cd "$FIX" && HOME="$H2" "$J" -c 'mem distill' 2>&1 | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q 'mem: no chain for this repo' \
  && ok 'no chain: rc=1 + declared' || no 'no chain' "rc=$rc [$out]"

# 16. brief with no cards: honest hint, rc=0 (pull, never push)
out=$( (cd "$FIX" && HOME="$H2" "$J" -c 'mem brief' 2>/dev/null | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 0 ] && printf '%s' "$out" | grep -q "no memory yet for this repo — run 'mem distill'" \
  && ok 'brief without cards: honest hint rc=0' || no 'brief empty' "rc=$rc [$out]"

rm -rf "$H2"
unset HOME
rm -rf "$TH" "$FIX"
echo "== mem: $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
