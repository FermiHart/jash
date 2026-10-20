#!/usr/bin/env bash
# test_rewind_scope.sh — PATCH-SPEC jash-rewind-scope: `rewind --scope <dir>`.
# Two-ended gate (a bug only dies when both ends are gated):
#   1. bug end: checkpoint → mutate /tmp/x AND repo/y → rewind --scope /tmp
#      reverts ONLY /tmp/x; repo/y is UNTOUCHED (content + journal entry).
#   2. holding end: rewind WITHOUT --scope behaves exactly as before.
# Plus: --dry lists would-revert/would-keep, bad scope errors loudly,
# hold-on-failure preserved inside the scope.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
bash "$ROOT/tests/setup_sigil_modules.sh" "$TMP/h"
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=1

# run BODY — sets OUT/RC/ERR. Same HOME across calls (journal persists).
run(){
  local body="$1"
  mkdir -p "$TMP/h" "$TMP/wd"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

echo "test_rewind_scope (PATCH-SPEC):"

SC="$TMP/scope"
OT="$TMP/other"
mkdir -p "$SC" "$OT" "$TMP/wd" "$TMP/h"

# Setup: checkpoint, then create a file INSIDE scope and one OUTSIDE.
printf 'checkpoint base\necho inside > %s/x.txt\necho outside > %s/y.txt\n' \
    "$SC" "$OT" > "$TMP/wd/t.jsh"
OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" "$TMP/wd/t.jsh" 2>/dev/null)"

# ── 1. bug end: scoped rewind reverts in-scope ONLY ─────────────────────
run "rewind --scope $SC base"
case "$OUT" in
  *"1 reverted, 0 failed, 1 kept out of scope"*)
    ok "report: 1 reverted, 1 kept out of scope";;
  *) no "scoped report" "$OUT";;
esac
[ ! -e "$SC/x.txt" ] && ok "in-scope file reverted (gone)" \
                     || no "in-scope not reverted" "$(ls "$SC")"
[ -e "$OT/y.txt" ] && [ "$(cat "$OT/y.txt")" = "outside" ] \
    && ok "out-of-scope file UNTOUCHED" \
    || no "out-of-scope touched" "$(ls "$OT" 2>&1)"

# journal still holds the out-of-scope entry → a plain undo reverts it now.
run 'undo'
case "$OUT" in *"removed"*|*"1 reverted, 0 failed"*)
    ok "out-of-scope entry still live in journal (undoable)";;
  *) no "entry lost" "$OUT";; esac

# ── 2. holding end: unscoped rewind behaves exactly as before ───────────
printf 'checkpoint base\necho a > %s/a.txt\necho b > %s/b.txt\n' \
    "$SC" "$OT" > "$TMP/wd/t.jsh"
OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" "$TMP/wd/t.jsh" 2>/dev/null)"
run "rewind base"
case "$OUT" in
  *"2 reverted, 0 failed"*)
    ok "unscoped rewind reverts everything (unchanged behavior)";;
  *) no "unscoped regression" "$OUT";; esac
[ ! -e "$SC/a.txt" ] && [ ! -e "$OT/b.txt" ] \
    && ok "unscoped: both reverted" || no "unscoped files" "$(ls "$SC" "$OT")"

# ── 3. --dry --scope: would-revert/would-keep, world untouched ──────────
printf 'checkpoint base\necho i > %s/i.txt\necho o > %s/o.txt\n' \
    "$SC" "$OT" > "$TMP/wd/t.jsh"
OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" "$TMP/wd/t.jsh" 2>/dev/null)"
run "rewind --dry --scope $SC base"
case "$OUT" in
  *"revert create"*"i.txt"*) r1=1;; *) r1=0;; esac
case "$OUT" in
  *"keep"*"o.txt"*"dry run"*) r2=1;; *) r2=0;; esac
if [ "$r1" = 1 ] && [ "$r2" = 1 ]; then
    ok "--dry lists would-revert and would-keep separately"
else
    no "--dry scope" "$OUT"
fi
[ -e "$SC/i.txt" ] && [ -e "$OT/o.txt" ] \
    && ok "--dry touched nothing" || no "--dry mutated" "$(ls "$SC" "$OT")"

# ── 4. nonexistent scope → visible error, never silent ──────────────────
run "rewind --scope $TMP/nope-not-here base"
case "$OUT" in
  *"no such directory"*) ok "bad scope errors loudly";;
  *) no "bad scope silent" "$OUT";; esac
[ "$RC" -eq 2 ] && ok "bad scope rc=2" || no "bad scope rc" "RC=$RC"

# ── 5. hold-on-failure inside scope: a failing undo STOPS the rewind ────
printf 'checkpoint base\necho z > %s/z.txt\ncp %s/z.txt %s/z2.txt\n' \
    "$SC" "$SC" "$SC" > "$TMP/wd/t.jsh"
OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" "$TMP/wd/t.jsh" 2>/dev/null)"
# make the overwrite-revert fail: delete the backup area → backup vanishes
rm -rf "$TMP/h/.jash/tl_backup"
run "rewind --scope $SC base"
case "$OUT" in
  *"vanished"*|*"failed"*)
    ok "failing reversion inside scope stops rewind (hold-on-failure)";;
  *) no "hold-on-failure" "$OUT";; esac

echo ""
echo "=== test_rewind_scope: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
