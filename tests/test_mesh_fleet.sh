#!/usr/bin/env bash
# test_mesh_fleet.sh — mesh mercado-cego features:
#   lease/heartbeat (renews task claims), dead-owner immediate reclaim,
#   owner-only close (--force override), capability routing from chain
#   attest, results feeding recall (chain.log), mesh sweep (dead-agent
#   detection via pid liveness — harness sets JASH_AGENT_PID).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; mkdir -p "$TH/.jash"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
# NB: no EXIT trap — bash runs EXIT traps in subshells too (cmdsubst/bg jobs),
# which would delete $TH mid-test. Explicit cleanup at the end (project style).

# a LONG-LIVED pid (the "harness") and a DEAD pid (a harness that died)
sleep 300 & LP=$!
sleep 300 & DP=$!; kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null

# M ID PID CMD — run one mesh command as agent ID owned by harness pid PID
M(){ local id="$1" pid="$2"; shift 2
     env -i HOME="$TH" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
         JASH_AGENT_ID="$id" ${pid:+JASH_AGENT_PID=$pid} \
         "$J" -c "mesh $1" 2>/dev/null | strip; }
ME(){ local id="$1" pid="$2"; shift 2   # same, stderr kept, rc preserved
     env -i HOME="$TH" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
         JASH_AGENT_ID="$id" ${pid:+JASH_AGENT_PID=$pid} \
         "$J" -c "mesh $1"; }

echo "== mesh fleet: lease, death, routing, recall (mercado-cego) =="

# ── fleet liveness via JASH_AGENT_PID ────────────────────────────────────────
M w1 "$LP" 'register --provider anthropic --model opus --id w1' >/dev/null
M w9 "$DP" 'register --provider x --model y --id w9' >/dev/null
out=$(M w1 "$LP" 'workers --json')
printf '%s' "$out" | grep -q '"agent_id":"w1".*"alive":true'  && ok 'live harness pid → alive:true'  || no 'alive true'  "$out"
printf '%s' "$out" | grep -q '"agent_id":"w9".*"alive":false' && ok 'dead harness pid → alive:false' || no 'alive false' "$out"
M wz "" 'register --provider p --model m --id wz' >/dev/null
out=$(M wz "" 'workers --json')
printf '%s' "$out" | grep -q '"agent_id":"wz".*"alive":null'  && ok 'no JASH_AGENT_PID → alive:null (honest unknown)' || no 'alive null' "$out"

# ── lease/heartbeat: live agent's task is NOT reclaimed past TTL ─────────────
M w1 "$LP" 'init "lease test"' >/dev/null
M w1 "$LP" 'add "long running task" --tag slow' >/dev/null
M w1 "$LP" 'next --id w1 --json' >/dev/null
# backdate the claim 700s into the past (past the 600s TTL)
tf="$TH/.jash/mesh/tasks/1.tsv"
now=$(date +%s)
awk -F'\t' -v OFS='\t' -v ts="$((now-700))" '{$4=ts; print}' "$tf" > "$tf.b" && mv "$tf.b" "$tf"
M w1 "$LP" 'heartbeat --id w1' >/dev/null    # lease renewal sweeps owned tasks
out=$(M w2 "$LP" 'next --id w2 --json')
printf '%s' "$out" | grep -q '^null' && ok 'heartbeat renewed lease: not reclaimed past TTL' || no 'lease renew' "$out"
# without a heartbeat, a backdated claim IS reclaimable by TTL
awk -F'\t' -v OFS='\t' -v ts="$((now-700))" '{$4=ts; print}' "$tf" > "$tf.b" && mv "$tf.b" "$tf"
out=$(M w2 "$LP" 'next --id w2 --json')
printf '%s' "$out" | grep -q '"id":1' && ok 'silent owner past TTL: task reclaimed' || no 'TTL reclaim' "$out"
M w2 "$LP" 'done 1 --id w2' >/dev/null

