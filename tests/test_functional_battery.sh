#!/usr/bin/env bash
# tests/test_functional_battery.sh — deep FUNCTIONAL coverage (not speed).
#
# Complements the A-Z battery (breadth) with DEPTH: many edge cases per shell
# feature and per reimplemented coreutil, each asserting exact output. Grouped
# by feature area with a per-group tally so a regression pinpoints its area.
#
# Philosophy: correctness of the POSIX subset + jash's own extensions. Every
# case states the expected output literally (not diffed against bash) — this
# is what jash SHOULD do, pinned.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
PROBE_HOME="$(mktemp -d)"; trap 'rm -rf "$PROBE_HOME"' EXIT
jash_usable() {
  /usr/bin/env -u JASH_SESSION -u JASH_AGENT \
    HOME="$PROBE_HOME" JASH_SIGIL=0 JASH_GUARD_BYPASS=0 \
    JASH_GUARD_BYPASS_STRICT=0 JASH_RECALL_GUARD=0 \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1
}
jash_usable "$JASH" || JASH="$PROJECT_ROOT/build-speed/jash-release"
if ! jash_usable "$JASH"; then echo "SKIP: no runnable jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

# Parity harness: it asserts literal output, so disable the stateful agent
# features (recording, memo, loopguard) that otherwise activate when the suite
# is run inside an agent session (inherited CLAUDE_CODE_SESSION_ID) and can
# prepend advisory lines / accumulate cross-run state. Those have their own
# dedicated tests (test_memo*.sh, test_loopguard.sh).
RUN() { /usr/bin/env -u JASH_SESSION -u JASH_AGENT HOME="$PROBE_HOME" JASH_BIN="$JASH" JASH_SIGIL=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_DRIFT=0 JASH_CHANGES=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_RECALL_GUARD=0 JASH_RISK_CHECKPOINT=0 JASH_GUARD=0 JASH_GUARD_STRICT=0 JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0 "$JASH" -c "$1" 2>&1; }

pass=0 fail=0 gpass=0 gfail=0
GROUP=""
group() { [[ -n "$GROUP" ]] && printf "   %s: %d/%d\n" "$GROUP" "$gpass" "$((gpass+gfail))"; GROUP="$1"; gpass=0; gfail=0; echo "── $1 ──"; }
t() { # name  command  expected
  local name="$1" cmd="$2" exp="$3" got
  got="$(RUN "$cmd")"
  if [[ "$got" == "$exp" ]]; then pass=$((pass+1)); gpass=$((gpass+1))
  else fail=$((fail+1)); gfail=$((gfail+1))
    printf "   ✗ %s\n       cmd: %s\n       want: [%s]\n       got:  [%s]\n" "$name" "$cmd" "$exp" "$got"
  fi
}
trc() { # name  command  expected-status
  local name="$1" cmd="$2" exp="$3" got
  RUN "$cmd" >/dev/null
  got=$?
  if [[ "$got" == "$exp" ]]; then pass=$((pass+1)); gpass=$((gpass+1))
  else fail=$((fail+1)); gfail=$((gfail+1))
    printf "   ✗ %s\n       cmd: %s\n       want rc: %s\n       got rc:  %s\n" "$name" "$cmd" "$exp" "$got"
  fi
}

# ═══ 1. Variable & parameter expansion ═══════════════════════════════════════
group "param expansion"
t "plain var"            'v=hi; echo $v'                    'hi'
t "braced var"           'v=hi; echo ${v}x'                 'hix'
t "default unset"        'echo ${u:-def}'                   'def'
t "default set"          'v=x; echo ${v:-def}'              'x'
t "assign default"       'echo ${u:=set}; echo $u'          $'set\nset'
t "alt value set"        'v=x; echo ${v:+yes}'              'yes'
t "alt value unset"      'echo ${u:+yes}'                   ''
t "length"               'v=hello; echo ${#v}'              '5'
t "remove prefix"        'v=abcabc; echo ${v#a}'            'bcabc'
t "remove prefix greedy" 'v=a.b.c; echo ${v##*.}'          'c'
t "remove suffix"        'v=file.txt; echo ${v%.txt}'       'file'
t "remove suffix greedy" 'v=a.b.c; echo ${v%%.*}'          'a'
t "replace first"        'v=aXaXa; echo ${v/X/_}'           'a_aXa'
t "replace all"          'v=aXaXa; echo ${v//X/_}'          'a_a_a'
t "substring"            'v=abcdef; echo ${v:2:3}'          'cde'
t "substring to end"     'v=abcdef; echo ${v:3}'            'def'

# ═══ 2. Special parameters ═══════════════════════════════════════════════════
group "special params"
t "exit status ok"       'true; echo $?'                    '0'
t "exit status fail"     'false; echo $?'                   '1'
t "arg count"            'set -- a b c; echo $#'            '3'
t "all args"             'set -- a b c; echo $@'            'a b c'
t "positional"           'set -- x y z; echo $2'            'y'
t "shift"                'set -- a b c; shift; echo $1'     'b'
t "shift 2"              'set -- a b c d; shift 2; echo $1' 'c'

# ═══ 3. Arithmetic ═══════════════════════════════════════════════════════════
group "arithmetic"
t "add"                  'echo $((2+3))'                    '5'
t "precedence"           'echo $((2+3*4))'                  '14'
t "parens"               'echo $(((2+3)*4))'                '20'
t "division"             'echo $((17/5))'                   '3'
t "modulo"               'echo $((17%5))'                   '2'
t "negative"             'echo $((3-10))'                   '-7'
t "comparison true"      'echo $((5>3))'                    '1'
t "comparison false"     'echo $((5<3))'                    '0'
t "logical and"          'echo $((1&&0))'                   '0'
t "logical or"           'echo $((1||0))'                   '1'
t "bitwise and"          'echo $((12&10))'                  '8'
t "bitwise or"           'echo $((12|10))'                  '14'
t "shift left"           'echo $((1<<4))'                   '16'
t "hex literal"          'echo $((0x1f))'                   '31'
t "var in arith"         'n=6; echo $((n*7))'               '42'
t "arith command"        'x=0; ((x=5+5)); echo $x'          '10'
t "ternary"              'echo $((1?42:0))'                 '42'
t "arith suffix command" 'echo $((1)); echo suffix'         $'1\nsuffix'
t "arith adjacent suffix" 'echo $((1))x'                    '1x'
t "arith redirection"    'd=$(mktemp -d); echo $((2)) > $d/o; cat $d/o; rm -rf $d' '2'
trc "arith divzero status" 'echo $((1/0))' '1'
trc "nested param keeps error" 'unset _fatal; echo "${_fatal?required}${HOME:-x}"' '1'

# ═══ 4. Command substitution ═════════════════════════════════════════════════
group "command substitution"
t "dollar-paren"         'echo $(echo hi)'                  'hi'
t "backtick"             'echo `echo hi`'                   'hi'
t "nested"               'echo $(echo $(echo deep))'        'deep'
t "in double quotes"     'echo "[$(echo x)]"'              '[x]'
t "assign from cmdsub"   'v=$(echo abc); echo $v'           'abc'
t "cmdsub with pipe"     'echo $(echo hello | tr a-z A-Z)' 'HELLO'
t "arith in cmdsub"      'echo $(echo $((6*7)))'            '42'
t "cmdsub alias isolated" 'alias keep="echo outer"; x=$(alias keep="echo inner"); keep' 'outer'
t "cmdsub shopt isolated" 'shopt -u nullglob; x=$(shopt -s nullglob); echo nomatch-cmdsub-*' 'nomatch-cmdsub-*'
t "cmdsub umask isolated" 'before=$(umask); x=$(umask 077); after=$(umask); [ "$before" = "$after" ]; echo $?' '0'
t "cmdsub cache cwd collision" 'd=$(mktemp -d); cd $d; for i in $(seq 0 256); do mkdir d$i; cd d$i; x=$(pwd); cd ..; done; bad=0; for i in $(seq 0 256); do cd d$i; x=$(pwd); case "$x" in */d$i) ;; *) bad=$((bad+1));; esac; cd ..; done; cd /; /bin/rm -rf $d; echo $bad' '0'
t "cmdsub printf-v isolated" 'unset x; y=$(printf -v x value); echo ${x:-unset}' 'unset'
t "cmdsub cache ignores fake PWD" 'd=$(mktemp -d); mkdir $d/a $d/b; cd $d/a; PWD=same; a=$(pwd); cd $d/b; PWD=same; b=$(pwd); cd /; /bin/rm -rf $d; basename $a; basename $b' $'a\nb'
t "cmdsub flushes stdio builtin" 'x=$(set -o); echo ${#x}' '4'

# ═══ 5. Quoting ══════════════════════════════════════════════════════════════
group "quoting"
t "single literal"       "echo 'a\$b'"                      'a$b'
t "double expand"        'v=x; echo "val=$v"'               'val=x'
t "single no expand"     "v=x; echo '\$v'"                  '$v'
t "escaped dollar"       'echo "\$5.00"'                    '$5.00'
t "escaped quote in dq"  'echo "she said \"hi\""'           'she said "hi"'
t "sq inside dq"         "echo \"it's ok\""                 "it's ok"
t "ansi-c newline"       "printf '%s' \$'a\\nb'"            $'a\nb'
t "ansi-c tab"           "printf '%s' \$'x\\ty'"            $'x\ty'
t "adjacent quotes"      'echo "a"b'"'"'c'"'"''             'abc'

