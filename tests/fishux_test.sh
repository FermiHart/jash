#!/usr/bin/env bash
# BearJash — Fish UX regression smoke tests (non-interactive pieces)
set -u -o pipefail
JASH="${JASH:-./build/jash}"
pass=0; fail=0

run_jash_c() {
  if [ "$(uname -s)" = "Darwin" ]; then
    arch -x86_64 "$JASH" -c "$1"
  else
    "$JASH" -c "$1"
  fi
}

clean() {
  sed 's/\r//g' | sed $'s/\x1b\[[0-9;]*[A-Za-z]//g' | grep -vE 'fermihart|\$ '
}

run_match() {
  local name="$1" script="$2" pattern="$3"
  local got
  got="$(run_jash_c "$script" 2>&1 | clean)"
  if [[ "$got" =~ $pattern ]]; then
    pass=$((pass+1)); printf '  PASS  %s\n' "$name"
  else
    fail=$((fail+1)); printf '  FAIL  %s\n        pattern=[%s]\n        got    =[%s]\n' "$name" "$pattern" "$got"
  fi
}

echo "== Fish UX — non-interactive smoke tests =="

run_match 'did you mean builtin' 'pritnf nope' "Did you mean 'printf'"
run_match 'did you mean external' 'sllep 1' "Did you mean 'sleep'"
run_match 'abbr builtin set/list' 'abbr g git status; abbr' 'abbr g git status'
run_match 'fish_config show' 'fish_config show' 'autosuggest='
run_match 'fish_config autosuggest off' 'fish_config autosuggest off; fish_config show' 'autosuggest=0'

echo "------------------------------------"
printf "TOTAL: %d pass, %d fail\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
