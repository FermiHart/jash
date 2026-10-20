#!/usr/bin/env bash
# BearJash — bash compatibility regression suite (Fase 6+)
set -u -o pipefail
JASH="${JASH:-./build/jash}"
pass=0; fail=0

run_jash() {
  if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
    arch -x86_64 "$JASH" "$@"
  else
    "$JASH" "$@"
  fi
}

clean_output() {
  sed 's/\r//g' \
    | sed $'s/\x1b\[[0-9;]*[A-Za-z]//g' \
    | awk '/__JBEGIN__/{f=1;next} /__JEND__/{f=0} f' \
    | grep -vE 'fermihart|\$ |█|╗|║|╚|═|Subsumption|help for|Pid [0-9]'
}

run_out() {
  local name="$1" script="$2" expected="$3"
  local got
  got="$(printf 'echo __JBEGIN__\n%s\necho __JEND__\nexit\n' "$script" \
        | run_jash 2>/dev/null \
        | clean_output)"
  if [ "$got" = "$expected" ]; then
    pass=$((pass+1)); printf '  PASS  %s\n' "$name"
  else
    fail=$((fail+1)); printf '  FAIL  %s\n        expected=[%s]\n        got     =[%s]\n' "$name" "$expected" "$got"
  fi
}

run_match() {
  local name="$1" script="$2" pattern="$3"
  local got
  got="$(printf 'echo __JBEGIN__\n%s\necho __JEND__\nexit\n' "$script" \
        | run_jash 2>/dev/null \
        | clean_output)"
  if [[ "$got" =~ $pattern ]]; then
    pass=$((pass+1)); printf '  PASS  %s\n' "$name"
  else
    fail=$((fail+1)); printf '  FAIL  %s\n        pattern=[%s]\n        got    =[%s]\n' "$name" "$pattern" "$got"
  fi
}

# clean_banner: strip banner, ANSI codes, prompt lines from script output.
clean_banner() {
  sed 's/\r//g' \
    | sed $'s/\x1b\[[0-9;]*[A-Za-z]//g' \
    | sed 's/\x1b\[K//g' \
    | grep -vE 'fermihart|JASH|License|Type help|ring 3|Subsumption|\$ |█|╗|║|╚|═|─' \
    | grep -v '^$' \
    | grep -v '^\.' \
    | grep -v '^:' \
    | grep -v '^\['
}

# run_script_out: run a script as a file (avoids REPL char-by-char issues).
run_script_out() {
  local name="$1" script="$2" expected="$3"
  local tmpf
  tmpf=$(mktemp /tmp/jash_test_XXXXXX.sh)
  printf '%s\n' "$script" > "$tmpf"
  local got
  got="$(run_jash "$tmpf" 2>/dev/null | clean_banner)"
  rm -f "$tmpf"
  if [ "$got" = "$expected" ]; then
    pass=$((pass+1)); printf '  PASS  %s\n' "$name"
  else
    fail=$((fail+1)); printf '  FAIL  %s\n        expected=[%s]\n        got     =[%s]\n' "$name" "$expected" "$got"
  fi
}

echo "== Fase 6+ — bash compatibility regression suite =="

# ── set -- positional parameters ──
run_out   'set -- simple'           'set -- a b c; echo "$1"' 'a'
run_out   'set -- $2'               'set -- a b c; echo "$2"' 'b'
run_out   'set -- $3'               'set -- a b c; echo "$3"' 'c'
run_out   'set -- clear'            'set --; echo "x${1:-none}y"' 'xnoney'
run_out   'set -- quoted args'      'set -- "hello world" foo; echo "$1"' 'hello world'
run_out   'set -- with IFS'         'IFS=:; set -- a b; echo "$*"' 'a:b'

# ── embedded "$@" ──
run_out   'embedded $@ simple'      'set -- a b; echo x"$@"y' 'xa by'
run_out   'embedded $@ quoted arg'  'set -- "hello world" foo; echo "$@"' 'hello world foo'

