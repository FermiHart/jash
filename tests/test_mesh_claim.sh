#!/usr/bin/env bash
# tests/test_mesh_claim.sh — ADR-0042 F9: distributed file intent-locks.
#
# A claim records "agent OWNER intends to work on PATH for TTL seconds". Before a
# mutating op (rm/mv) on a path a *live* OTHER agent has claimed, the mutation
# guard warns (advisory) or blocks (JASH_CLAIM_STRICT=1) and records the conflict
# in the JASH_AGENT envelope. Claims are heartbeat-refreshed; a claim whose owner
# goes silent past its TTL is auto-reclaimed. Own claims never conflict.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"
ERRF="$T/err"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# jrun OWNER CMD… → runs as agent OWNER, stderr captured to $ERRF, returns rc.
# Callers may pre-set STRICT=1 / AGENT=1 to add the matching env var.
STRICT="" ; AGENT=""
jrun() {
  local owner="$1"; shift
  env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_AGENT_ID="$owner" \
      ${STRICT:+JASH_CLAIM_STRICT=1} ${AGENT:+JASH_AGENT=1} \
      "$JASH" -c "$*" 2>"$ERRF"
}
warned()  { grep -q 'mesh-claim' "$ERRF"; }
blocked() { grep -q 'BLOCKED' "$ERRF"; }

# ── 1. A claims; B's rm warns (advisory) and proceeds ────────────────────────
f="$T/c1.c"; : > "$f"
jrun w1 "mesh claim $f --ttl 900 --note refactoring" >/dev/null
jrun w2 "rm $f" >/dev/null; rc=$?
if warned && [[ $rc -eq 0 && ! -e "$f" ]]; then
  ok "advisory: foreign rm warns and still proceeds"
else
  err "advisory (warned=$(warned&&echo y||echo n) rc=$rc exists=$([[ -e $f ]]&&echo y||echo n))"
fi

# ── 2. strict blocks (rc≠0, file remains) ────────────────────────────────────
f="$T/c2.c"; : > "$f"
jrun w1 "mesh claim $f" >/dev/null
STRICT=1 jrun w2 "rm $f" >/dev/null; rc=$?; STRICT=""
if blocked && [[ $rc -ne 0 && -e "$f" ]]; then
  ok "strict: foreign rm blocked, file preserved (rc=$rc)"
else
  err "strict (blocked=$(blocked&&echo y||echo n) rc=$rc exists=$([[ -e $f ]]&&echo y||echo n))"
fi

# ── 3. TTL expiry auto-reclaims (no warning after expiry) ────────────────────
f="$T/c3.c"; : > "$f"
jrun w1 "mesh claim $f --ttl 1" >/dev/null
sleep 2
jrun w2 "rm $f" >/dev/null; rc=$?
if ! warned && [[ $rc -eq 0 ]]; then
  ok "TTL expiry: stale claim no longer conflicts"
else
  err "TTL expiry (warned=$(warned&&echo y||echo n) rc=$rc)"
fi

# ── 4. unclaim releases ──────────────────────────────────────────────────────
f="$T/c4.c"; : > "$f"
jrun w1 "mesh claim $f" >/dev/null
jrun w1 "mesh unclaim $f" >/dev/null
jrun w2 "rm $f" >/dev/null
if ! warned; then ok "unclaim: released claim no longer conflicts"
else err "unclaim still conflicts"; fi

# ── 5. heartbeat refresh prevents reclaim ────────────────────────────────────
f="$T/c5.c"; : > "$f"
jrun w1 "mesh claim $f --ttl 3" >/dev/null
sleep 2
jrun w1 "mesh heartbeat --id w1" >/dev/null   # bumps claim_ts
sleep 2                                        # 4s total > original 3s ttl
jrun w2 "rm $f" >/dev/null
if warned && [[ -e "$f" || ! -e "$f" ]]; then
  # (advisory: rm still proceeds; what matters is the claim was still live → warned)
  ok "heartbeat refresh: claim stayed live past original TTL"
else
  err "heartbeat refresh (warned=$(warned&&echo y||echo n))"
fi

# ── 6. dead session (no heartbeat > TTL) doesn't block ───────────────────────
f="$T/c6.c"; : > "$f"
jrun w1 "mesh claim $f --ttl 1" >/dev/null
sleep 2                                        # w1 never heartbeats → dead
STRICT=1 jrun w2 "rm $f" >/dev/null; rc=$?; STRICT=""
if ! blocked && [[ $rc -eq 0 && ! -e "$f" ]]; then
  ok "dead session: claim reclaimed, even strict rm proceeds"
else
  err "dead session (blocked=$(blocked&&echo y||echo n) rc=$rc)"
fi

# ── 7. self-claim never conflicts (own intent-lock) ──────────────────────────
f="$T/c7.c"; : > "$f"
jrun w1 "mesh claim $f" >/dev/null
jrun w1 "rm $f" >/dev/null
if ! warned; then ok "self-claim: owner's own rm is not warned"
else err "self-claim wrongly warned"; fi

# ── 8. JASH_AGENT envelope carries claim_conflicts (valid JSON) ──────────────
f="$T/c8.c"; : > "$f"
jrun w1 "mesh claim $f" >/dev/null
AGENT=1 jrun w2 "rm $f" >/dev/null; AGENT=""
line=$(grep '"jash_agent":1' "$ERRF" | head -1)
case "$line" in
  *'"claim_conflicts":['*"$f"*) ok "envelope carries claim_conflicts with the path";;
  *) err "envelope claim_conflicts (got '$line')";;
esac
if command -v python3 >/dev/null 2>&1; then
  if printf '%s' "$line" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert isinstance(d.get("claim_conflicts"),list) and len(d["claim_conflicts"])>=1' 2>/dev/null; then
    ok "envelope is valid JSON with a non-empty claim_conflicts list"
  else
    err "envelope failed JSON parse / claim_conflicts check"
  fi
else
  echo "· (python3 absent — skipping JSON-parse sub-check)"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  mesh claim (ADR-0042 F9) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ mesh-claim tests PASSED"; exit 0; } \
                  || { echo "  ✗ mesh-claim tests FAILED"; exit 1; }
