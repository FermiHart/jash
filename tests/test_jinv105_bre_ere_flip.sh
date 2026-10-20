#!/usr/bin/env bash
# JINV-105 — a BRE that uses a GNU extension must not be re-read as an ERE.
#
# grep_translate() promotes GNU BRE extensions (\| \+ \? \( \) \{ \}) to bare
# ERE metacharacters and then flips the WHOLE pattern to REG_EXTENDED. That
# flip silently re-interprets every other character, because several
# characters are plain literals in a BRE but metacharacters in an ERE:
#
#     BRE  a+b   matches the three characters "a+b"
#     ERE  a+b   matches "aab"
#
# So `grep 'a+b\|zz'` changed meaning the moment the \| was added: no error,
# exit status 0, just different lines. The literal '(' variant is the loud
# half of the same bug (unbalanced group -> compile error), and -x forces the
# same flip through a different door.
#
# Method: ONE script file holding every case, run under bash (where `grep` is
# the system grep, the reference) and under jash (where `grep` is the builtin),
# then compared line by line. Patterns are single-quoted inside that script so
# they reach grep byte-identically under both shells -- double-quoted `\\` is
# itself under dispute (JINV-098) and would contaminate the comparison.
set -u
JASH="${JASH:-./build/jash}"
[ -x "$JASH" ] || { echo "no jash binary at $JASH"; exit 2; }
JASH_ABS="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

printf 'a+b\naab\nzz\n'  > "$work/plus.txt"
printf 'ab?c\nac\nzz\n'  > "$work/qm.txt"
printf 'a{2}\naa\nzz\n'  > "$work/brace.txt"
printf 'a(b\nxy\nc)d\n'  > "$work/paren.txt"

cat > "$work/cases.sh" <<'CASES'
# Capture through a command substitution, not a pipeline: `cmd | tr` would
# report tr's exit status, and grep's own rc is half the contract (an agent
# branches on it -- 0 match, 1 no match, 2 bad pattern).
run() {
  label="$1"; shift
  out="$("$@" 2>&1)"; rc=$?
  printf '%s :: %s (rc=%s)\n' "$label" "$(printf '%s' "$out" | tr '\n' ' ')" "$rc"
}

# --- the silent half: + ? { } are literals in a BRE -------------------------
run 'plus-literal'    grep 'a+b\|zz'    plus.txt
run 'qmark-literal'   grep 'ab?c\|zz'   qm.txt
run 'brace-literal'   grep 'a{2}\|zz'   brace.txt

# --- the loud half: ( is a literal in a BRE ---------------------------------
run 'paren-mid'       grep 'a(b\|xy'    paren.txt
run 'paren-lead'      grep '(\|xy'      paren.txt
run 'paren-close'     grep 'c)d\|xy'    paren.txt

# --- the same flip through the -x door --------------------------------------
run 'x-anchor'        grep -x 'a+b'     plus.txt

# --- the extension itself must still work -----------------------------------
run 'alternation'     grep 'a+b\|aab'   plus.txt
run 'alt-both-arms'   grep 'zz\|aab'    plus.txt
run 'escaped-group'   grep 'a\(b\)\|zz' paren.txt

# --- -E is a different grammar and must be untouched ------------------------
run 'ere-plus'        grep -E 'a+b|zz'  plus.txt
run 'ere-paren'       grep -E '(a)+b'   plus.txt

# --- bracket expressions: these chars are literal inside [] in BOTH ---------
run 'bracket-plus'    grep 'a[+]b\|zz'  plus.txt
run 'bracket-paren'   grep '[(]\|xy'    paren.txt

# --- no GNU extension present: plain BRE must stay exactly as it was --------
run 'plain-bre-plus'  grep 'a+b'        plus.txt
run 'plain-bre-paren' grep 'a(b'        paren.txt
CASES

( cd "$work" && bash cases.sh ) > "$work/ref.out" 2>&1
( cd "$work" && "$JASH_ABS" cases.sh ) 2>&1 | grep -v 'sigil append blocked' > "$work/jash.out"

pass=0; fail=0
while IFS= read -r refline; do
  label="${refline%% ::*}"
  gotline="$(grep -m1 "^$label ::" "$work/jash.out" || true)"
  if [ "$refline" = "$gotline" ]; then
    printf 'ok    %s\n' "$label"; pass=$((pass+1))
  else
    printf 'FAIL  %s\n        ref  %s\n        jash %s\n' \
      "$label" "${refline#*:: }" "${gotline#*:: }"
    fail=$((fail+1))
  fi
done < "$work/ref.out"

printf '\n=== JINV-105: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