# ═══ 6. Redirects & here-docs ════════════════════════════════════════════════
group "redirects"
t "to file and back"     'echo data > "$HOME/_fb.$$"; cat "$HOME/_fb.$$"; rm -f "$HOME/_fb.$$"'  'data'
t "append"               'echo a > "$HOME/_fa.$$"; echo b >> "$HOME/_fa.$$"; cat "$HOME/_fa.$$"; rm -f "$HOME/_fa.$$"'  $'a\nb'
t "stderr to stdout"     'ls /nonexistent_xyz 2>&1 | grep -c . | tr -d " "' '1'
t "discard stderr"       'ls /nonexistent_xyz 2>/dev/null; echo done'      'done'
t "here-string"          'cat <<< "piped in"'               'piped in'
t "here-doc"             'cat <<EOF
line1
line2
EOF'                                                        $'line1\nline2'
t "here-doc expand"      'v=X; cat <<EOF
val=$v
EOF'                                                        'val=X'
t "here-doc no expand"   'v=X; cat <<'"'"'EOF'"'"'
val=$v
EOF'                                                        'val=$v'
# BUG-164: a redirect/heredoc feeding a BUILTIN inside $() must apply the
# redirect (the cmdsub builtin fast-path used to leak a stray '<' into argv).
t "builtin < in cmdsub"  'd=$(mktemp -d);printf "hi\nbye\n">$d/f;x=$(grep hi < $d/f);echo "[$x]";rm -rf $d'  '[hi]'
t "builtin heredoc cmdsub" $'x=$(grep -c hi <<HD\nhi\nhi\nbye\nHD\n)\necho "n=$x"'  'n=2'
t "read heredoc in cmdsub" $'v=$(read x <<HD\nhello world\nHD\necho "$x"); echo "[$v]"'  '[hello world]'

# ═══ 7. Pipelines & lists ════════════════════════════════════════════════════
group "pipelines & lists"
t "simple pipe"          'echo hello | tr a-z A-Z'          'HELLO'
t "three-stage pipe"     'echo -e "c\\nb\\na" | sort | head -1' 'a'
t "and success"          'true && echo yes'                 'yes'
t "and skip on fail"     'false && echo no; echo after'     'after'
t "or on fail"           'false || echo recovered'          'recovered'
t "or skip on success"   'true || echo no; echo after'      'after'
t "semicolon seq"        'echo a; echo b'                   $'a\nb'
t "chain mixed"          'true && echo 1 || echo 2'         '1'
t "chain mixed fail"     'false && echo 1 || echo 2'        '2'
# BUG-157: external cmd as last pipe segment, repeated after other statements.
# Uninitialized is_function/is_brace_group in seg_t made the 2nd/3rd such
# pipeline route the external into the builtin path (NULL builtin) → SIGSEGV.
t "ext-lastpipe repeat"  'echo a | head -1; echo mid; echo b | head -1'  $'a\nmid\nb'
t "ext-lastpipe chain4"  'echo a|head -1;echo b|head -1;echo c|head -1;echo d|tail -1'  $'a\nb\nc\nd'

# ═══ 8. Loops ════════════════════════════════════════════════════════════════
group "loops"
t "for in list"          'for x in a b c; do echo $x; done' $'a\nb\nc'
t "for with seq"         'for i in $(seq 1 3); do echo $i; done'  $'1\n2\n3'
t "while countdown"      'i=3; while [ $i -gt 0 ]; do echo $i; i=$((i-1)); done' $'3\n2\n1'
t "until"                'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done' $'0\n1'
t "break"                'for i in 1 2 3 4; do [ $i -eq 3 ] && break; echo $i; done' $'1\n2'
t "continue"             'for i in 1 2 3; do [ $i -eq 2 ] && continue; echo $i; done' $'1\n3'
t "for C-style"          'for ((i=0;i<3;i++)); do echo $i; done'  $'0\n1\n2'
t "nested loop"          'for a in 1 2; do for b in x y; do echo $a$b; done; done' $'1x\n1y\n2x\n2y'
t "loop accumulate"      's=0; for n in 1 2 3 4; do s=$((s+n)); done; echo $s' '10'

# ═══ 9. Conditionals ═════════════════════════════════════════════════════════
group "conditionals"
t "if true"              'if true; then echo yes; fi'       'yes'
t "if else"              'if false; then echo a; else echo b; fi' 'b'
t "elif"                 'x=2; if [ $x -eq 1 ]; then echo one; elif [ $x -eq 2 ]; then echo two; fi' 'two'
t "string equal"         '[ abc = abc ] && echo eq'         'eq'
t "string not equal"     '[ abc != xyz ] && echo ne'        'ne'
t "numeric lt"           '[ 3 -lt 5 ] && echo lt'           'lt'
t "numeric ge"           '[ 5 -ge 5 ] && echo ge'           'ge'
t "empty string"         '[ -z "" ] && echo empty'          'empty'
t "nonempty string"      '[ -n "x" ] && echo nonempty'      'nonempty'
t "file exists"          'touch "$HOME/_fe.$$"; [ -f "$HOME/_fe.$$" ] && echo yes; rm -f "$HOME/_fe.$$"' 'yes'
t "negation"             '[ ! -f /nope_xyz_$$ ] && echo absent'  'absent'
t "case match"           'x=b; case $x in a) echo A;; b) echo B;; esac'  'B'
t "case wildcard"        'x=hello; case $x in h*) echo H;; *) echo other;; esac'  'H'
t "case default"         'x=z; case $x in a) echo A;; *) echo def;; esac'  'def'
t "[[ ]] equal"          '[[ abc == abc ]] && echo yes'     'yes'
t "[[ ]] pattern"        '[[ hello == h* ]] && echo match'  'match'

# ═══ 10. Functions ═══════════════════════════════════════════════════════════
group "functions"
t "define and call"      'f() { echo called; }; f'          'called'
t "function args"        'f() { echo "$1-$2"; }; f a b'     'a-b'
t "function return code" 'f() { return 3; }; f; echo $?'    '3'
t "function output used" 'f() { echo 42; }; x=$(f); echo $x'  '42'
t "recursion"            'fac() { if [ $1 -le 1 ]; then echo 1; else echo $(($1*$(fac $(($1-1))))); fi; }; fac 5'  '120'
t "local scope"          'f() { local x=inner; echo $x; }; x=outer; f; echo $x'  $'inner\nouter'
t "function param count" 'f() { echo $#; }; f a b c'        '3'
t "caller args restored" 'set -- outer1 "" outer3; f() { set -- inner; :; }; f arg; printf "%s|%s|%s|%s\n" "$#" "$1" "$2" "$3"' '3|outer1||outer3'
t "redir caller args restored" 'set -- outer1 "" outer3; f() { set -- inner; :; }; f arg >/dev/null; printf "%s|%s|%s|%s\n" "$#" "$1" "$2" "$3"' '3|outer1||outer3'
t "nested list compose"  'f() { echo "f:$1"; }; for i in 1 2 3; do false && f no || f "$(echo "$i")"; done' $'f:1\nf:2\nf:3'

# ═══ 11. Glob & brace expansion ══════════════════════════════════════════════
group "glob & brace"
t "brace list"           'echo {a,b,c}'                     'a b c'
t "brace range"          'echo {1..5}'                      '1 2 3 4 5'
t "brace prefix"         'echo pre{X,Y}post'                'preXpost preYpost'
t "brace range step"     'echo {0..10..2}'                  '0 2 4 6 8 10'
t "brace mixed literal"  'echo {a..3}'                      '{a..3}'   # BUG-142
t "brace mixed literal2" 'echo {1..c}'                      '{1..c}'
t "brace incomplete lit" 'echo {a..}'                       '{a..}'    # BUG-143
t "brace escaped braces" 'echo \{a,b\}'                     '{a,b}'    # BUG-144
t "brace escaped comma"  'echo {a\,b,c}'                    'a,b c'    # BUG-145
t "brace space literal"  'echo {a, b}'                      '{a, b}'   # BUG-146
t "brace empty element"  'echo {a,,b}'                      'a b'
t "brace empty affix"    'echo x{a,,b}y'                    'xay xy xby'
t "brace alpha step lit" 'echo {a..e..2}'                   '{a..e..2}'
t "array brace literal"  'v=({a,b}); echo "${v[@]}" ${#v[@]}'  'a b 2'   # BUG-147
t "array brace affix"    'v=(pre{1,2}); echo "${v[@]}"'          'pre1 pre2'
t "array brace range"    'v=({1..3}); echo ${#v[@]}'             '3'
t "array brace quoted"   'v=("{a,b}"); echo ${#v[@]}'            '1'
t "array brace mixed"    'v=(x {a,b} y); echo ${#v[@]}'          '4'
t "base gt36 upper A"    'echo $((37#A))'                        '36'   # BUG-130
t "base 64 upper Z"      'echo $((64#Z))'                        '61'
t "base le36 caseins"    'echo $((16#FF))'                       '255'
t "base gt36 lower a"    'echo $((37#a))'                        '10'
t "glob star"            'cd /tmp && mkdir -p _gl.$$ && cd _gl.$$ && touch a.c b.c d.h && echo *.c && cd .. && rm -rf _gl.$$'  'a.c b.c'
t "glob question"        'cd /tmp && mkdir -p _gq.$$ && cd _gq.$$ && touch a1 a2 bb && echo a? && cd .. && rm -rf _gq.$$'  'a1 a2'
t "glob no match literal" 'echo /nonexistent_dir_xyz_qwerty/*'  '/nonexistent_dir_xyz_qwerty/*'

