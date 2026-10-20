#!/usr/bin/env bash
# tests/test_sed_undo.sh — ADR-0044: sed -i is UNDOABLE (the census gold angle).
#
# In a normal shell `sed -i` is a destructive in-place edit with no undo. jash's
# in-process sed snapshots the file into the undo timeline before overwriting, so
# `undo` restores the original. (Recording must be on — JASH_SIGIL>=1 — for the
# timeline/backup to be active, exactly as for rm/cp.)
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_sedundo.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
run(){ env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=1 JASH_SESSION="sedundo-$$" \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

printf 'original line one\noriginal line two\n' > "$T/f.txt"

# ── 1. sed -i actually edits in place ────────────────────────────────────────
run "sed -i 's/original/EDITED/g' $T/f.txt" >/dev/null 2>&1
if [[ "$(cat "$T/f.txt")" == "$(printf 'EDITED line one\nEDITED line two')" ]]; then
    ok "sed -i edited the file in place"
else no "sed -i did not edit" "$(cat "$T/f.txt")"; fi

# ── 2. undo restores the pre-edit content ────────────────────────────────────
run 'undo' >/dev/null 2>&1
if [[ "$(cat "$T/f.txt")" == "$(printf 'original line one\noriginal line two')" ]]; then
    ok "undo restored the original file"
else no "undo did not restore" "$(cat "$T/f.txt")"; fi

echo "── sed -i undo (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