# ── IFS customization ──
run_out   'IFS join $*'             'IFS=+; set -- a b c; echo "$*"' 'a+b+c'
run_out   'IFS restore default'     'IFS=" "; set -- x y; echo "$*"' 'x y'

# ── BASH_SOURCE array ──
run_out   'BASH_SOURCE scalar'      'echo ${BASH_SOURCE}' 'jash'
run_out   'BASH_SOURCE array'       'echo "${BASH_SOURCE[@]}"' 'jash'
run_out   'BASH_SOURCE indexed'     'echo "${BASH_SOURCE[0]}"' 'jash'

# ── BASH_LINENO array ──
run_out   'BASH_LINENO scalar'      'echo ${BASH_LINENO}' '0'
run_out   'BASH_LINENO array'       'echo "${BASH_LINENO[@]}"' '0'
run_out   'BASH_LINENO indexed'     'echo "${BASH_LINENO[0]}"' '0'
run_out   'LINENO array'            'echo "${LINENO[@]}"' '0'

# ── time keyword ──
run_match 'time simple command'     'time echo hello' 'hello'

# ── trap -l ──
run_match 'trap -l lists signals'   'trap -l' 'INT'

# ── trap DEBUG fires before each command ──
run_script_out 'trap DEBUG fires once'   $'trap "echo DBG" DEBUG\necho hi' $'DBG\nhi'
run_script_out 'trap DEBUG fires 3x'     $'trap "echo DBG" DEBUG\necho a\necho b\necho c' $'DBG\na\nDBG\nb\nDBG\nc'

# ── trap RETURN fires when function returns ──
run_script_out 'trap RETURN on function' $'trap "echo RET" RETURN\nf(){ echo fn; }\nf\necho aft' $'fn\nRET\naft'

# ── trap EXIT fires on exit ──
run_script_out 'trap EXIT fires'         $'trap "echo BYE" EXIT\necho run' $'run\nBYE'

# ── $@ splits, $* joins (quoting) ──
run_out   '$@ splits words'         $'set -- a b c\nfor e in "$@"; do echo X$e; done' $'Xa\nXb\nXc'
run_out   '$* joins words'          'set -- a b c; echo "$*"' 'a b c'

# ── ${var:-default} with set -- ──
run_out   'default with set --'     'set --; echo ${X:-fallback}' 'fallback'
run_out   'no default with set'     'set --; X=ok; echo ${X:-fallback}' 'ok'

# ── echo basics ──
run_out   'echo plain word'         'echo hello' 'hello'
run_out   'echo multiple words'     'echo a b c' 'a b c'
run_out   'echo empty var'          'echo ${UNDEF:-}' ''

# ── variable assignment ──
run_out   'assign and echo'         'X=42; echo $X' '42'
run_out   'readonly var'            'X=ro; readonly X; echo $X' 'ro'

# ── arithmetic ──
run_out   'arith $(( ))'            'echo $((2+3))' '5'
run_out   'arith multiply'          'echo $((6*7))' '42'
run_out   'arith subtract'          'echo $((10-3))' '7'
run_out   'arith modulo'            'echo $((10%3))' '1'
run_out   'let expression'          'let X=2+3; echo $X' '5'

# ── [[ ]] conditional ──
run_out   '[[ string == ]]'         'X=hello; [[ $X == hello ]] && echo yes' 'yes'
run_out   '[[ string != ]]'         'X=hello; [[ $X == world ]] || echo no' 'no'
run_out   '[[ -n nonempty ]]'       'X=hi; [[ -n $X ]] && echo yes' 'yes'
run_out   '[[ numeric -eq ]]'       'X=5; [[ $X -eq 5 ]] && echo yes' 'yes'
run_out   '[[ numeric -gt ]]'       'X=10; [[ $X -gt 5 ]] && echo yes' 'yes'

# ── for loop (script file mode for multi-line) ──
run_script_out 'for in list'        'for i in 1 2 3; do echo $i; done' $'1\n2\n3'
run_script_out 'for c-style'        $'for ((i=0;i<3;i++)); do echo $i; done' $'0\n1\n2'