# ═══ 12. Reimplemented coreutils (correctness) ═══════════════════════════════
group "coreutils: grep"
t "grep basic"           'printf "apple\\nbanana\\ncherry\\n" | grep an'  'banana'
t "grep count"           'printf "a\\nab\\nabc\\n" | grep -c a | tr -d " "'  '3'
t "grep invert"          'printf "a\\nb\\nc\\n" | grep -v b'   $'a\nc'
t "grep ignore case"     'printf "Hello\\nWORLD\\n" | grep -i world'  'WORLD'
t "grep line number"     'printf "x\\ny\\nz\\n" | grep -n y'   '2:y'
t "grep extended"        'printf "cat\\ndog\\n" | grep -E "c.t"'  'cat'
t "grep word"            'printf "the theater\\n" | grep -wo the'  'the'
# ADR-0044 grep parity: PCRE-lite (-P) and pattern-source flags (-e, --).
t "grep -oP digit class" 'printf "id 12345 x\\n" | grep -oP "\\d+"'     '12345'   # the reported -oP failure
t "grep -P dot decimal"  'printf "v1.5 here\\n" | grep -oP "\\d+\\.\\d+"'  '1.5'
t "grep -P quantifier"   'printf "12 345 6789\\n" | grep -oP "\\d{3}"'  $'345\n678'
t "grep -oP word class"  'printf "foo bar\\n" | grep -oP "\\w+" | head -1'  'foo'
t "grep -e dash pattern" $'printf "%s\\n" "-v x" "plain" | grep -e "-v"'  '-v x'   # pattern starting with -
t "grep -- end options"  $'printf "%s\\n" "has -c in" "other" | grep -- "-c"'  'has -c in'
# ADR-0044 grep pass 2: --exclude-dir (flooder), -m, -L.
t "grep -m max-count"    'd=$(mktemp -d);printf "x\nx\nx\nx\n">$d/f;grep -m 2 x $d/f|wc -l|tr -d " ";rm -rf $d'  '2'
t "grep --exclude-dir"   'd=$(mktemp -d);mkdir -p $d/s $d/node_modules;echo hit>$d/s/a;echo hit>$d/node_modules/b;grep -rl --exclude-dir=node_modules hit $d 2>/dev/null|grep -c node_modules;rm -rf $d'  '0'
t "grep -L no-match"     'd=$(mktemp -d);echo hit>$d/a;echo no>$d/b;grep -L hit $d/a $d/b|grep -c "b$";rm -rf $d'  '1'

group "coreutils: wc/head/tail/sort/uniq"
t "wc lines"             'printf "a\\nb\\nc\\n" | wc -l | tr -d " "'  '3'
t "wc words"             'echo "one two three" | wc -w | tr -d " "'   '3'
t "head n"               'seq 1 10 | head -3'                $'1\n2\n3'
t "tail n"               'seq 1 10 | tail -2'                $'9\n10'
t "sort numeric"         'printf "10\\n2\\n1\\n" | sort -n'   $'1\n2\n10'
t "sort reverse"         'printf "a\\nc\\nb\\n" | sort -r'    $'c\nb\na'
t "uniq"                 'printf "a\\na\\nb\\nb\\nb\\nc\\n" | uniq'  $'a\nb\nc'
t "uniq count"           'printf "a\\na\\nb\\n" | uniq -c | tr -s " " | sed "s/^ //"'  $'2 a\n1 b'

group "coreutils: tr/cut/seq/echo/printf"
t "tr translate"         'echo hello | tr l L'              'heLLo'
t "tr delete"            'echo "a1b2c3" | tr -d 0-9'        'abc'
t "tr squeeze"           'echo "aaabbb" | tr -s a'          'abbb'
t "cut field"            'echo "a:b:c" | cut -d: -f2'       'b'
t "cut range"            'echo "a:b:c:d" | cut -d: -f2-3'   'b:c'
t "seq range"            'seq 3 5'                          $'3\n4\n5'
t "seq step"             'seq 0 2 6'                        $'0\n2\n4\n6'
t "echo -n"              'echo -n nonewline; echo X'        'nonewlineX'
t "printf integer"       'printf "%d\\n" 42'                '42'
t "printf padded"        'printf "%03d\\n" 7'               '007'
t "printf float"         'printf "%.2f\\n" 3.14159'         '3.14'
t "printf string"        'printf "[%s]\\n" hi'              '[hi]'
t "printf multiple"      'printf "%s-%s\\n" a b'            'a-b'
t "printf unknown esc"   'printf "x\\yz"'                   'x\yz'        # BUG-155
t "printf octal nolead"  'printf "\\101\\102"'             'AB'          # BUG-158
t "printf octal 3digit"  'printf "\\1011"'                 'A1'          # BUG-158
t "printf octal lead0"   'printf "\\0101"'                 $'\b1'        # BUG-158
t "printf b octal"       'printf "%b" "\\101-\\41"'        'A-!'         # BUG-159
t "printf b unknown esc" 'printf "%b" "a\\yb"'             'a\yb'        # BUG-159
t "printf esc tab keep"  'printf "a\\tb\\n"'               $'a\tb'       # known escapes intact
t "printf -- end opts"   'printf -- "%s\\n" foo'           'foo'         # BUG-160
t "printf -- dash fmt"   'printf -- "-n\\n"'               '-n'          # BUG-160 (format starting with -)

# ═══ 13. Builtins: control & env ═════════════════════════════════════════════
group "control builtins"
t "export visible"       'export FOO=bar; sh -c "echo \$FOO"'  'bar'
t "unset"                'v=x; unset v; echo "[${v:-gone}]"'  '[gone]'
t "alias"                'alias g="echo aliased"; g'        'aliased'
t "type builtin"         'type echo'                        'echo is a shell builtin'
t "read from heredoc"    'read a b <<< "one two"; echo "$b-$a"'  'two-one'
t "true false"           'true; echo $?; false; echo $?'    $'0\n1'
t "eval"                 'eval "echo evaluated"'            'evaluated'
t "cmd substitution set" 'set -- $(echo x y z); echo $#'    '3'
t "pick projects json"   'printf "{\"name\":\"a\",\"size\":2}\n" | pick name' '{"name":"a"}'
t "compgen builtin inventory" 'compgen -A builtin | grep -q "^echo$"; echo $?' '0'
t "jash_state clear"     'jash_state clear'                  'state cleared'
t "plugin empty list"    'plugin list'                       'No plugins loaded.'
t "statusbar help contract" 'statusbar help | grep -c "statusbar auto on|off"' '1'

# ═══ 14. test operators (unary/binary/connectives) ══════════════════════════
group "test operators"
t "test -d dir"          '[ -d /tmp ] && echo isdir'        'isdir'
t "test -e exists"       '[ -e /tmp ] && echo exists'       'exists'
t "test -a AND binary"   '[ 1 -eq 1 -a 2 -eq 2 ] && echo both'  'both'   # BUG-071
t "test -o OR binary"    '[ 1 -eq 9 -o 2 -eq 2 ] && echo either' 'either' # BUG-071
t "test -a string"       '[ a = a -a b = b ] && echo eqand' 'eqand'
t "test -a false"        '[ 1 -eq 1 -a 1 -eq 2 ] || echo notboth'  'notboth'
t "test -o both false"   '[ 1 -eq 2 -o 3 -eq 4 ] || echo neither'  'neither'
t "test negate binary"   '[ ! 1 -eq 2 ] && echo ne'         'ne'
t "test empty is false"  '[ "" ] || echo emptyfalse'        'emptyfalse'
t "test string as [["    '[[ -n abc && -n xyz ]] && echo both2'  'both2'

