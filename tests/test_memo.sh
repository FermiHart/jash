#!/usr/bin/env bash
# tests/test_memo.sh — ADR-0038 W1: read memoization behavior matrix.
#
# A repeated read-only command with unchanged inputs must answer with ONE
# attested "[jash memo]" line instead of re-emitting bytes; any input change,
# capture context, opt-out, or observer change must re-print in full.
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

# Agent-session env: recording on (memo rides the attested floor — the
# SIGIL=0 ultra-fast path skips the full eval pipeline by design).
run() { # sid cmd
  local sid="$1"; shift
  env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$sid" \
      JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      "$JASH" -c "$*" 2>/dev/null
}

printf 'alpha\nbeta\ngamma\n' > "$T/f.txt"

# 1. first run: full content
out=$(run s1 "cat $T/f.txt")
[[ "$out" == "$(printf 'alpha\nbeta\ngamma')" ]] \
    && ok "external cat: 1st run full content" \
    || err "external cat 1st run (got '$out')"

# 2. second run: memo line, correct metadata
out=$(run s1 "cat $T/f.txt")
case "$out" in
  *"[jash memo] unchanged"*"3 lines"*) ok "external cat: 2nd run memoized (3 lines)";;
  *) err "external cat 2nd run (got '$out')";;
esac

# 3. rc preserved (exit code of memoized run)
run s1 "cat $T/f.txt" >/dev/null; rc=$?
[[ $rc -eq 0 ]] && ok "memoized rc preserved (0)" || err "memoized rc ($rc)"

# 4. input change -> full content again
sleep 0.01; printf 'delta\n' >> "$T/f.txt"
out=$(run s1 "cat $T/f.txt")
case "$out" in
  *delta*) ok "modified input: full content again";;
  *) err "modified input (got '$out')";;
esac

# 5. JASH_MEMO=0 -> never memoized
run s1 "cat $T/f.txt" >/dev/null
out=$(env -i HOME="$T/home" PATH="$PATH" TERM=dumb CLAUDECODE=1 \
      CLAUDE_CODE_SESSION_ID=s1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      JASH_MEMO=0 "$JASH" -c "cat $T/f.txt" 2>/dev/null)
case "$out" in
  *"[jash memo]"*) err "JASH_MEMO=0 still memoized";;
  *delta*) ok "JASH_MEMO=0 re-prints in full";;
  *) err "JASH_MEMO=0 unexpected ('$out')";;
esac

# 6. different session id (different observer) -> full content
out=$(run s2 "cat $T/f.txt")
case "$out" in
  *"[jash memo]"*) err "new session served stale memo";;
  *delta*) ok "new session id: full content";;
  *) err "new session unexpected ('$out')";;
esac

# 7. cmdsub capture NEVER memoized (value integrity)
run s1 "v=\$(cat $T/f.txt); echo \"\$v\"" >/dev/null
out=$(run s1 "v=\$(cat $T/f.txt); echo \"\$v\"")
case "$out" in
  *"[jash memo]"*) err "cmdsub capture polluted by memo";;
  *delta*) ok "cmdsub capture stays real content";;
  *) err "cmdsub capture unexpected ('$out')";;
esac

# 8. pipelines never memoized
run s1 "cat $T/f.txt | wc -l" >/dev/null
out=$(run s1 "cat $T/f.txt | wc -l" | tr -d ' ')
[[ "$out" == "4" ]] && ok "pipeline stays real" || err "pipeline (got '$out')"

# 9. grep builtin: repeat memoized, rc=1 (no match) preserved
out1=$(run s1 "grep alpha $T/f.txt")
out2=$(run s1 "grep alpha $T/f.txt")
case "$out2" in
  *"[jash memo]"*) ok "grep builtin memoized on repeat";;
  *) err "grep builtin repeat (got '$out2')";;
esac
run s1 "grep nomatchxyz $T/f.txt" >/dev/null 2>&1
run s1 "grep nomatchxyz $T/f.txt" >/dev/null 2>&1; rc=$?
[[ $rc -eq 1 ]] && ok "grep rc=1 (no match) preserved through memo" \
               || err "grep memo rc ($rc)"

# 10. different flags -> different key -> full run
run s1 "head -n 2 $T/f.txt" >/dev/null
out=$(run s1 "head -n 3 $T/f.txt")
case "$out" in
  *"[jash memo]"*) err "different flags served same memo";;
  *gamma*) ok "different flags: fresh run";;
  *) err "different flags unexpected ('$out')";;
esac

# 11. ls: repeat memoized; new entry in dir -> full again
mkdir "$T/dir"; touch "$T/dir/a" "$T/dir/b"
run s1 "ls $T/dir" >/dev/null
out=$(run s1 "ls $T/dir")
case "$out" in
  *"[jash memo]"*) ok "ls memoized on repeat";;
  *) err "ls repeat (got '$out')";;
esac
touch "$T/dir/c"
out=$(run s1 "ls $T/dir")
case "$out" in
  *c*) ok "dir change: ls re-lists";;
  *) err "dir change (got '$out')";;
esac

# 12. non-agent env -> never memoized
out=$(env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
      JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "cat $T/f.txt" 2>/dev/null)
out=$(env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
      JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "cat $T/f.txt" 2>/dev/null)
case "$out" in
  *"[jash memo]"*) err "non-agent session memoized";;
  *delta*) ok "non-agent session always full";;
  *) err "non-agent unexpected ('$out')";;
esac

echo "═══════════════════════════════════════════════════════════════"
echo "  memo (ADR-0038 W1) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ memo tests PASSED"; exit 0; } \
                  || { echo "  ✗ memo tests FAILED"; exit 1; }