# ── case statement (script file mode, newline-separated) ──
run_script_out 'case match'         $'X=b\ncase $X in\na) echo A;;\nb) echo B;;\nesac' 'B'
run_script_out 'case fallthrough'   $'X=x\ncase $X in\na|b) echo AB;;\n*) echo def;;\nesac' 'def'

# ── function basics (script file mode, def and call on separate lines) ──
run_script_out 'function echo'      $'f() { echo hello; }\nf' 'hello'
run_script_out 'function with args' $'f() { echo $1 $2; }\nf a b' 'a b'

# ── command substitution ──
run_out   '$() simple'              'echo $(echo hi)' 'hi'

# ── shift ──
run_script_out 'shift positional'   'set -- a b c; shift; echo "$1 $2"' 'b c'
run_script_out 'shift 2'            'set -- a b c; shift 2; echo "$1"' 'c'

# ── declare ──
run_out   'declare -i'              'declare -i X=42; echo $X' '42'
run_out   'declare -u'              'declare -u X=hello; echo $X' 'HELLO'
run_out   'declare -l'              'declare -l Y=WORLD; echo $Y' 'world'

# ── printf ──
run_out   'printf format'           'printf "%s=%d\n" x 42' 'x=42'
run_out   'printf escape'           'printf "a\\nb\n"' $'a\nb'

# ── unset ──
run_out   'unset variable'          'X=hi; unset X; echo ${X:-gone}' 'gone'

# ── arrays ──
run_out   'array assign'            'arr[0]=a; arr[1]=b; echo "${arr[0]} ${arr[1]}"' 'a b'

# ── pushd / popd / dirs (script mode) ──
# Path-independent: bash prints the cwd tilde-compressed; compute the same.
expected_dirs=$(printf '%s' "$PWD" | sed "s|^$HOME|~|")
run_script_out 'dirs shows cwd'    'dirs' "$expected_dirs"

# ── BUG-049: brace/tilde/glob expansion inside a builtin-only $(...) that is
# itself wrapped in outer double quotes must still run (args_need_expansion
# in cmdsub.c's builtin fast-path used to only check for $/backtick, so
# `echo {a,b}` inside the substitution never got brace-expanded). ──
run_out   'brace expansion inside quoted cmdsub' 'echo "$(echo {a,b})"' 'a b'

# ── coproc basic data flow ──
run_out   'coproc bidirectional'    'coproc cat; echo hello >&${COPROC[1]}; read line <&${COPROC[0]}; echo "$line"' 'hello'
# Bash manual: a coproc NAME is only recognized before a COMPOUND command —
# `coproc mycat cat` makes bash run `mycat` as the command word (verified
# against real bash: "mycat: command not found"). Test the correct form.
run_out   'coproc named'           'coproc mycat { cat; }; echo named >&${mycat[1]}; read line <&${mycat[0]}; echo "$line"' 'named'

# ── ADR-0036 W5: param-expansion cache re-enabled with a generation counter
# (env_generation, bumped on every set/unset/append; a cache entry is only
# served if it was stamped with the generation still current). The cache had
# been fully disabled (lookup AND store) since 2026-07-01 for exactly the
# first repro below — any regression here is a real staleness bug, not a
# style nit. ──
run_out   'var-cache: loop var changes each iteration'  'for i in 1 2 3; do echo $i; done' $'1\n2\n3'
run_out   'var-cache: unset then reassign'              'X=foo; echo $X; unset X; X=bar; echo $X' $'foo\nbar'
run_out   'var-cache: value changes without unset'      'Z=1; echo $Z; Z=2; echo $Z; Z=3; echo $Z' $'1\n2\n3'
run_out   'var-cache: repeated read of unchanged var'   'Y=hello; echo $Y; echo $Y; echo $Y' $'hello\nhello\nhello'
run_out   'var-cache: $? excluded, never stale'         'true; echo $?; false; echo $?; true; echo $?' $'0\n1\n0'

echo "------------------------------------"
printf "TOTAL: %d pass, %d fail\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