# ═══ 14b. conformance-hunt regressions (jash↔bash) ═══════════════════════════
group "conformance regressions"
t "exit in cmdsub no kill"  'x=$(exit 3); echo alive'         'alive'   # BUG-074a (safety)
t "cmdsub survives exit"    'y=$(exit 0); echo after'         'after'
t "bare return exit status" 'f(){ false; return; }; f; echo $?'  '1'    # BUG-084
t "return preserves rc2"    'g(){ (exit 7); return; }; g; echo $?'  '7'
t "cmdsub paren in quotes"  'echo "$(echo "a)b")"'            'a)b'     # BUG-075
t "cmdsub squote literal"   "echo \"\$(echo 'sq')\""          'sq'      # BUG-075
t "unset clears array"      'a=(x y z); unset a; echo ${#a[@]}'  '0'    # BUG-078
t "array elem arithmetic"   'a=(10 20 30); echo $((a[1]+a[2]))'  '50'   # BUG-079
t "array arith single"      'a=(5 6 7); echo $((a[0]*a[2]))'   '35'
t "squote in array lit"     "v=hi; a=('lit\$v'); echo \${a[0]}"  'lit$v' # BUG-080
t "cmdsub in array lit"     'a=($(echo 1 2 3)); echo ${#a[@]}:${a[0]}:${a[2]}'  '3:1:3' # BUG-081
t "cmdsub array elem count" 'a=($(echo one two)); echo ${a[1]}'  'two'
t "cmdsub assign no leak"   'v=orig; x=$(v=changed); echo $v'  'orig'   # BUG-074b
t "cmdsub cd no leak"       'cd /tmp; x=$(cd /usr); echo $PWD | grep -c usr'  '0' # BUG-074c
t "cmdsub \$? from false"   'x=$(false); echo $?'              '1'      # BUG-074d
t "cmdsub \$? from exit"    'x=$(exit 4); echo $?'             '4'
t "cmdsub \$? plain assign" 'x=hi; echo $?'                    '0'
t "multiline func def"      $'f()\n{\n echo ml\n}\nf'          'ml'     # BUG-083
t "multiline func brace nl" $'greet()\n{\n echo hi\n}\ngreet' 'hi'
t "func quoted arg one"     'f(){ echo "[$1]"; }; f "a b"'    '[a b]'  # BUG-082
t "func quoted args count"  'f(){ echo $#; }; f "a b" c'      '2'
t "func quoted two"         'f(){ echo "$1|$2"; }; f "a b" "c d"'  'a b|c d'
t "func mixed quote"        'f(){ echo "$1"; }; f x'"'"'y z'"'"'w'  'xy zw'
t "array @ word split"      'a=("a b" "c d"); set -- "${a[@]}"; echo "$#:$1:$2"'  '2:a b:c d'  # BUG-077
t "array @ empty zero"      'a=(); set -- "${a[@]}"; echo $#'  '0'
t "bare array is elem0"     'a=(x y z); echo "$a"'            'x'
t "bare array braced elem0" 'a=(p q r); echo "${a}"'          'p'
t "array @ count preserved" 'a=(one "two three"); set -- "${a[@]}"; echo $#'  '2'
t "cmdsub cache not stale"  'i=0; a=$(echo $i); i=5; b=$(echo $i); echo "$a$b"'  '05'  # BUG-086
t "cmdsub-cond loop ends"   'i=0; while [ "$(echo $i)" -lt 3 ]; do i=$((i+1)); done; echo $i'  '3'
t "oneline if no truncate"  $'echo one\nif false; then :; else echo e; fi\necho two'  $'one\ne\ntwo'  # BUG-087
t "oneline case no swallow" $'case x in x) echo cx;; esac\necho after'  $'cx\nafter'
t "case paren pattern"      'case x in (x) echo cp;; esac'    'cp'     # BUG-088
t "bare exit inherits rc"   'x=$(false; exit); echo $?'       '1'      # BUG-091
t "exit 300 masks 8bit"     'x=$(exit 300); echo $?'          '44'     # BUG-092
t "cmdsub file read var"    'printf hi > "$HOME/_b101"; f="$HOME/_b101"; echo "$(<$f)"; rm -f "$HOME/_b101"'  'hi'  # BUG-101
t "len no-subscript array"  'a=(alpha beta); echo ${#a}'      '5'      # BUG-085
t "pattern var prefix"      'x=abc/def; b=abc; echo ${x#$b/}' 'def'    # BUG-093
t "pattern var replace"     'x=abc/def; b=abc; echo ${x/$b/X}'  'X/def'
t "pattern unset var"       'x=hello; echo "${x#$_unsetv}"'   'hello'
t "backslash in dq cmdsub"  'echo "$(echo \her\e)"'           'here'   # BUG-097
t "nested backticks"        'echo `echo outer \`echo inner\` done`'  'outer inner done'  # BUG-098
t "comment in cmdsub"       $'echo "$(echo hi # c with ) paren\n)"'  'hi'  # BUG-100
t "array reassign clears"   'a=(9 8 7); a=(1 2); echo ${#a[@]}'  '2'  # BUG-105
t "array+=string elem0"     'a=(x y); a+=z; echo ${a[0]}:${#a[@]}'  'xz:2'  # BUG-106
t "arith subscript in lit"  'i=2; a=([i]=x [i+1]=y); echo ${a[2]}${a[3]}'  'xy'  # BUG-107
t "assoc literal key"       'declare -A m=([foo]=7); echo ${m[foo]}'  '7'
t "array default operator"  'unset _zz; echo "${_zz[@]:-EMPTY}"'  'EMPTY'  # BUG-110
t "multiline array literal" $'a=(x\ny z\n)\necho ${#a[@]}:${a[2]}'  '3:z'  # BUG-089
t "multiline array comment" $'a=(one #two\nfour)\necho ${#a[@]}:${a[1]}'  '2:four'
t "arith postfix array"     'a=(5 6 7); ((a[0]++)); echo ${a[0]}'  '6'   # BUG-103
t "arith assign array"      'a=(5 6 7); (( a[2] = 99 )); echo ${a[2]}'  '99'
t "arith compound array"    'a=(5 6 7); (( a[0] += 10 )); echo ${a[0]}'  '15'
t "arith prefix array var"  'a=(5 6 7); i=0; ((++a[i])); echo ${a[0]}'  '6'
t "arith len array count"   'a=(2 4 8); echo $(( ${#a[@]} ))'     '3'    # BUG-104
t "arith len scalar"        'b=hello; echo $(( ${#b} + 1 ))'      '6'
t "arith last elem idiom"   'a=(2 4 8); echo $(( a[${#a[@]}-1] ))'  '8'
t "cmdsub quote lit split"  'a=($(echo "x y z")); echo ${#a[@]}'  '3'    # BUG-109
t "cmdsub sq lit split"     $'a=($(printf \'p q\'))\necho ${#a[@]}'  '2'
t "quoted cmdsub one word"  'q=("$(echo x y z)"); echo ${#q[@]}'  '1'
t "empty for no phantom"    $'n=0\nfor w in; do n=$((n+1)); done\necho $n'  '0'  # BUG-111
t "arith left shift"        'echo $((2<<3))'                  '16'     # BUG-113
t "arith shift assign"      'z=2; ((z<<=3)); echo $z'         '16'
t "arith right shift"       'echo $((16>>2))'                 '4'
t "heredoc still works"     $'cat <<EOF\nhd ok\nEOF'          'hd ok'
t "positionals past 9"      'f(){ echo "$#:${10}:${11}"; }; f 1 2 3 4 5 6 7 8 9 ten eleven'  '11:ten:eleven'  # BUG-095
t "set -- twelve args"      'set -- a b c d e f g h i j k l; echo "$#:${12}"'  '12:l'
t "384 positionals exact"   'set -- $(seq 1 384); echo "$#:${384}:${300}:${10}"' '384:384:300:10'
t "384 unquoted at"         'set -- $(seq 1 384); set -- $@; echo "$#:${384}"' '384:384'
t "384 unquoted star"       'set -- $(seq 1 384); set -- $*; echo "$#:${384}"' '384:384'
t "384 quoted at"           'set -- $(seq 1 384); n=0; for x in "$@"; do n=$((n+1)); done; echo "$n:$x"' '384:384'
t "384 quoted star"         'set -- $(seq 1 384); v="$*"; echo ${#v}' '1427'
t "384 adjacent at"         'set -- $(seq 1 384); set -- "pre$@post"; echo "$#:$1:${384}"' '384:pre1:384post'
t "positional slice 300+"   'set -- $(seq 1 384); set -- ${@:300:85}; echo "$#:$1:${85}"' '85:300:384'
t "positional modifier"     'set -- $(seq 1 384); set -- ${@:-fallback}; echo "$#:${384}"' '384:384'
t "385 positionals refused" 'set -- old; set -- $(seq 1 385) 2>/dev/null; echo "$?:$#:$1"' '1:1:old'
# Full shell-state capacity is independent of the checked external-exec envp
# ceiling. Build the operands without spawning a child after adding 650 vars.
t "384 with 650 variables"  'words=; i=1; while [ $i -le 384 ]; do words="$words $i"; i=$((i+1)); done; i=1; while [ $i -le 650 ]; do eval "v$i=x"; i=$((i+1)); done; set -- $words; echo "$#:${384}"' '384:384'
t "function 385 refused"     'f(){ echo must-not-run; }; f $(seq 1 385); echo $?' $'jash: f: more than 384 positional parameters — refusing\n1'
t "redirect function 385 refused" 'f(){ echo must-not-run; }; f $(seq 1 385) 2>/dev/null; echo $?' '1'
pipeline_args_385=$(seq -s ' ' 1 385)
t "pipeline function 385 refused" "f(){ echo must-not-run; }; out=\$(f $pipeline_args_385 | cat); echo \"[\$out]\"" $'jash: pipeline function: more than 384 positional parameters — refusing\n[]'
t "script 384 accepted"      'd=$(mktemp -d); printf '\''echo "$#:${384}"\n'\'' > $d/s; $JASH_BIN $d/s $(seq 1 384); rc=$?; /bin/rm -rf $d; exit $rc' '384:384'
t "script 385 refused"       'd=$(mktemp -d); printf '\''echo must-not-run\n'\'' > $d/s; $JASH_BIN $d/s $(seq 1 385) >/dev/null 2>&1; rc=$?; /bin/rm -rf $d; echo $rc' '2'
long_func_word=$(printf 'y%.0s' $(seq 1 9000))
t "function one 9k arg"       "f(){ echo \"\$#:\${#1}\"; }; f \"$long_func_word\"" '1:9000'
t "redirect function one 9k arg" "d=\$(mktemp -d); f(){ echo \"\$#:\${#1}\"; }; f \"$long_func_word\" > \$d/o; cat \$d/o; /bin/rm -rf \$d" '1:9000'
t "pipeline function one 9k arg" "f(){ echo \"\$#:\${#1}\"; }; f \"$long_func_word\" | cat" '1:9000'
# Prepare external-generator output before filling the persisted shell state;
# the saved values and positional count remain the actual assertions below.
t "session persists full env" 'd=$(mktemp -d); HOME=$d JASH_SESSION=large $JASH_BIN -c '\''BIG=$(/usr/bin/printf "%05000d" 0 | tr 0 q); set -- $(seq 1 384); i=1; while [ $i -le 650 ]; do eval "v$i=x"; i=$((i+1)); done'\'' >/dev/null 2>&1; HOME=$d JASH_SESSION=large $JASH_BIN -c '\''echo "${#BIG}:$#:${384}:$v650"'\'' 2>/dev/null; /bin/rm -rf $d' '5000:384:384:x'
t "session persists multiline env" 'd=$(mktemp -d); HOME=$d JASH_SESSION=multiline $JASH_BIN -c '\''X=$(printf "a\nb")'\'' >/dev/null 2>&1; HOME=$d JASH_SESSION=multiline $JASH_BIN -c '\''printf "<%s>" "$X"'\'' 2>/dev/null; /bin/rm -rf $d' $'<a\nb>'
t "session save failure preserves old" 'd=$(mktemp -d); HOME=$d JASH_SESSION=atomic $JASH_BIN -c '\''X=old'\'' >/dev/null 2>&1; chmod 500 $d/.jash/sessions; HOME=$d JASH_SESSION=atomic $JASH_BIN -c '\''X=new'\'' >/dev/null 2>&1; chmod 700 $d/.jash/sessions; HOME=$d JASH_SESSION=atomic $JASH_BIN -c '\''echo "$X"'\'' 2>/dev/null; /bin/rm -rf $d' 'old'
t "session reads legacy state" 'd=$(mktemp -d); mkdir -p $d/.jash/sessions; printf '\''# JASH STATE FILE\n[CWD]\n[ENV]\nLEGACY=yes\n[ALIAS]\n'\'' > $d/.jash/sessions/legacy.state; HOME=$d JASH_SESSION=legacy $JASH_BIN -c '\''echo "$LEGACY"'\'' 2>/dev/null; /bin/rm -rf $d' 'yes'
t "session refuses incomplete v2" 'd=$(mktemp -d); mkdir -p $d/.jash/sessions; printf '\''# JASH STATE FILE v2\n[CWD2]\n[ENV2]\nBROKEN=796573\n'\'' > $d/.jash/sessions/broken.state; HOME=$d JASH_SESSION=broken $JASH_BIN -c '\''echo "${BROKEN-unset}"'\'' 2>/dev/null; /bin/rm -rf $d' 'unset'
t "session rejects stale cwd" 'd=$(mktemp -d); mkdir -p $d/.jash/sessions; printf '\''# JASH STATE FILE\n[CWD]\n/no/such/jash-cwd\n[ENV]\nPWD=/no/such/jash-cwd\n[ALIAS]\n'\'' > $d/.jash/sessions/stalecwd.state; HOME=$d JASH_SESSION=stalecwd $JASH_BIN -c '\''pwd; echo "$PWD"'\'' 2>/dev/null; /bin/rm -rf $d' "$PROJECT_ROOT"$'\n'"$PROJECT_ROOT"
t "boot thread failure has fallback" "grep -A6 'if (pthread_create' '$PROJECT_ROOT/c/boot.c' | grep -q 'boot_run_phases(bl)'; echo \$?" '0'
t "array refusal rolls back" 'd=$(mktemp -d); printf '\''a=(old keep)\na=(new $(seq 1 50000))\necho "${a[0]}:${a[1]}"\n'\'' > $d/f; $JASH_BIN $d/f 2>/dev/null; /bin/rm -rf $d' 'old:keep'
t "array refusal rolls back env" 'd=$(mktemp -d); printf '\''a=(old keep)\nunset side\na=(${side:=made} $(seq 1 50000))\necho "${side-unset}:${a[0]}:${a[1]}"\n'\'' > $d/f; $JASH_BIN $d/f 2>/dev/null; /bin/rm -rf $d' 'unset:old:keep'
t "array storage failure rolls back" 'a=(old keep); i=1; while [ $i -le 2048 ]; do eval "fill$i=x"; i=$((i+1)); done; a=(new more extra); echo "${a[0]}:${a[1]}:${a[2]-unset}"' $'jash: array element storage exhausted — rolling back\nold:keep:unset'
t "shift past nine"         'set -- a b c d e f g h i j k; shift 9; echo "$#:$1"'  '2:j'
t "bare dollar10 is dollar1" 'f(){ echo "$10"; }; f one two'  'one0'
t "array append elem"       'a=(x y z); a+=([1]=B); echo ${a[1]}'  'yB'  # BUG-106b
t "for glob expands"        'd=$(mktemp -d); cd $d; touch fa fb; for x in f*; do printf "<%s>" $x; done; echo; cd /; rm -rf $d'  '<fa><fb>'  # BUG-108
t "array glob expands"      'd=$(mktemp -d); cd $d; touch fa fb; a=(f*); echo ${#a[@]}; cd /; rm -rf $d'  '2'
t "nested dq cmdsub d2"     'echo "$(echo "$(echo "lvl2 )")")"'  'lvl2 )'  # BUG-099
t "nested dq hash"          'echo "$(echo "$(echo "a # b")")"'  'a # b'
t "heredoc in cmdsub"       $'out=$(cat <<HD\nhd text\nHD\n)\necho "got: $out"'  'got: hd text'  # BUG-090
t "multiline cmdsub"        $'out=$(echo a\necho b)\necho "$out"'  $'a\nb'
t "multiline subshell"      $'(echo s1\necho s2)'               $'s1\ns2'
t "local array shadows"     'a=(o1 o2 o3); f(){ local a; a=(i1 i2); echo -n "${#a[@]}"; }; f; echo ":${#a[@]}"'  '2:3'  # BUG-102
t "array argv builtin"      'a=(x "y z" w); printf "<%s>" "${a[@]}"; echo'  '<x><y z><w>'  # BUG-094
t "array argv external"     'a=(x "y z" w); /usr/bin/printf "<%s>" "${a[@]}"; echo'  '<x><y z><w>'
t "for array elems exact"   'a=("one two" "three  four" last); for w in "${a[@]}"; do printf "<%s>" "$w"; done; echo'  '<one two><three  four><last>'
long_for_word=$(printf 'x%.0s' $(seq 1 5000))
t "for token over 4096"     "for w in $long_for_word; do echo \${#w}; done" '5000'
t "for arith keeps suffix"  "x=0; for w in $long_for_word; do x=\$((x+1)); echo required; done; echo \$x" $'required\n1'
trc "for arith divzero"     "for w in $long_for_word; do x=\$((1/0)); done" '1'
t "for arith += fallback"   "x=1; for w in $long_for_word; do x+=\$((1)); done; echo \$x" '11'
trc "for arith readonly"    "readonly x=1; for w in $long_for_word; do x=\$((2)); done" '1'
t "for expanded list over 64k" 'n=0; for x in $(python3 -c "print((('\''x'\''*300+'\'' '\'')*300))"); do n=$((n+1)); done; echo $n' '300'
t "for literal token over 64k" 'd=$(mktemp -d); python3 -c "open('\''$d/f.jsh'\'', '\''w'\'').write('\''for x in '\'' + '\''x'\''*70000 + '\''; do echo \${#x}; done\\n'\'')"; $JASH_BIN $d/f.jsh; rc=$?; /bin/rm -rf $d; exit $rc' '70000'
t "array copy literal"      'a=(one two three); b=("${a[@]}" four); echo ${#b[@]}:${b[1]}:${b[3]}'  '4:two:four'
t "at adjacency join"       'f(){ printf "<%s>" "pre $@ post"; echo; }; f a "b c" d'  '<pre a><b c><d post>'  # BUG-094d
t "at tail join"            'f(){ printf "<%s>" "$@ tail"; echo; }; f a b'  '<a><b tail>'
t "dq value in dq"          $'q=\x27"\x27\necho "[$q]"'       '["]'    # BUG-096
t "dq value external"       $'q=\x27"\x27\n/bin/echo "ext:[$q]"'  'ext:["]'
t "dq value set"            $'q=\x27"\x27\nset -- "[$q]"\necho "$1"'  '["]'
t "dq value from cmdsub"    $'c=$(printf \x27"\x27)\necho "len=${#c} [$c]"'  'len=1 ["]'
t "dq value in case"        $'v=\x27"\x27\ncase "$v" in \x27"\x27) echo isdq;; *) echo other;; esac'  'isdq'
t "dq value in test"        $'v=\x27"\x27\nif [ "$v" = \x27"\x27 ]; then echo eq; fi'  'eq'
t "dq value heredoc"        $'v=\x27say "hi"\x27\ncat <<XEOF\nline: $v\nXEOF'  'line: say "hi"'
t "apostrophe in comment"   $'x=1  # it\x27s a note\necho "x=$x"'  'x=1'   # BUG-114
t "dq in comment"           $'y=2  # a "note\necho "y=$y"'    'y=2'
t "backslash in comment"    $'z=3  # trailing \\\necho "z=$z"'  'z=3'
t "assign keeps dq value"   $'v=\x27a "b"\x27\nw=$v\necho "$w"'  'a "b"'  # BUG-116a
t "cmdsub capture keeps dq" 'c=$(/bin/echo "say \"go\""); echo "[$c]"'  '[say "go"]'
t "set dq value via at"     $'set -- \x27x"y\x27 tail\nfor a in "$@"; do echo "<$a>"; done'  $'<x"y>\n<tail>'  # BUG-116b
t "escdq array literal"     'a=("has\"escdq"); echo "${#a[@]}:[${a[0]}]"'  '1:[has"escdq]'  # BUG-116c
t "empty elem for at"       'a=("" "b c" ""); i=0; for x in "${a[@]}"; do i=$((i+1)); done; echo $i'  '3'  # BUG-117a
t "IFS empty star join"     $'set -- 1 "2 3"\nIFS=\x27\x27\nprintf "[%s]\\n" "$*"'  '[12 3]'  # BUG-117c
t "IFS empty array star"    $'a=(p q r)\nIFS=\x27\x27\nprintf "[%s]\\n" "${a[*]}"'  '[pqr]'
t "case var pattern"        $'pat=plain\ncase plain in $pat) echo y;; *) echo n;; esac'  'y'  # BUG-119
t "case var glob pattern"   $'pat=\x27pl*\x27\ncase plain in $pat) echo g;; *) echo n;; esac'  'g'
t "array slice multiword"   'a=("a b" "c" "d e" "f"); printf "[%s]" "${a[@]:1:2}"; echo'  '[c][d e]'  # BUG-117b
t "array slice open end"    'a=("a b" "c" "d e"); printf "[%s]" "${a[@]:1}"; echo'  '[c][d e]'
t "array slice into copy"   'a=("a b" "c" "d e" f); b=("${a[@]:1:2}"); echo ${#b[@]}'  '2'
t "array slice unquoted"    'a=(one two three four); echo "${a[@]:1:2}"'  'two three'
t "pattern quoted dq"       $'w="\x27quoted\x27"\necho "${w#"\x27"}"\necho after'  $'quoted\x27\nafter'  # BUG-115
t "pattern escaped sq"      $'w="\x27quoted\x27"\necho "${w#\\\x27}"'      $'quoted\x27'
t "replace escaped sq"      $'w="\x27quoted\x27"\necho "${w//\\\x27/_}"'   '_quoted_'
t "replace escaped dq"      $'x=\x27he said "hi"\x27\necho "${x//\\"/#}"'  'he said #hi#'
t "pattern quoted plain"    'v=abcdef; echo "${v#"ab"}"'      'cdef'
t "comment paren mline sub" $'x=$(\necho hi # note )\necho bye\n)\necho "[$x]"'  $'[hi\nbye]'  # BUG-121a
t "backtick heredoc"        $'x=`cat <<HD\nbt $((5+5))\nHD\n`\necho "[$x]"'  '[bt 10]'  # BUG-121b
t "heredoc mline cmdsub"    $'cat <<HD\nsum=$(\necho $((3+4))\n)\nHD'  'sum=7'  # BUG-121d
t "assign dq cmdsub dq"     'v="$(echo "a b")"; echo "[$v]"'  '[a b]'  # BUG-153
t "assign dq cmdsub pipe"   'v="$(printf "%s" "a b" | cat)"; echo "[$v]"'  '[a b]'
t "assign dq cmdsub nest"   'v="$(echo "$(echo "deep nest")")"; echo "[$v]"'  '[deep nest]'
t "multi assign dq cmdsub"  'A="$(echo "a b")" ; B=2; echo "$A|$B"'  'a b|2'
t "inline env dq cmdsub"    'X="$(echo "a b")" /usr/bin/env | grep "^X="'  'X=a b'
t "sq dq inside dq cmdsub"  $'v="$(echo \'"\')"\necho "ok:[$v]"'  'ok:["]'  # BUG-154
t "dq cmdsub sq patterns"   $'n="$(printf \'%s\\n\' \'a"b\' \'c"d\' | tr -d \'"\')"\necho "[$n]"'  $'[ab\ncd]'  # BUG-154
t "subshell multiline"      $'(echo s1\necho s2)'             $'s1\ns2'
t "set lists vars"          $'zzz_v=hi\nset | grep "^zzz_v="'  'zzz_v=hi'  # BUG-120a
t "set -o not vars"         'zzz_v=hi; set -o | grep -c "^zzz_v="'  '0'
t "declare double-dash"     $'v="a b"\nd=$(declare -p v)\nunset v\neval "$d"\necho "[$v]"'  '[a b]'  # BUG-120b
t "declare -- accepted"     'declare -- w=ok; echo $w'        'ok'
t "arith mul neg rhs"       'echo $(( 5 * -3 ))'              '-15'   # BUG-122
t "arith div neg rhs"       'echo $(( 6 / -2 ))'              '-3'
t "arith sub neg"           'echo $(( 5 - -3 ))'              '8'
t "arith add mul neg"       'echo $(( 2 + 3 * -4 ))'          '-10'
t "arith mod neg"           'echo $(( 7 % -3 ))'              '1'
t "arith power right-assoc" 'echo $(( 2 ** 3 ** 2 ))'         '512'   # BUG-123
t "arith neg power"         'echo $(( -2 ** 2 ))'             '4'      # BUG-124
t "arith double neg guard"  'echo $(( - -5 ))'                '5'
t "let quoted spaces"       $'let "x = 5 + 3"\necho $x'        '8'      # BUG-127
t "let quoted compound"     $'v=5; let "v += 3"\necho $v'      '8'
t "count params at"         $'set -- one two three\necho ${#@}'  '3'   # BUG-136
t "count params star"       $'set -- a b\necho ${#*}'          '2'
t "subst delete first"      's=aXbXc; echo ${s/X}'            'abXc'   # BUG-135
t "subst delete all"        's=aXbXc; echo ${s//X}'           'abc'
t "shift past keeps params" 'set -- a b c; shift 9 2>/dev/null; echo "$? $# $*"'  '1 3 a b c'  # BUG-137
t "shift 0 noop"            'set -- a b c; shift 0; echo $#'  '3'      # BUG-138
t "shift nonnumeric err"    'set -- a b c; shift x 2>/dev/null; echo "$? $#"'  '1 3'  # BUG-139
t "shift normal guard"      'set -- a b c d; shift 2; echo "$# $*"'  '2 c d'
t "slice var offset"        's=HelloWorld; i=2; echo "${s:i}"'  'lloWorld'  # BUG-131
t "slice var off len"       's=HelloWorld; i=2; n=3; echo "${s:i:n}"'  'llo'
t "slice arith offset"      's=HelloWorld; i=2; echo "${s:i+1}"'  'loWorld'
t "slice len expr offset"   's=HelloWorld; echo "${s:${#s}-2}"'  'ld'
t "slice literal guard"     's=HelloWorld; echo "${s:2:3}"'  'llo'
t "slice neg offset guard"  's=HelloWorld; echo "${s: -2}"'  'ld'
t "star default empty"      'set --; echo "${*:-DEF}"'  'DEF'  # BUG-133
t "at default empty"        'set --; echo "${@:-DEF}"'  'DEF'
t "star default set"        'set -- a b; echo "${*:-DEF}"'  'a b'
t "at plus alt set"         'set -- a b; echo "${@:+YES}"'  'YES'
t "indirect default empty"  'v=x; x=; echo "${!v:-DEF}"'  'DEF'  # BUG-132
t "indirect default unset"  'v=x; unset x; echo "${!v:-DEF}"'  'DEF'
t "indirect value"          'v=x; x=hi; echo "${!v:-DEF}"'  'hi'
t "subst escaped star"      's="a*b*c"; echo "${s//\*/-}"'  'a-b-c'  # BUG-134
t "subst literal lbracket"  's="a[b]c"; echo "${s//[/(}"'  'a(b]c'
t "subst quoted star"       's="a*b"; echo "${s//"*"/-}"'  'a-b'
t "strip escaped star"      's="*.txt"; echo "${s#\*}"'  '.txt'
t "subst active class"      's=a1b2c; echo "${s//[0-9]/#}"'  'a#b#c'
t "subst dot active glob"   's=x.y.z; echo "${s//./_}"'  'x_y_z'
t "set -f flag in dash"      'set -f; case "$-" in *f*) echo yes;; *) echo no;; esac'  'yes'  # BUG-141
t "arith div sane"          'echo $((8/2))'  '4'

# ═══ 15. Advanced constructs ═════════════════════════════════════════════════
group "advanced constructs"
t "process substitution" 'diff <(echo hi) <(echo hi) >/dev/null && echo same'  'same'
t "getopts"              'set -- -a val; getopts a: o && echo "$o=$OPTARG"'  'a=val'
t "trap EXIT"            'trap "echo bye" EXIT; echo hi'    $'hi\nbye'
t "array index"          'a=(x y z); echo ${a[1]}'          'y'
t "array length"         'a=(p q r s); echo ${#a[@]}'       '4'
t "array all"            'a=(1 2 3); echo ${a[@]}'          '1 2 3'
t "array subscript var"  'i=5; a[i]=v; echo ${a[5]}'        'v'                # BUG-072b
t "array subscript expr" 'a[1+1]=m; echo ${a[2]}'           'm'                # BUG-072b
t "array prefix cmd"     'a[0]=hi echo test'                'test'             # BUG-072a
t "array prefix exit"    'a[0]=hi false; echo $?'           '1'                # BUG-072a
t "array explicit index" 'a=([0]=x [2]=y); echo "${a[0]}|${a[2]}|${#a[@]}"'  'x|y|2'
t "declare -a compound"  'declare -a a=([0]=x [2]=y); echo "${a[0]}|${a[2]}"'  'x|y'  # BUG-073
t "declare -A string key" 'declare -A m=([foo]=1 [bar]=2); echo "${m[foo]}|${m[bar]}"'  '1|2'  # BUG-073
t "assoc per-element"    'declare -A m; m[k1]=a; m[k2]=b; echo "${m[k1]}|${m[k2]}|${#m[@]}"'  'a|b|2'
t "nested cmdsub arith"  'echo $(( $(echo 3) + $(echo 4) ))' '7'
t "cmdsub in condition"  'if [ "$(echo yes)" = yes ]; then echo ok; fi'  'ok'

# ═══ 16. More coreutils ══════════════════════════════════════════════════════
group "coreutils: ps parity"
# ADR-0044: all-process selectors -e/-A/ax (were: only 'aux' showed all).
t "ps -e all procs"      'test $(ps -e 2>/dev/null | wc -l) -gt 10 && echo many || echo few'  'many'
t "ps -A all procs"      'test $(ps -A 2>/dev/null | wc -l) -gt 10 && echo many || echo few'  'many'
t "ps ax all procs"      'test $(ps ax 2>/dev/null | wc -l) -gt 10 && echo many || echo few'  'many'
t "ps --json pid"        'ps --json 2>/dev/null | head -1 | grep -c pid'  '1'

group "coreutils: kill parity"
# ADR-0044: -l NUM/NAME reverse lookup, extended signal table (deterministic).
t "kill -l 9 name"       'kill -l 9'        'KILL'
t "kill -l TERM number"  'kill -l TERM'     '15'
t "kill -l waitstatus"   'kill -l 139'      'SEGV'    # 139-128 = SIGSEGV
t "kill -l has WINCH"    'kill -l | grep -c WINCH'  '1'

group "coreutils: df parity"
# ADR-0044 (BUG-163): df PATH → containing mount, -T type, --total.
t "df PATH one mount"    'df / | wc -l | tr -d " "'  '2'
t "df -T Type header"    'df -T / | head -1 | grep -c Type'  '1'
t "df --total row"       'df --total | grep -c "^total"'  '1'
t "df --json mountpoint" 'df --json | head -1 | grep -c mountpoint'  '1'

group "coreutils: du parity"
# ADR-0044: du now emits GNU-parseable SIZE<TAB>PATH (was an ASCII chart).
t "du -s summarize"      'd=$(mktemp -d);mkdir $d/a;dd if=/dev/zero of=$d/a/f bs=1024 count=20 2>/dev/null;du -s $d/a|grep -cE "^[0-9]+	.*/a$";rm -rf $d'  '1'
t "du -sh human"         'd=$(mktemp -d);mkdir $d/a;dd if=/dev/zero of=$d/a/f bs=1024 count=20 2>/dev/null;du -sh $d/a|grep -cE "^[0-9.]+[KMG]?	.*/a$";rm -rf $d'  '1'
t "du -c total line"     'd=$(mktemp -d);mkdir $d/a;du -sc $d/a|grep -c "	total$";rm -rf $d'  '1'
t "du -d1 depth"         'd=$(mktemp -d);mkdir -p $d/a/b;du -d1 $d/a|wc -l|tr -d " ";rm -rf $d'  '2'

group "coreutils: sed (in-process, ADR-0044)"
t "sed s basic"          'printf "axb\n" | sed "s/x/-/"'          'a-b'
t "sed s global"         'printf "xax\n" | sed "s/x/-/g"'         '-a-'
t "sed s 2nd occ"        'printf "aaa\n" | sed "s/a/X/2"'         'aXa'
t "sed s backref"        'printf "ab\n" | sed "s/\(a\)\(b\)/\2\1/"'  'ba'
t "sed s whole &"        'printf "cat\n" | sed "s/cat/[&]/"'      '[cat]'
t "sed alt delim"        'printf "a/b\n" | sed "s|/|_|"'          'a_b'
t "sed -n p flag"        'printf "a\nb\n" | sed -n "s/a/X/p"'     'X'
t "sed -E groups"        'printf "12ab\n" | sed -E "s/([0-9]+)([a-z]+)/\2\1/"'  'ab12'
t "sed fallback /d"      'printf "a\nb\nc\n" | sed "/b/d"'        $'a\nc'
t "sed fallback range"   'printf "a\nb\nc\n" | sed -n "2,3p"'     $'b\nc'
t "sed -i edits file"    'd=$(mktemp -d);printf "foo\n">$d/f;sed -i "s/foo/bar/" $d/f;cat $d/f;rm -rf $d'  'bar'

group "coreutils: find (in-process, ADR-0044)"
t "find -name glob"      'd=$(mktemp -d);mkdir -p $d/a;touch $d/a/x.py $d/a/y.txt;cd $d;find . -name "*.py"|sort;cd /;rm -rf $d'  './a/x.py'
t "find -type f count"   'd=$(mktemp -d);mkdir -p $d/a;touch $d/a/1 $d/a/2 $d/b;cd $d;find . -type f|wc -l|tr -d " ";cd /;rm -rf $d'  '3'
t "find -type d"         'd=$(mktemp -d);mkdir -p $d/a/b;cd $d;find . -type d|sort|tr "\n" ",";cd /;rm -rf $d'  '.,./a,./a/b,'
t "find -maxdepth 1"     'd=$(mktemp -d);mkdir -p $d/a/b;touch $d/top;cd $d;find . -maxdepth 1 -type f;cd /;rm -rf $d'  './top'
t "find -empty dir"      'd=$(mktemp -d);mkdir $d/e $d/f;touch $d/f/x;cd $d;find . -type d -empty;cd /;rm -rf $d'  './e'
t "find -not -name"      'd=$(mktemp -d);touch $d/a.py $d/b.txt;cd $d;find . -type f -not -name "*.py"|sort;cd /;rm -rf $d'  './b.txt'
t "find fallback -mtime" 'd=$(mktemp -d);touch $d/x;cd $d;find . -type f -mtime -1|wc -l|tr -d " ";cd /;rm -rf $d'  '1'

group "coreutils: cat (in-process, ADR-0044)"
t "cat file"             'd=$(mktemp -d);printf "a\nb\n">$d/f;cat $d/f;rm -rf $d'  $'a\nb'
t "cat concat"           'd=$(mktemp -d);echo x>$d/a;echo y>$d/b;cat $d/a $d/b;rm -rf $d'  $'x\ny'
t "cat stdin"            'printf "hi\n" | cat'              'hi'
t "cat -n number"        'printf "x\ny\n" | cat -n | tr -s " "'  $' 1\tx\n 2\ty'
t "cat -s squeeze"       'd=$(mktemp -d);printf "a\n\n\n\nb\n">$d/f;cat -s $d/f;rm -rf $d'  $'a\n\nb'
t "cat -E ends"          'd=$(mktemp -d);printf "a\n">$d/f;cat -E $d/f;rm -rf $d'  'a$'
t "cat -- dashfile"      'd=$(mktemp -d);cd $d;printf "z\n">./-w;cat -- -w;cd /;rm -rf $d'  'z'
t "cat heredoc cmdsub"   $'x=$(cat <<HD\nhd text\nHD\n)\necho "$x"'  'hd text'   # BUG-164

group "coreutils: wc (in-process, ADR-0044)"
# Byte-exact BSD columns (" %7ju" per count, fixed order l/w/[c|m]/L) + --json GOLD.
# Column padding is per-wc-lineage (BSD pads to 7 always; GNU derives the width
# from the operands' byte size). Assert agreement with the HOST's wc — that is
# what the parity claim means — instead of freezing one platform's spacing.
t "wc columns"           'printf "a b c\nd e\n" | wc'          "$(printf 'a b c\nd e\n' | wc)"
t "wc -c no newline"     'printf "abc" | wc -c | tr -d " "'    '3'
t "wc -m utf8 chars"     'printf "héllo\n" | wc -m | tr -d " "'  '6'
t "wc -L bytes"          'printf "héllo\n" | wc -L | tr -d " "'  '6'   # é=2 bytes, no -m ⇒ byte length
# jash's wc always decodes UTF-8; GNU wc only does so under a UTF-8 locale (a
# C-locale host counts the 2-byte 'é' as two). The assertion here is jash's
# documented behaviour — 6 chars, longest line 5 — so compare the VALUES and
# let awk drop the column padding, which is per-wc-lineage.
t "wc -mL char len"      'printf "héllo\n" | wc -mL | awk "{print \$1, \$2}"'  '6 5'   # -m active ⇒ -L is char length
t "wc fixed col order"   'printf "a b\n" | wc -c -w -l'        "$(printf 'a b\n' | wc -c -w -l)"
t "wc multi-file total"  'd=$(mktemp -d);printf "a\nb\n">$d/a;printf "c\n">$d/b;wc -l $d/a $d/b|tail -1|tr -dc "0-9";rm -rf $d'  '3'
t "wc --json stdin"      'printf "a b\n" | wc --json'          '[{"name":"-","lines":1,"words":2,"bytes":4}]'
t "wc --json subset"     'printf "x\ny\n" | wc --json -l'      '[{"name":"-","lines":2}]'
t "wc --json chars"      'printf "hé\n" | wc --json -lwm'      '[{"name":"-","lines":1,"words":1,"chars":3}]'

group "coreutils: touch parity"
# ADR-0044 (BUG-161): touch was a stub that skipped every flag.
t "touch -t stamp"       'd=$(mktemp -d);cd $d;touch -t 202001020304 f;date -r f +%Y%m%d%H%M;cd /;rm -rf $d'  '202001020304'
t "touch -d iso date"    'd=$(mktemp -d);cd $d;touch -d "2021-06-15 12:30:00" f;date -r f +%Y%m%d%H%M;cd /;rm -rf $d'  '202106151230'
t "touch -r reference"   'd=$(mktemp -d);cd $d;touch -t 201512250000 r;touch -r r f;date -r f +%Y%m%d%H%M;cd /;rm -rf $d'  '201512250000'
t "touch -c no-create"   'd=$(mktemp -d);cd $d;touch -c gone;test -f gone && echo THERE || echo absent;cd /;rm -rf $d'  'absent'

group "coreutils: cp parity"
# ADR-0044: multi-source (was silently dropping files) + -n/-v/-p.
t "cp multi-source"      'd=$(mktemp -d);cd $d;mkdir o;echo A>a;echo B>b;echo C>c;cp a b c o/;printf "%s%s%s" $(cat o/a) $(cat o/b) $(cat o/c);cd /;rm -rf $d'  'ABC'
t "cp -n no-clobber"     'd=$(mktemp -d);cd $d;echo orig>x;echo new>y;cp -n y x;cat x;cd /;rm -rf $d'  'orig'
t "cp -v verbose"        'd=$(mktemp -d);cd $d;echo hi>s;cp -v s t 2>&1|grep -c "s. -> .t";cd /;rm -rf $d'  '1'
t "cp -- dash source"    'd=$(mktemp -d);cd $d;echo z>./-dash;cp -- -dash out;cat out;cd /;rm -rf $d'  'z'

group "coreutils: mv parity"
# ADR-0044: -f/-n/-v/--, stdout stays POSIX-clean (summary → stderr).
t "mv -f overwrite"      'd=$(mktemp -d);cd $d;echo old>x;echo new>y;mv -f y x;cat x;cd /;rm -rf $d'  'new'
t "mv -n no-clobber"     'd=$(mktemp -d);cd $d;echo keep>x;echo z>y;mv -n y x 2>/dev/null;cat x;cd /;rm -rf $d'  'keep'
t "mv -v verbose stdout" 'd=$(mktemp -d);cd $d;echo z>s;mv -v s t 2>/dev/null|grep -c "s. -> .t";cd /;rm -rf $d'  '1'
t "mv stdout clean"      'd=$(mktemp -d);cd $d;echo z>s;out=$(mv s t);printf "[%s]" "$out";cd /;rm -rf $d'  '[]'
t "mv -- dash source"    'd=$(mktemp -d);cd $d;echo q>./-w;mv -- -w ok;cat ok;cd /;rm -rf $d'  'q'

group "coreutils: ls parity"
# ADR-0044: the sort/target flags that were silent no-ops or mis-wired.
# NOTE: /usr/bin/touch (not jash's builtin, whose -t is unimplemented — a known
# touch-rewrite gap) to set deterministic mtimes; this test targets ls, not touch.
t "ls -t newest first"   'd=$(mktemp -d);cd $d;/usr/bin/touch -t 202001010000 old;/usr/bin/touch -t 202506010000 new;ls -t|tr "\n" " ";cd /;rm -rf $d'  'new old '  # was ALPHABETICAL
t "ls -rt oldest first"  'd=$(mktemp -d);cd $d;/usr/bin/touch -t 202001010000 old;/usr/bin/touch -t 202506010000 new;ls -rt|tr "\n" " ";cd /;rm -rf $d'  'old new '
t "ls -S largest first"  'd=$(mktemp -d);cd $d;printf aaaa>big;printf a>sml;ls -S|tr "\n" " ";cd /;rm -rf $d'  'big sml '
t "ls -r reverse name"   'd=$(mktemp -d);cd $d;touch a b c;ls -r|tr "\n" " ";cd /;rm -rf $d'  'c b a '   # was RECURSIVE
t "ls -l file long"      'd=$(mktemp -d);cd $d;touch f;ls -l f|cut -c1;cd /;rm -rf $d'  '-'   # was name-only
t "ls -d dir entry"      'd=$(mktemp -d);cd $d;mkdir sub;touch sub/inner;ls -d sub;cd /;rm -rf $d'  'sub'  # not contents
t "ls -R still recurses" 'd=$(mktemp -d);cd $d;mkdir sub;touch sub/inner;ls -R|grep -c inner;cd /;rm -rf $d'  '1'
t "ls -l symlink arrow"  'd=$(mktemp -d);cd $d;echo x>tgt;ln -s tgt lnk;ls -l lnk|grep -cE "^l.*lnk -> tgt";cd /;rm -rf $d'  '1'   # lstat, not stat
t "ls -l symlink type l" 'd=$(mktemp -d);cd $d;mkdir rd;ln -s rd ld;ls -l|grep ld|grep -cE "^l";cd /;rm -rf $d'  '1'

group "coreutils: extended"
t "basename"             'basename /a/b/file.txt'           'file.txt'
t "basename suffix"      'basename /a/b/file.txt .txt'      'file'
t "dirname"              'dirname /a/b/file.txt'            '/a/b'
t "cat -n"               'printf "x\\ny\\n" | cat -n | tr -d "[:blank:]"'  $'1x\n2y'
t "rev"                  'echo abcde | rev'                 'edcba'
t "tr char class"        'echo HeLLo | tr "[:upper:]" "[:lower:]"'  'hello'
t "sort -u dedup"        'printf "b\\na\\nb\\nc\\na\\n" | sort -u'  $'a\nb\nc'
t "sort -k field"        'printf "3 c\\n1 a\\n2 b\\n" | sort -k1 -n'  $'1 a\n2 b\n3 c'
t "wc -c bytes"          'printf "abcde" | wc -c | tr -d " "'  '5'
t "head -c bytes"        'printf "abcdefgh" | head -c 4'     'abcd'
t "seq -w padded"        'seq -w 8 10'                      $'08\n09\n10'
t "printf hex"           'printf "%x\\n" 255'               'ff'
t "printf char"          'printf "%c%c%c\\n" a b c'         'abc'
t "printf width neg"     'printf "[%-5s]\\n" hi'            '[hi   ]'

# ═══ 17. jash differentials (functional smoke) ═══════════════════════════════
group "jash differentials"
t "kv set/get"           'kv set _bk hello >/dev/null 2>&1; kv get _bk'  'hello'
t "kv missing"           'kv get _nope_xyz_zzz >/dev/null 2>&1; echo done'  'done'
t "expect rc pass"       'true; expect rc 0 >/dev/null 2>&1 && echo passed'  'passed'
t "expect rc fail"       'false; expect rc 0 >/dev/null 2>&1 || echo caught'  'caught'
t "type reports builtin" 'type kv | grep -o builtin'        'builtin'
t "calc inline"          'calc 6*7 2>/dev/null | grep -o 42 | head -1'  '42'

echo ""
[[ -n "$GROUP" ]] && printf "   %s: %d/%d\n" "$GROUP" "$gpass" "$((gpass+gfail))"
echo "═══════════════════════════════════════════════════════════════"
echo "  FUNCTIONAL BATTERY RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [[ $fail -eq 0 ]]; then echo "  ✅ functional battery PASSED"; exit 0
else echo "  ✗ functional battery FAILED ($fail)"; exit 1; fi
