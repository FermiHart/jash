#!/usr/bin/env bash
# tests/test_prefetch.sh — ADR-0042 F5: the anticipatory shell.
#
# Prefetch learns the command-transition graph from the chain and, at the end of
# a -c call, speculatively runs the most probable NEXT commands — only read-only
# memoizable ones — so the next call hits a warm memo. Default OFF.
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

# agent session (memo active); prefetch toggled per-call via a prefixed VAR.
base() { env -i HOME="$T/home" PATH="$PATH" TERM=dumb CLAUDECODE=1 \
             CLAUDE_CODE_SESSION_ID=s1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 "$@"; }
clear_memo() { rm -f "$T/home/.jash/memo.tsv"; rm -rf "$T/home/.jash/memo_out"; }
is_hit() { case "$1" in *"[jash memo]"*) return 0;; *) return 1;; esac; }

printf 'content of B\n' > "$T/f"

# Train (echo trigger) → (cat f) ×4 so the transition count reaches ≥3.
for i in 1 2 3 4; do
  base JASH_PREFETCH=1 "$JASH" -c "echo trigger" >/dev/null 2>&1
  base JASH_PREFETCH=1 "$JASH" -c "cat $T/f"     >/dev/null 2>&1
done

# ── 1. train A→B ×3; run A warms B → B's next run is a memo hit (+ control) ───
clear_memo
base "$JASH" -c "echo trigger" >/dev/null 2>&1            # PREFETCH OFF (control)
sleep 0.2
is_hit "$(base "$JASH" -c "cat $T/f" 2>&1)" \
    && err "control: B was a hit without prefetch" \
    || ok  "control: B is cold without prefetch"
clear_memo
base JASH_PREFETCH=1 "$JASH" -c "echo trigger" >/dev/null 2>&1   # PREFETCH ON
sleep 0.2
is_hit "$(base JASH_PREFETCH=1 "$JASH" -c "cat $T/f" 2>&1)" \
    && ok  "prefetch warmed B → next run is a memo hit" \
    || err "prefetch did not warm B"

# ── 2. non-whitelisted successor is never prefetched ─────────────────────────
# Train A2 → (touch marker) ×4; touch is NOT memoizable → must not be run.
for i in 1 2 3 4; do
  base JASH_PREFETCH=1 "$JASH" -c "echo trig2" >/dev/null 2>&1
  base JASH_PREFETCH=1 "$JASH" -c "touch $T/marker" >/dev/null 2>&1
  rm -f "$T/marker"
done
rm -f "$T/marker"
base JASH_PREFETCH=1 "$JASH" -c "echo trig2" >/dev/null 2>&1   # would fire touch IF whitelisted
sleep 0.2
[[ ! -e "$T/marker" ]] && ok "non-memoizable successor (touch) not prefetched" \
                       || err "non-whitelisted command was prefetched"

# ── 3. no cascade: a prefetch child (JASH_PREFETCH_CHILD=1) fires nothing ─────
clear_memo
base JASH_PREFETCH=1 JASH_PREFETCH_CHILD=1 "$JASH" -c "echo trigger" >/dev/null 2>&1
sleep 0.2
is_hit "$(base "$JASH" -c "cat $T/f" 2>&1)" \
    && err "prefetch child cascaded (warmed B)" \
    || ok  "recursion guard: prefetch child fires nothing"

# ── 4. budget respected: a hanging successor is killed at MAX_MS (no hang) ────
mkfifo "$T/pipe" 2>/dev/null || true
# seed the transition directly (running 'cat pipe' would block during training)
printf 'echo gobudget\tcat %s\t3\t%s\n' "$T/pipe" "$(date +%s)" > "$T/home/.jash/prefetch.tsv"
( base JASH_PREFETCH=1 JASH_PREFETCH_MAX_MS=300 "$JASH" -c "echo gobudget" >/dev/null 2>&1 ) &
apid=$!
waited=0; done_ok=0
while [[ $waited -lt 40 ]]; do
  kill -0 "$apid" 2>/dev/null || { done_ok=1; break; }
  sleep 0.1; waited=$((waited+1))
done
if [[ $done_ok -eq 1 ]]; then ok "budget: hanging prefetch child killed, A returned"
else kill -9 "$apid" 2>/dev/null; err "budget: A hung (child not killed)"; fi
wait "$apid" 2>/dev/null || true
rm -f "$T/pipe"

# ── 5. mutation after prefetch invalidates the memo (no stale content) ───────
printf 'echo trigger\tcat %s\t3\t%s\n' "$T/f" "$(date +%s)" > "$T/home/.jash/prefetch.tsv"
clear_memo
base JASH_PREFETCH=1 "$JASH" -c "echo trigger" >/dev/null 2>&1   # warm cat f (old content)
sleep 0.2
printf 'BRAND NEW CONTENT\n' > "$T/f"                            # mutate the file
out=$(base "$JASH" -c "cat $T/f" 2>&1)
case "$out" in
  *"BRAND NEW CONTENT"*) ok "mutation invalidates: B re-reads fresh content (no stale memo)";;
  *"[jash memo]"*)       err "stale memo hit after mutation";;
  *)                     err "unexpected B output ('$out')";;
esac

echo "═══════════════════════════════════════════════════════════════"
echo "  prefetch (ADR-0042 F5) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ prefetch tests PASSED"; exit 0; } \
                  || { echo "  ✗ prefetch tests FAILED"; exit 1; }
