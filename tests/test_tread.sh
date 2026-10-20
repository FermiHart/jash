#!/usr/bin/env bash
# test_tread.sh — time-travel reads without rewind (ADR-0042 F8).
# Uses the timeline snapshot store that cp now populates before overwriting.
set -u
set -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\[[0-9;]*m//g'; }

W="$ROOT/.tread_w"; rm -rf "$W"; mkdir -p "$W"
TH="$(mktemp -d)"; mkdir -p "$TH/.jash"
bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"
S="$TH/s.jsh"

# ---- helper: run jash and strip ANSI (rc is jash's rc because pipefail) ----
runj(){ JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 HOME="$TH" "$J" "$@" 2>&1 | strip; }

# ---- F8.1: tread reads old content; tdiff shows the change ----
{
  echo "cd $W"
  echo "printf 'old' > g"
  echo "cp g h"
  echo "checkpoint base"
  echo "printf 'new' > f"
  echo "cp f h"
  echo "tread h @base"
  echo "echo MARKER"
  echo "tdiff h @base"
  echo "timeline"
} > "$S"
out="$(runj "$S")"
printf '%s' "$out" | grep -q 'old'                && ok 'tread returns old content' \
  || no 'tread returns old content' "$out"
printf '%s' "$out" | grep -q '^-old$'             && ok 'tdiff shows the old hunk' \
  || no 'tdiff shows old hunk' "$out"
printf '%s' "$out" | grep -q '^+new$'             && ok 'tdiff shows the new hunk' \
  || no 'tdiff shows new hunk' "$out"
printf '%s' "$out" | grep -q 'overwrite'          && ok 'cp overwrites appear in timeline' \
  || no 'cp overwrite not journaled' "$out"

# ---- F8.2: tread without checkpoint returns current content ----
out="$(runj -c "cd $W; tread h")"
printf '%s' "$out" | grep -q '^new$'              && ok 'tread (no checkpoint) reads current file' \
  || no 'tread no checkpoint' "$out"

# ---- F8.3: unknown checkpoint -> error rc 1 ----
runj -c "cd $W; tread h @nosuch" >/dev/null 2>&1
[ $? -eq 1 ]                                       && ok 'unknown checkpoint -> rc 1' \
  || no 'unknown checkpoint rc' "rc=$?"

# ---- F8.4: tdiff without checkpoint -> usage error rc 1 ----
runj -c "cd $W; tdiff h" >/dev/null 2>&1
[ $? -eq 1 ]                                       && ok 'tdiff without checkpoint -> rc 1' \
  || no 'tdiff no checkpoint rc' "rc=$?"

# ---- F8.5: tdiff identical files -> rc 0 ----
{
  echo "cd $W"
  echo "printf 'same' > x"
  echo "cp x y"
  echo "checkpoint same"
  echo "cp x y"
  echo "tdiff y @same"
} > "$S"
runj "$S" >/dev/null 2>&1
[ $? -eq 0 ]                                       && ok 'tdiff identical -> rc 0' \
  || no 'tdiff identical rc' "rc=$?"

# ---- F8.6: file deleted after checkpoint -> no readable backup ----
{
  echo "cd $W"
  echo "printf 'gone' > z"
  echo "cp z w"
  echo "checkpoint del"
  echo "rm w"
  echo "tread w @del"
} > "$S"
out="$(runj "$S")"
printf '%s' "$out" | grep -q 'no readable backup' && ok 'tread of deleted file -> clean error' \
  || no 'tread deleted file error' "$out"

rm -rf "$W" "$TH"
echo "== tread/tdiff (ADR-0042 F8): $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
