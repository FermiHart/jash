#!/usr/bin/env bash
# tests/test_memo_delta.sh — ADR-0042 F1: structural output diff (memoΔ).
#
# When a whitelisted read-only command is re-run and its output changed only
# slightly, jash emits just the +N/-M changed lines (with the actual line text)
# instead of re-paying the full body — while preserving rc exactly. A too-large
# diff falls back to the full body; JASH_MEMO_DELTA=0 restores the ADR-0038 W1
# binary-memo behavior (change ⇒ full reprint); the JASH_AGENT envelope carries
# a "memo":{"delta":{...}} object with the counts.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# Base agent-session env (memo active). Extra VAR=val pairs may be prepended by
# passing them before the sid — see run_env.
run_env() { # extra_env... -- sid cmd...
  local extra=()
  while [[ "$1" != "--" ]]; do extra+=("$1"); shift; done
  shift                       # drop the --
  local sid="$1"; shift
  env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$sid" \
      JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      ${extra[@]+"${extra[@]}"} \
      "$JASH" -c "$*" 2>/dev/null
}
run() { run_env -- "$@"; }     # plain memo env (delta default-on)

# ── 1. one changed line ⇒ +/- pair only (not the unchanged bulk), rc identical ─
printf 'alpha\nbeta\ngamma\ndelta\n' > "$T/f.txt"
run s1 "cat $T/f.txt" >/dev/null; rc1=$?
sleep 0.01
printf 'alpha\nBETAX\ngamma\ndelta\n' > "$T/f.txt"
out=$(run s1 "cat $T/f.txt"); rc2=$?
case "$out" in
  *"[jash memoΔ]"*"+1/-1"*) ok "memoΔ header shows +1/-1";;
  *) err "memoΔ header (got '$out')";;
esac
case "$out" in
  *BETAX*) ok "changed line (BETAX) shown as an addition";;
  *) err "added line missing (got '$out')";;
esac
case "$out" in
  *alpha*|*gamma*) err "unchanged bulk leaked into delta (got '$out')";;
  *) ok "unchanged lines (alpha/gamma) suppressed";;
esac
[[ $rc1 -eq $rc2 ]] && ok "rc preserved across delta ($rc1==$rc2)" \
                    || err "rc changed ($rc1 -> $rc2)"

# ── 2. identical re-run ⇒ ADR-0038 one-line [jash memo] (no delta, no regression)
printf 'x\ny\nz\n' > "$T/g.txt"
run s2 "cat $T/g.txt" >/dev/null
out=$(run s2 "cat $T/g.txt")
case "$out" in
  *"[jash memo] unchanged"*) ok "identical re-run still binary-memoized";;
  *"memoΔ"*) err "identical re-run wrongly emitted a delta ('$out')";;
  *) err "identical re-run (got '$out')";;
esac

# ── 3. over-cap diff ⇒ full body + too-large note ─────────────────────────────
seq 1 1500 > "$T/big.txt"
run s3 "cat $T/big.txt" >/dev/null
sleep 0.01
seq 2 1501 > "$T/big.txt"
out=$(run s3 "cat $T/big.txt")
case "$out" in
  *"diff too large"*) ok "over-cap emits the too-large note";;
  *) err "over-cap note missing";;
esac
case "$out" in
  *"1501"*) ok "over-cap streams the full new body";;
  *) err "over-cap did not stream full body";;
esac

# ── 4. JASH_MEMO_DELTA=0 ⇒ byte-parity with binary memo (change ⇒ full reprint) ─
printf '1\n2\n3\n' > "$T/h.txt"
run_env JASH_MEMO_DELTA=0 -- s4 "cat $T/h.txt" >/dev/null
sleep 0.01
printf '1\n2\nTHREE\n' > "$T/h.txt"
out=$(run_env JASH_MEMO_DELTA=0 -- s4 "cat $T/h.txt")
if [[ "$out" == "$(printf '1\n2\nTHREE')" ]]; then
  ok "JASH_MEMO_DELTA=0 reprints the full body verbatim"
else
  err "JASH_MEMO_DELTA=0 parity (got '$out')"
fi
case "$out" in
  *"memoΔ"*) err "JASH_MEMO_DELTA=0 still emitted a delta";;
  *) ok "JASH_MEMO_DELTA=0 emits no delta marker";;
esac

# ── 5. JASH_AGENT=1 envelope carries memo.delta with correct counts (valid JSON)
# The envelope is on stderr, so bypass run_env (which swallows fd2) and drive
# env directly, redirecting stdout to /dev/null and stderr to a file.
agent_run() { # sid cmd... → stderr into $T/env.json
  local sid="$1"; shift
  env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$sid" \
      JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_AGENT=1 \
      "$JASH" -c "$*" >/dev/null 2>"$T/env.json"
}
printf 'p\nq\nr\n' > "$T/e.txt"
agent_run s5 "cat $T/e.txt"
sleep 0.01
printf 'p\nQ\nr\n' > "$T/e.txt"
agent_run s5 "cat $T/e.txt"
line=$(grep '"jash_agent":1' "$T/env.json" | head -1)
case "$line" in
  *'"memo":{"delta":{"add":1,"del":1'*) ok "envelope carries memo.delta add:1/del:1";;
  *) err "envelope memo.delta (got '$line')";;
esac
if command -v python3 >/dev/null 2>&1; then
  if printf '%s' "$line" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["memo"]["delta"]["add"]==1 and d["memo"]["delta"]["del"]==1' 2>/dev/null; then
    ok "envelope is valid JSON with correct memo.delta"
  else
    err "envelope failed JSON parse / count check"
  fi
else
  echo "· (python3 absent — skipping JSON-parse sub-check)"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  memoΔ (ADR-0042 F1) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ memoΔ tests PASSED"; exit 0; } \
                  || { echo "  ✗ memoΔ tests FAILED"; exit 1; }
