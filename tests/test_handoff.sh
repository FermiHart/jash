#!/usr/bin/env bash
# tests/test_handoff.sh — ADR-0038 W8: session handoff compiler matrix.
#
# `handoff` distills the session (command record + fs journal + git) into an
# LLM-optimized brief: command span, grouped failures, files touched, repo
# state. Verifies BUG-063 too (session record actually accumulates).
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/probe-home"
jash_usable() {
  /usr/bin/env -u JASH_SESSION -u JASH_AGENT \
    HOME="$T/probe-home" JASH_SIGIL=0 JASH_GUARD_BYPASS=0 \
    JASH_GUARD_BYPASS_STRICT=0 JASH_RECALL_GUARD=0 \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1
}
jash_usable "$JASH" || JASH="$PROJECT_ROOT/build-speed/jash-release"
if ! jash_usable "$JASH"; then echo "SKIP: no runnable jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

mkdir -p "$T/home" "$T/repo"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"
( cd "$T/repo" && git init -q -b main && git config user.email a@b.c &&
  git config user.name x && echo hello > f.txt && git add f.txt &&
  git commit -qm "seed commit" --no-gpg-sign ) 2>/dev/null

SID="handoff-$$"
run() { ( cd "$T/repo" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
          CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$SID" JASH_SIGIL=1 \
          JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "$1" >/dev/null 2>&1 ); }

run "echo working"
run "false"
run "grep zzz f.txt"
run "false"                     # recurring failure
run "echo new > created.txt"    # mutation (created)
( cd "$T/repo" && echo more >> f.txt )   # dirty the tracked file

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

OUT=$( cd "$T/repo" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
       CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="$SID" JASH_SIGIL=0 \
       JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "handoff" 2>&1 )

# 1. header names the session
case "$OUT" in *"Session handoff — $SID"*) ok "header names session";; *) err "header";; esac

# 2. BUG-063: command span accumulated (5 recorded, not 1)
case "$OUT" in *"5 commands"*) ok "session record accumulates (BUG-063)";;
  *) err "span (got: $(printf '%s' "$OUT" | grep -i span))";; esac
case "$OUT" in *"3 failed"*) ok "failure count correct";; *) err "fail count";; esac

# 3. failures grouped, recurring one flagged
case "$OUT" in *"false"*"recurring"*) ok "recurring failure flagged 2x";; *) err "recurring";; esac
case "$OUT" in *"grep zzz"*) ok "distinct failure shape shown";; *) err "distinct fail";; esac

# 4. files touched this session
case "$OUT" in *"Files touched"*"created.txt"*) ok "files touched listed (+created.txt)";;
  *) err "files (got: $(printf '%s' "$OUT" | grep -iA1 touched))";; esac

# 5. repo state
case "$OUT" in *"branch: main"*) ok "repo branch reported";; *) err "branch";; esac
case "$OUT" in *"dirty files: "*) ok "dirty count reported";; *) err "dirty";; esac
case "$OUT" in *"last commit: seed commit"*) ok "last commit reported";; *) err "commit";; esac

# 6. actionable next-step footer
case "$OUT" in *"recall failed"*"impact"*) ok "actionable next-steps";; *) err "footer";; esac

# 7. no-session graceful (no sid env)
OUT2=$( env -i HOME="$T/home" PATH="$PATH" TERM=dumb "$JASH" -c "handoff" 2>&1 )
case "$OUT2" in *"Session handoff"*) ok "runs without a session id";; *) err "no-sid handoff";; esac

echo "═══════════════════════════════════════════════════════════════"
echo "  handoff compiler (ADR-0038 W8) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ handoff tests PASSED"; exit 0; } \
                  || { echo "  ✗ handoff tests FAILED"; exit 1; }
