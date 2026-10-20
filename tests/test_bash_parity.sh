#!/usr/bin/env bash
# test_bash_parity.sh — broad jash-vs-bash parity across common shell constructs.
# Born from the Wave-sweep methodology: each case runs the SAME script under jash
# and /bin/bash and asserts identical output. Self-validating + anti-rot: if a
# future change regresses any construct, this fails. (Constructs where jash is
# intentionally AHEAD of the system's bash 3.2 — e.g. ${V^^} — are excluded.)
# Verified via SCRIPT FILE per the golden rule, never `jash -c` under a jash parent.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; echo "      jash=[$2]"; echo "      bash=[$3]"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
D="$(mktemp -d)"; printf 'l1\nl2\nl3\n' > "$D/lines.txt"

chk(){ # label  script
  printf '%s' "$2" > "$D/p.jsh"
  local j b
  j="$(cd "$D" && "$J" p.jsh 2>/dev/null | strip)"
  b="$(cd "$D" && /bin/bash p.jsh 2>/dev/null)"
  [ "$j" = "$b" ] && ok "$1" || no "$1" "$j" "$b"
}

echo "== jash == bash parity (broad) =="
# brace expansion
chk 'brace range'        'echo {1..5}'
chk 'brace list'         'echo {a,b,c}'
chk 'brace cross'        'echo {a,b}{1,2}'
chk 'brace prefix/suf'   'echo pre{x,y}post'
# arrays
chk 'array index'        'a=(x y z); echo ${a[1]}'
chk 'array all'          'a=(x y z); echo ${a[@]}'
chk 'array length'       'a=(x y z); echo ${#a[@]}'
# arithmetic
chk 'arith pow'          'echo $(( 2 ** 10 ))'
chk 'arith hex'          'echo $(( 0xff ))'
chk 'arith octal'        'echo $(( 010 ))'
chk 'arith pre-incr'     'x=5; echo $(( ++x )); echo $x'
chk 'arith post-incr'    'x=5; echo $(( x++ )); echo $x'
chk 'arith for-loop'     'for ((i=0;i<3;i++)); do echo $i; done'
chk 'arith ternary'      'echo $(( 5 > 3 ? 10 : 20 ))'
# parameter expansion
chk 'global replace'     'v=a.b.a.b; echo ${v//a/X}'
chk 'strip prefix'       'V=foo.bar; echo ${V#*.}'
chk 'strip suffix'       'V=foo.bar; echo ${V%.*}'
chk 'substring offset'   'V=abcdef; echo ${V:2:3}'
chk 'default value'      'echo ${UNSET_X:-fb}'
chk 'indirect var'       'a=b; b=val; echo ${!a}'
# eval / command
chk 'eval basic'         'eval "echo hi"'
chk 'eval from var'      'c="echo ev"; eval $c'
chk 'command -v'         'command -v echo >/dev/null && echo ok'
# control / flow
chk 'set -e stops'       'set -e; false; echo NOPE'
chk 'shift'              'set -- a b c; shift; echo $1'
chk 'negate'             '! false; echo $?'
chk 'or-and precedence'  'false || echo x && echo y'
chk 'until loop'         'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
chk 'case statement'     'case foo in foo) echo m;; *) echo n;; esac'
# read / pipes
chk 'read -r from pipe'  'printf "x y\n" | { read -r a b; echo "$a|$b"; }'
chk 'while read file'    'while read -r ln; do echo "got:$ln"; done < lines.txt'
chk 'regex =~'           '[[ "ab12" =~ [0-9]+ ]] && echo matched'
chk 'printf -v'          'printf -v o "%03d" 7; echo $o'
# quoting / cmdsub / continuation (the bugs fixed this session)
chk 'quoted cmd word'    'CMD=echo; "$CMD" hi'
chk 'backslash cont'     $'echo one \\\ntwo'
chk 'cmdsub keeps NL'    'V=$(printf "a\nb"); echo "[$V]"'
chk 'heredoc'            $'cat <<EOF\nhi $USER\nEOF'
chk 'heredoc <<- tabs'   $'cat <<-EOF\n\tindented\n\tEOF'
chk 'here-string'        'cat <<< "hello world"'

rm -rf "$D"
echo "== bash-parity: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
