#!/usr/bin/env bash
# JINV-126 — a variable set INSIDE a jash session must reach that session's
# own getenv(), not just its children's environment.
#
# jash's contract is that the var table IS the child's environment (see
# builtins/export.c): `V=1; sh -c 'echo $V'` prints 1 with no `export`. But
# the table was materialized into envp at exec time only, so the shell's own
# `environ` stayed frozen at launch. Every getenv() inside the process — 62
# JASH_* knobs, plus libc's and any dlopened module's — read the frozen copy.
#
# Consequence: documented escape hatches silently no-op'd. The memo banner
# literally prints "JASH_MEMO=0 re-prints"; following that advice from inside
# a session changed nothing, so an agent concluded the FILE was unchanged
# rather than that its override had been ignored.
#
# Why it survived a 41-gate suite: every existing gate exports the knobs from
# BASH (see the header of any test_jinv*.sh), where they are inherited by the
# jash child and work correctly. The in-session path was never exercised.
#
# Direction matters. Failing to bypass a guard fails safe; failing to APPLY
# JASH_GUARD_STRICT / JASH_MODULE_REQUIRE_HASH / JASH_CLAIM_STRICT fails open,
# with the shell reporting hardening it never enforced.
set -u

JASH="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
[ -x "$JASH" ] || { echo "SKIP: no jash at $JASH"; exit 0; }

# Correctness gates must not pay the audit-chain startup scan (JINV-122): with
# a large per-repo chain it costs seconds PER INVOCATION, which turns a
# sub-second gate into a timeout and looks like a hang, not a failure.
export JASH_SIGIL=0
# Os testes de memo neste gate precisam que a sessao "cheire a agente" e de um
# override para funcionar enquanto capturam stdout (BUG-067).
export JASH_AGENT=1 JASH_MEMO_TEST=1
export JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off

# JINV-126 may be executed from within a jash agent shell. A prior in-session
# `export JASH_MEMO=0` is mirrored to the real environment by design; remove any
# inherited override before starting the gate.
unset JASH_MEMO

pass=0; fail=0
ck() { # ck <label> <expected> <actual>
    if [ "$2" = "$3" ]; then pass=$((pass+1)); printf '  ok   %s\n' "$1"
    else fail=$((fail+1)); printf 'FAIL  %s\n       want=%s\n       got =%s\n' "$1" "$2" "$3"; fi
}

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
F="$TMP/probe.txt"; echo hi > "$F"

# ── 1. The reported failure: in-session override must take effect ──────────
# Two identical commands after `export JASH_MEMO=0`. Memo must NOT collapse
# the second one, so we expect two lines of real output and no memo banner.
out="$(JASH_SESSION=jinv126-test-1 "$JASH" -c "export JASH_MEMO=0; wc -l $F; wc -l $F" 2>&1)"
n_banner="$(printf '%s\n' "$out" | grep -c 'jash memo' || true)"
ck "in-session export JASH_MEMO=0 suppresses memo" "0" "$n_banner"

# ── 2. No regression: without the override, memo still collapses ───────────
# Use a fresh session so a prior in-session JASH_MEMO=0 is not restored from
# session-scoped persistence (jash_session_save/load in core/persist.c).
G="$TMP/fresh$$.txt"; echo hi > "$G"
out="$(JASH_SESSION=jinv126-test-2 "$JASH" -c "wc -l $G; wc -l $G" 2>&1)"
n_banner="$(printf '%s\n' "$out" | grep -c 'jash memo' || true)"
ck "memo still active when not overridden" "1" "$n_banner"

# ── 3. unset must un-mirror (the unsetenv path) ────────────────────────────
H="$TMP/fresh2$$.txt"; echo hi > "$H"
out="$(JASH_SESSION=jinv126-test-3 "$JASH" -c "export JASH_MEMO=0; unset JASH_MEMO; wc -l $H; wc -l $H" 2>&1)"
n_banner="$(printf '%s\n' "$out" | grep -c 'jash memo' || true)"
ck "unset restores default (memo returns)" "1" "$n_banner"

# ── 4. The flat-table contract must survive: children still inherit ────────
# The mirror is additive; it must not have broken envp construction.
# O envelope JSON do JASH_AGENT vai para stderr; comparamos apenas stdout.
got="$("$JASH" -c 'V=childvis; env | grep -c "^V=childvis$"' 2>/dev/null)"
ck "child still inherits var set without export" "1" "$got"

got="$("$JASH" -c 'export W=exp; env | grep -c "^W=exp$"' 2>/dev/null)"
ck "child still inherits exported var" "1" "$got"

# ── 5. unset must remove it from the child's environment too ───────────────
got="$("$JASH" -c 'V=gone; unset V; env | grep -c "^V=" || true' 2>/dev/null)"
ck "unset removes var from child env" "0" "$got"

# ── 6. Overwrite must not duplicate: one key, one entry ────────────────────
got="$("$JASH" -c 'V=a; V=b; V=c; env | grep -c "^V="' 2>/dev/null)"
ck "repeated assignment keeps a single entry" "1" "$got"

got="$("$JASH" -c 'V=a; V=b; V=c; env | grep "^V="' 2>/dev/null)"
ck "repeated assignment keeps the LAST value" "V=c" "$got"

printf '\nJINV-126: %d pass / %d fail\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
