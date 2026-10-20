#!/usr/bin/env bash
# tests/test_xargs.sh — ADR-0044: `xargs` builtin (batch-build + safe execution).
#
# xargs is a top context-flooder and a classic footgun (`find | xargs rm`). jash's
# in-process xargs OWNS the safe angles: (1) `--plan`/`--dry-run` prints the exact
# batches it WOULD run as JSON {argv} without executing — a preview no upstream
# xargs offers; (2) real batches run through jash's evaluator, so each spawned
# command is attested / auto-checkpointed / output-governed (`| xargs rm` is
# recoverable). Input splitting is POSIX-correct (blank/newline + quotes, -0 NUL,
# -I whole-line). Advanced flags (-P, -a, …) fall back to the real xargs.
# This gate also locks the BUG-165 flagship: `xargs -0 rm` on a spaced filename.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_xargs.XXXXXX)"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
# PATH kept real so the fallback path can find the system xargs.
run(){ env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

# ── 1. default: whitespace-split into ONE batch ──────────────────────────────
out="$(printf 'a\nb\nc\n' | run "xargs echo")"
[[ "$out" == "a b c" ]] && ok "default batch: 'a b c'" || no "default batch" "$out"

# ── 2. -n1: one batch per item ───────────────────────────────────────────────
out="$(printf 'a\nb\nc\n' | run "xargs -n1 echo" | tr '\n' '|')"
[[ "$out" == "a|b|c|" ]] && ok "-n1 → three batches" || no "-n1" "$out"

# ── 3. -I{}: template substitution ───────────────────────────────────────────
out="$(printf 'x\ny\n' | run "xargs -I{} echo item={} done" | tr '\n' '|')"
[[ "$out" == "item=x done|item=y done|" ]] && ok "-I{} template" || no "-I{}" "$out"

# ── 4. -0: NUL-separated items keep embedded spaces ──────────────────────────
out="$(printf 'a b\0c d\0' | run "xargs -0 -n1 echo" | tr '\n' '|')"
[[ "$out" == "a b|c d|" ]] && ok "-0 preserves spaced items" || no "-0" "$out"

# ── 5. input quoting: 'a b' is ONE item (POSIX xargs quoting) ────────────────
out="$(printf "'a b'\nc\n" | run "xargs -n1 echo" | tr '\n' '|')"
[[ "$out" == "a b|c|" ]] && ok "quoted input stays one item" || no "input quoting" "$out"

# ── 6. --plan previews JSON batches and runs NOTHING ─────────────────────────
: > "$T/keep1"; : > "$T/keep2"
out="$(printf 'keep1\nkeep2\n' | run "cd '$T' && xargs --plan rm")"
if [[ "$out" == *'"argv"'* && "$out" == *'"rm"'* && -e "$T/keep1" && -e "$T/keep2" ]]; then
    ok "--plan emits JSON {argv}, deletes nothing"
else no "--plan preview" "$out"; fi
rm -f "$T/keep1" "$T/keep2"

# ── 7. GOLD: real `xargs rm` removes via the evaluator (attested/undoable) ────
: > "$T/f1"; : > "$T/f2"
printf 'f1\nf2\n' | run "cd '$T' && xargs rm" >/dev/null 2>&1
[[ ! -e "$T/f1" && ! -e "$T/f2" ]] && ok "xargs rm removed both" || no "xargs rm"

# ── 8. FLAGSHIP (BUG-165): xargs -0 rm on a spaced filename, decoy survives ──
: > "$T/a b"; : > "$T/b"
printf 'a b\0' | run "cd '$T' && xargs -0 rm" >/dev/null 2>&1
if [[ ! -e "$T/a b" && -e "$T/b" ]]; then
    ok "xargs -0 rm 'a b' removed the spaced file, left decoy 'b'"
else no "FLAGSHIP xargs -0 rm mis-split (BUG-165)" "a b:$([[ -e "$T/a b" ]] && echo y||echo n) b:$([[ -e "$T/b" ]] && echo y||echo n)"; fi
rm -f "$T/a b" "$T/b"

# ── 9. empty input → one run by default, zero with -r ────────────────────────
out="$(printf '' | run "xargs echo" | od -An -c | tr -d ' \n')"
[[ "$out" == '\n' ]] && ok "empty input → one blank echo" || no "empty→one-run" "$out"
out="$(printf '' | run "xargs -r echo")"
[[ -z "$out" ]] && ok "-r suppresses the empty run" || no "-r empty" "$out"

# ── 10. -I with empty input → zero runs ──────────────────────────────────────
out="$(printf '' | run "xargs -I{} echo nope")"
[[ -z "$out" ]] && ok "-I empty input → zero runs" || no "-I empty" "$out"

# ── 11. exit status: a failing child yields 123 ──────────────────────────────
printf 'a\n' | run "xargs false" >/dev/null 2>&1; rc=$?
[[ "$rc" == 123 ]] && ok "child failure → rc 123" || no "rc on failure" "rc=$rc"

# ── 12. --plan + an unowned flag errors (loud, never silent-wrong) ───────────
printf 'a\n' | run "xargs --plan -P2 rm" >/dev/null 2>&1; rc=$?
[[ "$rc" != 0 ]] && ok "--plan + unsupported flag errors (rc=$rc)" || no "--plan unowned flag" "rc=$rc"

# ── 13. fallback: -P defers to the real xargs (still works) ──────────────────
if command -v xargs >/dev/null 2>&1; then
    out="$(printf 'a\nb\n' | run "xargs -P2 -n1 echo" | sort | tr '\n' '|')"
    [[ "$out" == "a|b|" ]] && ok "-P falls back to real xargs" || no "-P fallback" "$out"
else ok "-P fallback (system xargs absent — skipped)"; fi

# ── 14. -t traces the command to stderr before running ───────────────────────
err="$(printf 'a\n' | run "xargs -t echo hi" 2>&1 >/dev/null)"
[[ "$err" == *"echo"* && "$err" == *"hi"* ]] && ok "-t traces to stderr" || no "-t trace" "$err"

echo "── xargs builtin (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
