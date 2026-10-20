#!/usr/bin/env bash
# Differential fuzz: jash's builtin grep vs the system grep, over the BRE/ERE
# grammar boundary. Patterns are read from a file into a variable and passed as
# "$p" so no shell quoting layer can differ between the two runs -- the only
# variable is the regex engine.
set -u
JASH="${JASH:-./build/jash}"
JASH_ABS="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

printf '%s\n' \
  'a+b' 'aab' 'ab' 'b' 'a?b' 'a{2}' 'aa' 'a{2,3}' 'aaa' \
  'a(b)c' 'abc' 'a|b' 'a\b' 'a.b' 'axb' 'a*b' 'a^b' 'a$b' \
  '(x)' 'x' '{y}' 'y' 'a+' '+' '?' '|' 'zz' 'word boundary' \
  'foo123' 'FOO' 'tab	sep' 'a]b' 'a[b' > "$work/corpus.txt"

# Mode-specific POSIX grammar only. GNU BRE extensions and invalid repetition
# stay in JINV-105, which pins Jash rather than a host-dependent oracle.
cat > "$work/pats.txt" <<'PATS'
B	a.b
B	a*b
B	^a.b$
B	a[+]b
B	[[:digit:]]
B	a\{2\}
B	a\(b\)c
B	\(a\)\1
B	[]]
B	[^]]
E	a+b
E	a?b
E	a{2}
E	a(b)c
E	a|b
E	^a+b$
E	[[:alpha:]]+
E	(x|y)
E	a\.b
E	a\*b
PATS

cat > "$work/cases.sh" <<'CASES'
while IFS='	' read -r mode p; do
  if [ "$mode" = B ]; then opts='B Bx Bw Bo'; else opts='E Ex Ew Eo'; fi
  for opt in $opts; do
    case "$opt" in
      B)  out="$(grep "$p" corpus.txt 2>&1)";;
      Bx) out="$(grep -x "$p" corpus.txt 2>&1)";;
      Bw) out="$(grep -w "$p" corpus.txt 2>&1)";;
      Bo) out="$(grep -o "$p" corpus.txt 2>&1)";;
      E)  out="$(grep -E "$p" corpus.txt 2>&1)";;
      Ex) out="$(grep -E -x "$p" corpus.txt 2>&1)";;
      Ew) out="$(grep -E -w "$p" corpus.txt 2>&1)";;
      Eo) out="$(grep -E -o "$p" corpus.txt 2>&1)";;
    esac
    rc=$?
    printf '[%s][%s] %s (rc=%s)\n' "$opt" "$p" "$(printf '%s' "$out" | tr '\n' '|')" "$rc"
  done
done < pats.txt
CASES

( cd "$work" && bash cases.sh ) > "$work/ref.out" 2>&1
( cd "$work" && "$JASH_ABS" cases.sh ) 2>&1 | grep -v 'sigil append blocked' > "$work/jash.out"

# Normalize only the error *wording* (jash and GNU grep phrase errors
# differently); the exit status and the match set still have to agree exactly.
norm() { sed -E 's/(grep|bash): .*\(rc=/ERROR (rc=/' "$1"; }
norm "$work/ref.out"  > "$work/ref.n"
norm "$work/jash.out" > "$work/jash.n"

diff -u "$work/ref.n" "$work/jash.n" > "$work/d.txt" && {
  printf 'PASS — %d pattern/flag combinations agree\n' "$(wc -l < "$work/ref.n")"
  exit 0
}

# Undefined empty/repeated/leading repetition is covered by JINV-105 against
# Jash's own contract, not a host grep whose answer varies across GNU/BSD.
printf 'total %s defined pattern/flag combinations\n' "$(wc -l < "$work/ref.n")"
printf '\nDEFINED-GRAMMAR DIVERGENCE\n'
exit 1