# ── dead owner: immediate reclaim (no TTL wait) ──────────────────────────────
M w1 "$LP" 'add "orphan task" --tag x' >/dev/null
M w9 "$DP" 'next --id w9 --json' >/dev/null    # w9's harness pid is already dead
out=$(M w1 "$LP" 'next --id w1 --json')
printf '%s' "$out" | grep -q '"id":2' && ok 'dead owner: task reclaimed immediately' || no 'dead reclaim' "$out"
M w1 "$LP" 'done 2 --id w1' >/dev/null

# ── owner-only close + --force ───────────────────────────────────────────────
M w1 "$LP" 'add "guarded task" --tag g' >/dev/null
M w1 "$LP" 'next --id w1 --json' >/dev/null    # #3 claimed by w1
err=$(ME w2 "$LP" 'done 3 --id w2' 2>&1 >/dev/null); rc=$?
{ [ $rc -ne 0 ] && printf '%s' "$err" | grep -q -- '--force'; } && ok 'non-owner done refused with hint' || no 'owner check' "rc=$rc [$err]"
st=$(M w1 "$LP" 'board --json')
printf '%s' "$st" | grep -q '"id":3,"state":"claimed"' && ok 'refused close left task claimed' || no 'still claimed' "$st"
err=$(ME w2 "$LP" 'done 3 --id w2 --force' 2>&1 >/dev/null); rc=$?
{ [ $rc -eq 0 ] && printf '%s' "$err" | grep -q 'WARNING'; } && ok '--force closes but is LOUD' || no 'force close' "rc=$rc [$err]"

# ── capability routing: chain attest steers next ─────────────────────────────
M w1 "$LP" 'init "routing"' >/dev/null
M w1 "$LP" 'add "mech warm-up" --tag mechanical' >/dev/null
M w1 "$LP" 'next --id w1 --json' >/dev/null
M w1 "$LP" 'done 1 --id w1' >/dev/null          # opus proves mechanical
M w1 "$LP" 'add "verify thing" --tag verify' >/dev/null
M w1 "$LP" 'add "mech thing" --tag mechanical' >/dev/null
out=$(M w1 "$LP" 'next --id w1 --json')          # #2 verify (score 0) vs #3 mechanical (score +1)
printf '%s' "$out" | grep -q '"id":3' && ok 'routing: proven tag beats lower id' || no 'routing' "$out"
printf '%s' "$out" | grep -q '"cap_score":1' && ok 'cap_score exposed in JSON' || no 'cap_score' "$out"
M w1 "$LP" 'fail 3 --id w1' >/dev/null           # mechanical now 0 (1 done, 1 fail)
out=$(M w1 "$LP" 'next --id w1 --json')
printf '%s' "$out" | grep -q '"id":2' && ok 'fail erases the advantage (FIFO again)' || no 'routing fail' "$out"
M w1 "$LP" 'done 2 --id w1' >/dev/null

# ── W2: reputation table from the same attested history ──────────────────────
out=$(M w1 "$LP" 'rep --json')
printf '%s' "$out" | grep -q '"model":"opus","tag":"mechanical","done":1,"fail":1' \
  && ok 'W2: rep aggregates attested model×tag history' || no 'rep json' "$out"
out=$(M w1 "$LP" 'rep')
printf '%s' "$out" | grep -q 'MESH REPUTATION' && printf '%s' "$out" | grep -q 'opus' \
  && ok 'W2: human rep table renders' || no 'rep human' "$out"

# ── results feed recall (chain.log attest) ───────────────────────────────────
# chain is per-repo inside a repo (ADR-0051 W6): match whichever chain file got it
chain=$(ls "$TH"/.jash/chain*.log 2>/dev/null | head -1)
[ -n "$chain" ] && grep -q 'cmd=mesh-done: task=1 .*tag=mechanical .*model=opus' "$chain" && ok 'done attested in chain (recall-visible)' || no 'attest done' "$(tail -3 "$chain" 2>/dev/null)"
[ -n "$chain" ] && grep -q 'cmd=mesh-fail: task=3 ' "$chain" && ok 'fail attested in chain' || no 'attest fail' "$(tail -3 "$chain" 2>/dev/null)"

