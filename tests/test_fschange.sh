#!/usr/bin/env bash
# tests/test_fschange.sh — ADR-0038 W2: files_changed behavior matrix.
#
# After a mutating command, the agent must SEE the real filesystem delta:
# a stderr footer for plain agent sessions, a files_changed object in the
# JSON envelope for JASH_AGENT=1. Pure commands must cost/say nothing.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/probe-home"
jash_usable() {
  /usr/bin/env -u JASH_SESSION -u JASH_AGENT HOME="$T/probe-home" \
    JASH_SIGIL=0 JASH_RECALL_GUARD=0 JASH_RISK_CHECKPOINT=0 \
    "$1" -c 'exit 0' >/dev/null 2>&1
}
jash_usable "$JASH" || JASH="$PROJECT_ROOT/build-speed/jash-release"
if ! jash_usable "$JASH"; then echo "SKIP: no runnable jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"   # cd-proof

mkdir -p "$T/home" "$T/work/sub"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"
printf 'seed\n' > "$T/work/base.txt"
printf 'seed\n' > "$T/work/sub/nested.txt"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# stderr of an agent (non-envelope) session. The scan roots at the cwd the
# shell STARTS in (tool-call granularity) — so set cwd before launching,
# exactly like a real agent harness does.
foot() { ( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
           CLAUDECODE=1 CLAUDE_CODE_SESSION_ID=fs1 JASH_SIGIL=1 \
           JASH_BRIEF=0 JASH_NUDGE=0 \
           "$JASH" -c "$1" 2>&1 >/dev/null ); }

# 1. creation reported
out=$(foot "touch novo.txt")
case "$out" in
  *"files changed"*"+novo.txt"*) ok "creation reported (+novo.txt)";;
  *) err "creation (got '$out')";;
esac
rm -f "$T/work/novo.txt"

# 2. modification reported (nested)
out=$(foot "echo more >> sub/nested.txt")
case "$out" in
  *"files changed"*"~sub/nested.txt"*) ok "nested modification reported";;
  *) err "modification (got '$out')";;
esac

# 3. deletion reported
touch "$T/work/togo.txt"
out=$(foot "rm -f togo.txt")
case "$out" in
  *"files changed"*"-togo.txt"*) ok "deletion reported (-togo.txt)";;
  *) err "deletion (got '$out')";;
esac

# 4. no change -> silent
out=$(foot "true")
case "$out" in
  *"files changed"*) err "no-op still reported ('$out')";;
  *) ok "no change: silent";;
esac

# 5. pure read command -> silent (scan skipped entirely)
out=$(foot "cat base.txt")
case "$out" in
  *"files changed"*) err "pure read reported changes ('$out')";;
  *) ok "pure read: silent";;
esac

# 6. JASH_CHANGES=0 -> silent even when mutating
out=$( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb CLAUDECODE=1 \
      CLAUDE_CODE_SESSION_ID=fs1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      JASH_CHANGES=0 "$JASH" -c "touch off.txt" 2>&1 >/dev/null )
case "$out" in
  *"files changed"*) err "JASH_CHANGES=0 still reported";;
  *) ok "JASH_CHANGES=0: silent";;
esac
rm -f "$T/work/off.txt"

# 7. .git contents excluded
mkdir -p "$T/work/.git"
out=$(foot "touch .git/interno")
case "$out" in
  *"files changed"*) err ".git change reported ('$out')";;
  *) ok ".git contents excluded";;
esac

# 8. non-agent session -> nothing
out=$( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
      JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "touch human.txt" 2>&1 >/dev/null )
case "$out" in
  *"files changed"*) err "non-agent session reported";;
  *) ok "non-agent: silent";;
esac
rm -f "$T/work/human.txt"

# 9. JSON envelope (JASH_AGENT=1) carries files_changed
out=$( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb CLAUDECODE=1 \
      CLAUDE_CODE_SESSION_ID=fs1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      JASH_AGENT=1 "$JASH" -c "touch envfile.txt" 2>&1 >/dev/null )
case "$out" in
  *'"files_changed"'*'"created":["envfile.txt"'*) ok "JSON envelope carries files_changed";;
  *) err "envelope (got '$out')";;
esac
rm -f "$T/work/envfile.txt"

# 10. multiple changes: count + list
out=$(foot "touch a1.txt a2.txt a3.txt")
case "$out" in
  *"files changed (3)"*"+a1.txt"*"+a2.txt"*"+a3.txt"*) ok "multi-change count and list";;
  *) err "multi-change (got '$out')";;
esac
rm -f "$T/work/a1.txt" "$T/work/a2.txt" "$T/work/a3.txt"

# 11. More than the report cap: keep the lexicographically first 24, not an
# arbitrary readdir prefix sorted after truncation.
many=""; i=30
while [[ $i -ge 1 ]]; do many="$many z$(printf '%02d' "$i").txt"; i=$((i-1)); done
out=$(foot "touch$many")
exact=1
case "$out" in *"files changed (30)"*) ;; *) exact=0;; esac
i=1
while [[ $i -le 24 ]]; do
  z="z$(printf '%02d' "$i").txt"
  case "$out" in *"+$z"*) ;; *) exact=0;; esac
  i=$((i+1))
done
while [[ $i -le 30 ]]; do
  z="z$(printf '%02d' "$i").txt"
  case "$out" in *"+$z"*) exact=0;; esac
  i=$((i+1))
done
[[ $exact -eq 1 ]] && ok "capped report keeps exactly first 24 paths" \
                    || err "capped report retained wrong set (got '$out')"
rm -f "$T/work"/z*.txt

# 12. The runtime fixture above still depends on filesystem enumeration order.
# Pin the algorithmic half too: ordering happens during bounded insertion and
# there is no post-truncation sort that could preserve an arbitrary prefix.
if grep -q 'while (pos < out->n && strcmp(out->paths\[pos\], kept) < 0)' \
      "$PROJECT_ROOT/c/core/fschange.c" &&
   ! grep -q 'diff_sort' "$PROJECT_ROOT/c/core/fschange.c"; then
  ok "source keeps bounded diff sorted during insertion"
else
  err "bounded diff reverted to sort-after-truncation"
fi

# 13. Complete paths, not a shared 255-byte display prefix, decide which 24
# entries survive the cap.
long_a=$(printf 'a%.0s' $(seq 1 120))
long_b=$(printf 'b%.0s' $(seq 1 120))
mkdir -p "$T/work/$long_a/$long_b"
many=""; i=30
while [[ $i -ge 1 ]]; do many="$many $long_a/$long_b/z$(printf '%02d' "$i").txt"; i=$((i-1)); done
out=$(foot "touch$many")
exact=1; i=1
while [[ $i -le 24 ]]; do
  z="$long_a/$long_b/z$(printf '%02d' "$i").txt"
  case "$out" in *"+$z"*) ;; *) exact=0;; esac
  i=$((i+1))
done
while [[ $i -le 30 ]]; do
  z="$long_a/$long_b/z$(printf '%02d' "$i").txt"
  case "$out" in *"+$z"*) exact=0;; esac
  i=$((i+1))
done
[[ $exact -eq 1 ]] && ok "capped report sorts complete long paths" \
                    || err "long-path cap used truncated sort keys"
rm -rf "$T/work/$long_a"

echo "═══════════════════════════════════════════════════════════════"
echo "  files_changed (ADR-0038 W2) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ fschange tests PASSED"; exit 0; } \
                  || { echo "  ✗ fschange tests FAILED"; exit 1; }
