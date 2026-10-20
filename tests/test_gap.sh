#!/usr/bin/env bash
# test_gap.sh — capability-gap radar: report (in-the-moment), dedup by
# signature (frequency = roadmap), observed misuse mining (rc=2 clusters),
# proof-closing verify (a fix is PROVEN, not declared), chain attestation.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)" || exit 2
trap 'rm -rf "$TH"' EXIT
export HOME="$TH" JASH_SIGIL=1
PROOF_PATH="$TH/gaptest-nope"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
G(){ "$J" -c "gap $1" 2>/dev/null | strip; }

echo "== gap radar: report, mine, verify =="

# report: quoted multi-word tried survives as ONE argument (argv_fn, ADR-0036 W2)
G '"jash_writef %+d unsupported" --expected "signed format verb"' >/dev/null
out=$(G 'list --json')
printf '%s' "$out" | grep -q '"tried":"jash_writef %+d unsupported"' \
  && ok 'quoted tried preserved whole' || no 'quoted tried' "$out"
printf '%s' "$out" | grep -q '"expected":"signed format verb"' \
  && ok 'expected recorded' || no 'expected' "$out"
printf '%s' "$out" | grep -q '"status":"open","count":1' \
  && ok 'open with count 1' || no 'count1' "$out"

# dedup: same signature → count++, not a new entry
G '"jash_writef %+d unsupported" --expected "signed format verb"' >/dev/null
out=$(G 'list --json')
[ "$(printf '%s' "$out" | grep -o '"sig"' | wc -l | tr -d ' ')" = "1" ] \
  && printf '%s' "$out" | grep -q '"count":2' \
  && ok 'dedup by signature: hit #2' || no 'dedup' "$out"

# a DIFFERENT gap is a second entry
G '"mesh frobnicate" --expected "subcommand exists"' >/dev/null
out=$(G 'list --json')
[ "$(printf '%s' "$out" | grep -o '"sig"' | wc -l | tr -d ' ')" = "2" ] \
  && ok 'distinct gaps listed separately' || no 'distinct' "$out"

# demand-weighted ordering: most-hit first
out=$(G 'list')
first=$(printf '%s' "$out" | grep -m1 '[0-9]x \[' )
printf '%s' "$first" | grep -q '2x \[' && ok 'queue sorted by frequency' || no 'freq sort' "$first"

# proof-attached verify: failing proof keeps it open, rc=1 (pipeline gate)
G "\"artifact exists\" --prove \"file $PROOF_PATH\"" >/dev/null
"$J" -c 'gap verify' >/dev/null 2>&1; rc=$?
[ $rc -eq 1 ] && ok 'verify rc=1 while a proof fails (gate)' || no 'verify gate' "rc=$rc"
out=$(G 'list --json')
printf '%s' "$out" | grep -Fq "\"tried\":\"artifact exists\",\"expected\":\"\",\"prove\":\"file $PROOF_PATH\"" \
  && ok 'prove field stored verbatim' || no 'prove stored' "$out"
printf '%s' "$out" | grep -q '"status":"open"' \
  && ok 'failed proof stays open' || no 'stays open' "$out"

# fix the world → verify closes ONLY the proven one
: > "$PROOF_PATH"
out=$(G 'verify'); rc=$?
[ $rc -eq 0 ] && printf '%s' "$out" | grep -q '1 proven fixed' \
  && ok 'verify closes the proven gap, rc=0' || no 'verify close' "rc=$rc [$out]"
out=$(G 'list --all --json')
printf '%s' "$out" | grep -q '"status":"fixed","count":1[^}]*"tried":"artifact exists"' \
  && ok 'fixed status materialized' || no 'fixed status' "$out"
out=$(G 'list --json')
printf '%s' "$out" | grep -q 'artifact exists' \
  && no 'default list hides fixed' "$out" || ok 'default list hides fixed'

# chain attestation: gap + gap-fixed are recall-visible
C=$(ls "$TH"/.jash/chain*.log 2>/dev/null | head -1)
[ -n "$C" ] && grep -q 'cmd=gap: sig=.*tried=jash_writef' "$C" \
  && ok 'report attested in chain' || no 'attest gap' "$(grep -c 'cmd=gap' "$C" 2>/dev/null)"
