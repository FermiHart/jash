#!/usr/bin/env bash
# tests/test_drift.sh — ADR-0043 W1: environment-drift forensics.
#
# drift records an env fingerprint per (cmd, cwd) on success and, on a failure
# of the same command, names exactly what changed since the last success.
# Proof style: control vs treatment — same failing command, with and without
# an environment change between the recorded success and the failure.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_drift.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin1" "$T/bin2" "$T/wd" "$T/h2" "$T/h3"
printf '#!/bin/sh\nexit 0\n' > "$T/bin1/mytool"
printf '#!/bin/sh\nexit 1\n' > "$T/bin2/mytool"
chmod +x "$T/bin1/mytool" "$T/bin2/mytool"

pass=0; fail=0
ok() { pass=$((pass+1)); echo "  ✓ $1"; }
no() { fail=$((fail+1)); echo "  ✗ $1"; }

# Agent-session env (drift is agent-only); isolate every other stateful feature.
BASE=(env -i HOME="$T" CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="drift-$$" TERM=dumb
      JASH_BRIEF=0 JASH_SIGIL=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_CHANGES=0
      JASH_NUDGE=0 JASH_PREFETCH=0)

# ── 1. success records a ledger row ─────────────────────────────────────────
(cd "$T/wd" && "${BASE[@]}" PATH="$T/bin1:/usr/bin:/bin" "$JASH" -c mytool) >/dev/null 2>&1
if [[ -s "$T/.jash/drift.tsv" ]]; then ok "success records drift row"; else no "success records drift row"; fi

# ── 2. re-success upserts (no duplicate rows for the same key) ──────────────
(cd "$T/wd" && "${BASE[@]}" PATH="$T/bin1:/usr/bin:/bin" "$JASH" -c mytool) >/dev/null 2>&1
n=$(wc -l < "$T/.jash/drift.tsv" | tr -d ' ')
if [[ "$n" == "1" ]]; then ok "upsert keeps one row per key"; else no "upsert keeps one row per key (got $n)"; fi

# ── 3. failure with NO drift → silent (failure isn't env-caused) ────────────
touch "$T/flag"
(cd "$T/wd" && "${BASE[@]}" PATH="/usr/bin:/bin" "$JASH" -c "test -f $T/flag") >/dev/null 2>&1
rm -f "$T/flag"
err=$( (cd "$T/wd" && "${BASE[@]}" PATH="/usr/bin:/bin" "$JASH" -c "test -f $T/flag") 2>&1 >/dev/null )
if [[ "$err" != *"[jash drift]"* ]]; then ok "no drift → silent"; else no "no drift → silent (got: $err)"; fi

# ── 4. PATH/tool drift on failure → named ────────────────────────────────────
err=$( (cd "$T/wd" && "${BASE[@]}" PATH="$T/bin2:/usr/bin:/bin" "$JASH" -c mytool) 2>&1 >/dev/null )
if [[ "$err" == *"[jash drift]"* && "$err" == *PATH* ]]; then ok "PATH drift named"; else no "PATH drift named (got: $err)"; fi
if [[ "$err" == *mytool* ]]; then ok "tool change named"; else no "tool change named (got: $err)"; fi

# ── 5. env-var drift (CC) on failure → named with transition marker ─────────
touch "$T/flag2"
(cd "$T/wd" && "${BASE[@]}" PATH="/usr/bin:/bin" CC=clang "$JASH" -c "test -f $T/flag2") >/dev/null 2>&1
rm -f "$T/flag2"
err=$( (cd "$T/wd" && "${BASE[@]}" PATH="/usr/bin:/bin" CC=gcc "$JASH" -c "test -f $T/flag2") 2>&1 >/dev/null )
if [[ "$err" == *"[jash drift]"* && "$err" == *CC* ]]; then ok "env var (CC) drift named"; else no "env var (CC) drift named (got: $err)"; fi

# ── 6. git HEAD drift → named ────────────────────────────────────────────────
if command -v git >/dev/null 2>&1; then
  R="$T/repo"; mkdir -p "$R"
  git -C "$R" init -q
  git -C "$R" -c user.email=t@t -c user.name=t commit -q --allow-empty -m one
  touch "$T/flag3"
  (cd "$R" && "${BASE[@]}" PATH="/usr/bin:/bin" "$JASH" -c "test -f $T/flag3") >/dev/null 2>&1
  git -C "$R" -c user.email=t@t -c user.name=t commit -q --allow-empty -m two
  rm -f "$T/flag3"
  err=$( (cd "$R" && "${BASE[@]}" PATH="/usr/bin:/bin" "$JASH" -c "test -f $T/flag3") 2>&1 >/dev/null )
  if [[ "$err" == *"git HEAD"* ]]; then ok "git HEAD drift named"; else no "git HEAD drift named (got: $err)"; fi
else
  ok "git HEAD drift named (skipped: no git)"
fi

# ── 7. JASH_DRIFT=0 → fully off (no warn, no ledger) ────────────────────────
err=$( (cd "$T/wd" && "${BASE[@]}" PATH="$T/bin2:/usr/bin:/bin" JASH_DRIFT=0 "$JASH" -c mytool) 2>&1 >/dev/null )
if [[ "$err" != *"[jash drift]"* ]]; then ok "JASH_DRIFT=0 silences warning"; else no "JASH_DRIFT=0 silences warning"; fi
(cd "$T/wd" && "${BASE[@]}" HOME="$T/h2" PATH="/usr/bin:/bin" JASH_DRIFT=0 "$JASH" -c true) >/dev/null 2>&1
if [[ ! -e "$T/h2/.jash/drift.tsv" ]]; then ok "JASH_DRIFT=0 skips ledger"; else no "JASH_DRIFT=0 skips ledger"; fi

# ── 8. non-agent session → no ledger, no warning ─────────────────────────────
(cd "$T/wd" && env -i HOME="$T/h3" PATH="/usr/bin:/bin" TERM=dumb JASH_BRIEF=0 JASH_SIGIL=0 "$JASH" -c true) >/dev/null 2>&1
if [[ ! -e "$T/h3/.jash/drift.tsv" ]]; then ok "non-agent session inert"; else no "non-agent session inert"; fi

# ── 9. never-succeeded command fails → silent ────────────────────────────────
err=$( (cd "$T/wd" && "${BASE[@]}" PATH="/usr/bin:/bin" "$JASH" -c "test -f $T/never_existed") 2>&1 >/dev/null )
if [[ "$err" != *"[jash drift]"* ]]; then ok "no prior success → silent"; else no "no prior success → silent (got: $err)"; fi

echo "── drift (ADR-0043 W1) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] || exit 1
exit 0
