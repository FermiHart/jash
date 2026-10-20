#!/usr/bin/env bash
# tests/test_builtin_governor.sh — ADR-0044: output-budget governing for builtins.
#
# A single top-level builtin writing to a NON-tty sink with JASH_LLM_BUDGET set is
# capped (shown bytes bounded, the rest spilled to ~/.jash/overflow, a recall-able
# footer emitted) — the same protection external commands get, so a builtin like
# `cat bigfile` / `ls node_modules` never floods the agent window. Crucially this
# must NOT touch: pipe segments (`cat f | grep x`), file redirects (`cat f > out`),
# tty output, or the default (no budget) path.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_gov.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
awk 'BEGIN{for(i=1;i<=5000;i++)print "line " i}' > "$T/big.txt"

pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
# Isolated agent-ish env: non-tty stdout (captured), budget set, sigil/memo off.
run(){ env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=0 JASH_MEMO=0 \
       JASH_BRIEF=0 JASH_NUDGE=0 JASH_DRIFT=0 JASH_LOOPGUARD=off "$@"; }

# ── 1. builtin cat is capped + emits the recall-able footer ──────────────────
out="$(run JASH_LLM_BUDGET=500 "$JASH" -c "cat $T/big.txt" 2>&1)"
bytes=$(printf '%s' "$out" | wc -c | tr -d ' ')
if grep -q 'context-governor' <<<"$out" && [[ "$bytes" -lt 3000 ]]; then
    ok "cat capped ($bytes B) with governor footer"
else no "cat not capped / no footer" "$bytes B"; fi

# ── 2. the capped output spilled to a recoverable overflow file ──────────────
if ls "$T/home/.jash/overflow/"*.txt >/dev/null 2>&1; then ok "overflow spill file written"
else no "no overflow spill file"; fi

# ── 3. CRITICAL: a builtin in a PIPE is never capped (would break the pipe) ──
n="$(run JASH_LLM_BUDGET=500 "$JASH" -c "cat $T/big.txt | grep -c line" 2>/dev/null)"
if [[ "$n" == "5000" ]]; then ok "pipe segment not capped ($n lines intact)"
else no "PIPE TRUNCATED — data loss" "$n"; fi

# ── 4. default (no budget) → full output, no footer ──────────────────────────
out="$(run "$JASH" -c "cat $T/big.txt" 2>&1)"; n="$(grep -c line <<<"$out")"
if [[ "$n" == "5000" ]] && ! grep -q 'context-governor' <<<"$out"; then
    ok "no budget → full output, no footer"
else no "default path altered" "$n"; fi

# ── 5. file redirect is never capped (output to a file must be complete) ─────
run JASH_LLM_BUDGET=500 "$JASH" -c "cat $T/big.txt > $T/out.txt" 2>/dev/null
n="$(grep -c line "$T/out.txt" 2>/dev/null)"
if [[ "$n" == "5000" ]]; then ok "redirect to file → complete ($n)"
else no "file redirect capped" "$n"; fi

# ── 6. small output under budget → verbatim, no footer ───────────────────────
out="$(run JASH_LLM_BUDGET=100000 "$JASH" -c 'echo hello' 2>&1)"
if [[ "$out" == "hello" ]]; then ok "under-budget output verbatim"
else no "under-budget altered" "$out"; fi

# ── DELEGATING builtins (xargs, diff, stat, env, and the awk/sed/wc/find
# fallbacks). These reach jash_exec_external from INSIDE the builtin, with fd 1
# ALREADY redirected by builtin_redir.c (dup2 in the parent) or by pipeline.c.
# exec.c only saw "stdout is not a tty" and capped the pipe and the file:
# measured 2026-07-28, `xargs cat < list > f` wrote 131167 B of 2380000 B, rc=0,
# nothing on stderr. Cases 1-6 never caught it because they only exercise NATIVE
# builtins, governed at the writer level in eval.c, which never touch exec.c.
# Both ends have to hold: no data loss when redirected, still capped when the
# output is heading for the agent's window. `xargs` is the probe because it
# still delegates unconditionally; `find -exec` went native and would pass for
# the wrong reason.
printf '%s\n' "$T/big.txt" > "$T/list.txt"

# ── 7. delegating builtin in a PIPE is never capped ──────────────────────────
n="$(run JASH_LLM_BUDGET=500 "$JASH" -c "xargs cat < $T/list.txt | grep -c line" 2>/dev/null)"
if [[ "$n" == "5000" ]]; then ok "delegated builtin in pipe intact ($n lines)"
else no "DELEGATED PIPE TRUNCATED — data loss" "$n"; fi

# ── 8. delegating builtin redirected to a FILE is never capped ───────────────
run JASH_LLM_BUDGET=500 "$JASH" -c "xargs cat < $T/list.txt > $T/deleg.txt" 2>/dev/null
n="$(grep -c line "$T/deleg.txt" 2>/dev/null)"
if [[ "$n" == "5000" ]] && ! grep -q 'context-governor' "$T/deleg.txt"; then
    ok "delegated builtin > file complete ($n lines, no footer)"
else no "DELEGATED FILE TRUNCATED — data loss" "$n"; fi

# ── 9. the other end: same command at top level IS still capped ──────────────
out="$(run JASH_LLM_BUDGET=500 "$JASH" -c "xargs cat < $T/list.txt" 2>&1)"
bytes=$(printf '%s' "$out" | wc -c | tr -d ' ')
if grep -q 'context-governor' <<<"$out" && [[ "$bytes" -lt 3000 ]]; then
    ok "delegated builtin to the agent window still capped ($bytes B)"
else no "governor lost on the delegated path" "$bytes B"; fi

# ── 10. and the plain external path keeps its cap ────────────────────────────
out="$(run JASH_LLM_BUDGET=500 "$JASH" -c "/bin/cat $T/big.txt" 2>&1)"
bytes=$(printf '%s' "$out" | wc -c | tr -d ' ')
if grep -q 'context-governor' <<<"$out" && [[ "$bytes" -lt 3000 ]]; then
    ok "external command still capped ($bytes B)"
else no "governor lost on the external path" "$bytes B"; fi

echo "── builtin output governor (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