# ── mesh sweep: dead-agent cleanup ───────────────────────────────────────────
M w1 "$LP" 'init "sweep"' >/dev/null
M w1 "$LP" 'add "sweepable" --tag s' >/dev/null
M w9 "$DP" 'next --id w9 --json' >/dev/null       # dead-harness worker claims #1
f="$TH/claimed.c"; : > "$f"
M w9 "$DP" "claim $f --ttl 900" >/dev/null
out=$(M w1 "$LP" 'sweep --json')
printf '%s' "$out" | grep -q '"dead":1' && printf '%s' "$out" | grep -q '"reclaimed":1' \
  && ok 'sweep: dead agent swept, task reclaimed' || no 'sweep' "$out"
st=$(M w1 "$LP" 'board --json')
printf '%s' "$st" | grep -q '"id":1,"state":"ready"' && ok 'swept task back to ready' || no 'swept ready' "$st"
cl=$(M w1 "$LP" 'claims --json')
[ "$cl" = "[]" ] && ok 'sweep dropped dead agent file claims' || no 'sweep claims' "$cl"
w9st=$(M w1 "$LP" 'workers --json')
printf '%s' "$w9st" | grep -q '"agent_id":"w9","provider":"x","model":"y","status":"dead"' && ok 'swept worker marked status=dead' || no 'status dead' "$w9st"

# ── W3: dead-agent autopsy attested in the chain ─────────────────────────────
chain=$(ls "$TH"/.jash/chain*.log 2>/dev/null | head -1)
[ -n "$chain" ] && grep -q 'cmd=mesh-dead: agent=w9 task=1 silent=[0-9]*s reclaimed=1' "$chain" \
  && ok 'W3: sweep autopsy attested (recall can explain the stall)' || no 'mesh-dead attest' "$(grep -c mesh-dead "$chain" 2>/dev/null)"

# ── W1: done only closes with evidence (--prove) ─────────────────────────────
M w1 "$LP" 'init "prove"' >/dev/null
M w1 "$LP" 'add "produce the artifact" --tag build' >/dev/null
M w1 "$LP" 'next --id w1 --json' >/dev/null     # #1 claimed by w1
pf="$TH/artifact.c"
err=$(ME w1 "$LP" "done 1 --id w1 --prove \"file $pf\"" 2>&1 >/dev/null); rc=$?
{ [ $rc -ne 0 ] && printf '%s' "$err" | grep -q 'proof failed'; } \
  && ok 'W1: unproven done refused (loud)' || no 'prove refuse' "rc=$rc [$err]"
st=$(M w1 "$LP" 'board --json')
printf '%s' "$st" | grep -q '"id":1,"state":"claimed"' \
  && ok 'W1: refused proof leaves task claimed' || no 'prove still claimed' "$st"
echo 'int main(void){return 0;}' > "$pf"
out=$(M w1 "$LP" "done 1 --id w1 --prove \"file $pf\"")
printf '%s' "$out" | grep -q '→ done (proven)' && ok 'W1: proven done closes' || no 'prove close' "$out"
[ -n "$chain" ] && grep -q 'cmd=mesh-unproven: task=1 tag=build' "$chain" \
  && ok 'W1: unproven attempt attested in chain' || no 'unproven attest' ""

# ── crash-safety: no .tmp debris left by atomic writes ───────────────────────
tmpn=$(find "$TH/.jash/mesh" -name '*.tmp.*' | wc -l | tr -d ' ')
[ "$tmpn" = "0" ] && ok 'atomic writes leave no .tmp debris' || no 'tmp debris' "$tmpn files"

kill "$LP" 2>/dev/null
rm -rf "$TH"
echo "== mesh fleet: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