[ -n "$C" ] && grep -q 'cmd=gap-fixed: sig=' "$C" \
  && ok 'auto-close attested in chain' || no 'attest fixed' ""

# re-reporting a "fixed" gap reopens it (the fix didn't hold)
G "\"artifact exists\" --prove \"file $PROOF_PATH\"" >/dev/null
out=$(G 'list --json')
printf '%s' "$out" | grep -q '"status":"open","count":[0-9]*,"fleet":[0-9]*,"last_ts":[0-9]*,"tried":"artifact exists"' \
  && ok 're-report reopens a fixed gap' || no 'reopen' "$out"
rm -f "$PROOF_PATH"

# nudge: rc=2 (misuse) is the wall moment — jash suggests `gap` right there
tip=$("$J" -c 'expect' 2>&1 >/dev/null | strip)
printf '%s' "$tip" | grep -q 'gap "' \
  && ok 'rc=2 misuse nudges a gap report' || no 'misuse nudge' "$tip"
tip=$(JASH_NUDGE=0 "$J" -c 'expect' 2>&1 >/dev/null | strip)
printf '%s' "$tip" | grep -q 'gap "' \
  && no 'nudge kill switch' "$tip" || ok 'JASH_NUDGE=0 silences the gap nudge'

# scan: mines rc=2 misuse from the chain (observed, honestly labeled)
"$J" -c 'expect' >/dev/null 2>&1   # rc=2 misuse, chained
"$J" -c 'expect' >/dev/null 2>&1
out=$(G 'scan --json')
printf '%s' "$out" | grep -q '"misuse":"expect"[^}]*"count":[0-9]' \
  && ok 'scan clusters observed rc=2 misuse' || no 'scan' "$out"

# manual close is honest about being unverified
sig=$(G 'list --json' | grep -m1 -o '"sig":"[0-9a-f]*","status":"open","count":2' | grep -o '[0-9a-f]\{16\}')
out=$(G "close $sig")
printf '%s' "$out" | grep -q 'honor close' && ok 'manual close labeled unverified' || no 'close' "$out"

# fleet demand: absent while mesh inactive, aggregated once the mesh dir exists
[ ! -e "$TH/.jash/mesh/gaps.tsv" ] \
  && ok 'fleet file absent while mesh inactive' || no 'fleet inactive' ""
mkdir -p "$TH/.jash/mesh"
G '"swarm-wide itch"' >/dev/null
G '"swarm-wide itch"' >/dev/null
[ -f "$TH/.jash/mesh/gaps.tsv" ] \
  && ok 'fleet file created once mesh active' || no 'fleet file' ""
hits=$(awk -F'\t' '$2==2{c++} END{print c+0}' "$TH/.jash/mesh/gaps.tsv")
[ "$hits" = "1" ] && ok 'fleet counter aggregates hits' || no 'fleet hits' "$(cat "$TH/.jash/mesh/gaps.tsv")"

# another machine's pain: fleet > local surfaces in the queue
flt="$TH/.jash/mesh/gaps.tsv"
awk -F'\t' 'BEGIN{OFS="\t"}{$2=7;print}' "$flt" > "$flt.tmp" && mv "$flt.tmp" "$flt"
out=$(G 'list --json')
printf '%s' "$out" | grep -q '"fleet":7' \
  && ok 'fleet hits surface in json' || no 'fleet json' "$out"
out=$(G 'list')
printf '%s' "$out" | grep -q 'fleet 7x' \
  && ok 'fleet hits surface in text queue' || no 'fleet text' "$out"

# doctor surfaces the open-gap backlog (count derived, not hardcoded)
exp=$(G 'list --json' | grep -o '"status":"open"' | wc -l | tr -d ' ')
out=$("$J" -c 'doctor' 2>/dev/null | strip)
printf '%s' "$out" | grep -qE "gaps +$exp open" \
  && ok 'doctor surfaces open gaps' || no 'doctor gaps' "$out"

# no-args help
printf '%s' "$(G '')" | grep -q 'capability-gap radar' && ok 'help on no args' || no 'help' ""

unset HOME
rm -rf "$TH"
echo "== gap: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
