#!/usr/bin/env bash
# test_dashboard.sh — `dashboard`: the "measure" layer in one view. Aggregates,
# from the attested chain + mesh + state, agent usage: adoption %, activity,
# fleet/board, safety. Human panel + --json for BI.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; export HOME="$TH"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
export JASH_SIGIL=1
for c in "/bin/echo a" "/bin/ls /nope" "recall failed" "expect file /etc/hosts" "checkpoint base"; do
  "$J" -c "$c" >/dev/null 2>&1; done
"$J" -c "mesh register --provider anthropic --model opus-4.8 --id w1" >/dev/null 2>&1
"$J" -c 'digest --brief --no-cache' >/dev/null 2>&1

echo "== dashboard (measure layer) =="
hj="$("$J" -c 'dashboard --json' 2>/dev/null | strip)"
for k in '"adoption"' '"pct":' '"commands":' '"failed":' '"fleet":' '"board":' '"checkpoints":' '"recording"'; do
  printf '%s' "$hj" | grep -q "$k" && ok "json has $k" || no "json $k" "$hj"
done
printf '%s' "$hj" | grep -q '"recall":' && ok 'json tallies differentiator usage' || no 'json features' "$hj"
printf '%s' "$hj" | grep -q '"digest":1' && ok 'json counts native digest in current repo' || no 'digest adoption' "$hj"

out="$("$J" -c 'dashboard' 2>/dev/null | strip)"
printf '%s' "$out" | grep -q 'ADOPTION' && ok 'human panel: ADOPTION section' || no 'ADOPTION' "$out"
printf '%s' "$out" | grep -q 'ACTIVITY' && ok 'human panel: ACTIVITY section' || no 'ACTIVITY' "$out"
printf '%s' "$out" | grep -q 'MESH'     && ok 'human panel: MESH (fleet present)' || no 'MESH' "$out"
printf '%s' "$out" | grep -q 'SAFETY'   && ok 'human panel: SAFETY section' || no 'SAFETY' "$out"

# recording-off → tells you to enable it (metrics need the chain)
off="$(JASH_SIGIL=0 "$J" -c 'dashboard' 2>/dev/null | strip)"
printf '%s' "$off" | grep -qi 'recording OFF' && ok 'flags recording OFF' || no 'rec-off' "$off"

rm -rf "$TH"
echo "== dashboard: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
