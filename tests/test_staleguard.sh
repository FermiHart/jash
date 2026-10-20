#!/usr/bin/env bash
# tests/test_staleguard.sh — ADR-0038 W3: stale-write guard behavior matrix.
#
# The #1 agent-corruption bug, checked by physics: read a file (ledger notes
# the version) → someone else changes it → the agent mutates it based on the
# outdated view → LOUD warning. No external change → silence.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/work"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

SID="stale-$$"
runw() { ( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
           CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$SID" JASH_SIGIL=1 \
           JASH_BRIEF=0 JASH_NUDGE=0 "$@" 2>&1 >/dev/null ); }

printf 'v1\n' > "$T/work/doc.txt"

# 1. agent reads the file (ledger note), no warning on the read itself
out=$(runw "$JASH" -c "cat doc.txt")
case "$out" in
  *STALE*) err "read alone warned";;
  *) ok "read alone: silent";;
esac

# 2. agent mutates WITHOUT external change -> no warning
out=$(runw "$JASH" -c "echo v2 >> doc.txt")
case "$out" in
  *STALE*) err "clean write warned ('$out')";;
  *"files changed"*) ok "clean write: files_changed only, no stale warning";;
  *) err "clean write unexpected ('$out')";;
esac

# 3. re-read (refresh ledger), external change, agent overwrites -> WARNING
runw "$JASH" -c "cat doc.txt" >/dev/null
sleep 0.01
printf 'external-edit\n' >> "$T/work/doc.txt"   # someone else changes it
out=$(runw "$JASH" -c "echo agent-overwrite > doc.txt")
case "$out" in
  *"STALE-WRITE WARNING"*doc.txt*) ok "stale overwrite: LOUD warning";;
  *) err "stale overwrite (got '$out')";;
esac

# 4. deletion of a stale-read file also warns
printf 'v1\n' > "$T/work/gone.txt"
runw "$JASH" -c "cat gone.txt" >/dev/null
sleep 0.01
printf 'changed\n' >> "$T/work/gone.txt"
out=$(runw "$JASH" -c "rm -f gone.txt")
case "$out" in
  *"STALE-WRITE WARNING"*gone.txt*) ok "stale deletion warns";;
  *) err "stale deletion (got '$out')";;
esac

# 5. JASH_STALE_GUARD=0 silences (files_changed stays)
printf 'v1\n' > "$T/work/quiet.txt"
runw "$JASH" -c "cat quiet.txt" >/dev/null
sleep 0.01
printf 'changed\n' >> "$T/work/quiet.txt"
out=$( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$SID" JASH_SIGIL=1 JASH_BRIEF=0 \
      JASH_NUDGE=0 JASH_STALE_GUARD=0 "$JASH" -c "echo x > quiet.txt" 2>&1 >/dev/null )
case "$out" in
  *STALE*) err "JASH_STALE_GUARD=0 still warned";;
  *"files changed"*) ok "JASH_STALE_GUARD=0: silenced";;
  *) err "guard-off unexpected ('$out')";;
esac

# 6. different session's read does not arm the guard for this session
printf 'v1\n' > "$T/work/other.txt"
( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb CLAUDECODE=1 \
  CLAUDE_CODE_SESSION_ID=other-sid JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
  "$JASH" -c "cat other.txt" >/dev/null 2>&1 )
sleep 0.01
printf 'changed\n' >> "$T/work/other.txt"
out=$(runw "$JASH" -c "echo x > other.txt")
case "$out" in
  *STALE*) err "other session's read armed this session's guard";;
  *) ok "session-scoped ledger";;
esac

# 7. JSON envelope carries stale_writes
printf 'v1\n' > "$T/work/env.txt"
runw "$JASH" -c "cat env.txt" >/dev/null
sleep 0.01
printf 'changed\n' >> "$T/work/env.txt"
out=$( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
      CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$SID" JASH_SIGIL=1 JASH_BRIEF=0 \
      JASH_NUDGE=0 JASH_AGENT=1 "$JASH" -c "echo x > env.txt" 2>&1 >/dev/null )
case "$out" in
  *'"stale_writes":["env.txt"]'*) ok "envelope carries stale_writes";;
  *) err "envelope stale (got '$out')";;
esac

echo "═══════════════════════════════════════════════════════════════"
echo "  stale-write guard (ADR-0038 W3) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ stale-guard tests PASSED"; exit 0; } \
                  || { echo "  ✗ stale-guard tests FAILED"; exit 1; }
