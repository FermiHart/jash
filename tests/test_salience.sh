#!/usr/bin/env bash
# tests/test_salience.sh — ADR-0042 F4: salient output governor.
#
# When JASH_GOVERNOR=salient and a command's output overflows JASH_LLM_BUDGET,
# jash RANKS lines (error/novel/rare/edge) and shows the top selection in
# original order + a pattern histogram — instead of position (tail) capping.
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

# base env; extra VAR=val may prefix. MEMO=0 keeps a parity-style harness clean.
run() { env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
            JASH_SIGIL=1 JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 "$@"; }

# This test exercises the SALIENT governor (rank lines, keep the FATAL). That
# path is external-only (c/parser/exec.c → jash_budget_flush). ADR-0044 gives
# builtins output governing too, but via the streaming head-cap+spill wrap
# (jash_budget_wrap) — NOT salient ranking — so a builtin cat would head-cap and
# drop the mid-output FATAL. Use /bin/cat (external) to hit the salient path.
CAT=/bin/cat

# 10k lines, a FATAL at line 5000, everything else a single repeated pattern.
BIG="$T/big.txt"
awk 'BEGIN{for(i=1;i<=10000;i++){if(i==5000)print "FATAL: disk corruption at sector 42";else print "processing item " i " ok"}}' > "$BIG"
FULL_BYTES=$(wc -c < "$BIG" | tr -d ' ')

# ── 1. salient keeps the decisive middle line; tail drops it ─────────────────
sal=$(run JASH_LLM_BUDGET=3000 JASH_GOVERNOR=salient "$JASH" -c "$CAT $BIG" 2>&1)
tai=$(run JASH_LLM_BUDGET=3000 JASH_GOVERNOR=tail    "$JASH" -c "$CAT $BIG" 2>&1)
if echo "$sal" | grep -q "FATAL"; then ok "salient keeps the mid-output FATAL"
else err "salient dropped the FATAL"; fi
if echo "$tai" | grep -q "FATAL"; then err "tail unexpectedly kept the mid FATAL"
else ok "tail (position cap) drops the mid FATAL"; fi

# ── 2. repeated pattern collapses → ≥80% byte savings + histogram footer ─────
sal_bytes=$(printf '%s' "$sal" | wc -c | tr -d ' ')
# savings ≥ 80% ⇔ shown bytes < 20% of full
if [[ $((sal_bytes * 5)) -lt $FULL_BYTES ]]; then
  ok "byte savings ≥ 80% (shown ${sal_bytes} vs full ${FULL_BYTES})"
else
  err "insufficient savings (shown ${sal_bytes} vs full ${FULL_BYTES})"
fi
echo "$sal" | grep -q "governor:salient" && ok "histogram footer present" \
                                         || err "footer missing"

# ── 3. novel line on 2nd run is ranked into the selection ────────────────────
awk 'BEGIN{for(i=1;i<=5000;i++)print "routine log line " i}' > "$T/a.txt"
run JASH_LLM_BUDGET=2000 JASH_GOVERNOR=salient "$JASH" -c "$CAT $T/a.txt" >/dev/null 2>&1
awk 'BEGIN{for(i=1;i<=5000;i++){print "routine log line " i; if(i==2500)print "NOVEL-UNIQUE-EVENT-XYZ"}}' > "$T/b.txt"
nov=$(run JASH_LLM_BUDGET=2000 JASH_GOVERNOR=salient "$JASH" -c "$CAT $T/b.txt" 2>&1)
echo "$nov" | grep -q "NOVEL-UNIQUE-EVENT-XYZ" \
    && ok "novel line ranked above seen lines (2nd run)" \
    || err "novel line not selected"

# ── 4. under-budget output passes through untouched (no salience tax) ─────────
printf 'alpha\nbeta\ngamma\n' > "$T/small.txt"
u=$(run JASH_LLM_BUDGET=100000 JASH_GOVERNOR=salient "$JASH" -c "$CAT $T/small.txt" 2>&1)
[[ "$u" == "$(printf 'alpha\nbeta\ngamma')" ]] \
    && ok "under-budget: verbatim, no salience footer" \
    || err "under-budget altered ('$u')"

# ── 5. tail mode = byte-parity with the default (no governor) ────────────────
# The capped content must be identical; the only per-run difference is the PID
# inside the [context-governor] recall handle, which is pre-existing noise.
d=$(run JASH_LLM_BUDGET=3000 "$JASH" -c "$CAT $BIG" 2>&1)
d_c=$(printf '%s' "$d"   | grep -v 'context-governor')
tai_c=$(printf '%s' "$tai" | grep -v 'context-governor')
if [[ "$d_c" == "$tai_c" ]] && ! echo "$tai" | grep -q "governor:salient"; then
  ok "tail mode content is byte-identical to the default governor"
else
  err "tail mode diverged from default"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  salient governor (ADR-0042 F4) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ salience tests PASSED"; exit 0; } \
                  || { echo "  ✗ salience tests FAILED"; exit 1; }
