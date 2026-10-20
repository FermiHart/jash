#!/usr/bin/env bash
# test_builtin_sweep.sh — exercita TODOS os builtins do registry, um a um,
# numa sandbox descartável. Complementa (nao substitui) as baterias: elas
# testam SEMANTICA em profundidade, este varre COBERTURA em largura e responde
# "algum builtin sumiu, trava ou quebra?" sem ninguem digitar 170 comandos.
#
# Uso:  bash tests/test_builtin_sweep.sh
#       JASH=./build/jash bash tests/test_builtin_sweep.sh
#
# Critério de FALHA (não é "rc != 0" — `false`, `[ ]` e `expect` retornam != 0
# por contrato):
#   CRASH    rc >= 128, ou stderr com Segmentation/Aborted/buffer overflow
#   HANG     estourou o timeout
#   MISSING  "command not found" — está no registry mas não resolve
#   USAGE    só imprimiu usage/erro de sintaxe para uma invocação que deveria
#            funcionar (reportado à parte, é sinal fraco)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
# Read the compiled profile's live registry. Parsing source text counted
# conditional SinguOS entries in a Linux build and the old installed /usr/local
# binary could be stale; both made the sweep claim a different denominator than
# the executable it was meant to test.
LIST="$(mktemp)"
python3 - "$JASH" > "$LIST" <<'PYEOF'
import json, subprocess, sys
result = subprocess.run([sys.argv[1], "--builtins-json"],
                        capture_output=True, text=True, timeout=15)
if result.returncode:
    raise SystemExit("cannot query compiled builtin registry: " + result.stderr)
document = json.loads(result.stdout)
names = sorted({row["name"] for row in document["builtins"]})
if len(names) != document["count"]:
    raise SystemExit("compiled builtin registry count mismatch")
print("\n".join(names))
PYEOF
TO=8

SB=$(mktemp -d /tmp/jash-sweep.XXXXXX)
export HOME="$SB/home"
mkdir -p "$HOME/.jash/modules" "$SB/work"
cp /home/alf/OS/Jash/modules/*/*.modjash "$HOME/.jash/modules/" 2>/dev/null
cd "$SB/work" || exit 1

# fixtures que os builtins de arquivo consomem
printf 'alpha\nbeta\ngamma\n'  > f.txt
printf '3\n1\n2\n'             > n.txt
printf 'a b c\n'               > w.txt
mkdir -p sub && printf 'x\n' > sub/g.txt
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_SIGIL=0 JASH_LOOPGUARD=off

# Invocação segura por builtin. Sem entrada aqui => tenta "<nome> --help".
declare -A INV=(
  [.]='. /dev/null'                 [:]=':'
  ['[']='[ 1 -eq 1 ]'               [abbr]='abbr'
  [alias]='alias'                   [array]='array'
  [assert]='assert 1 -eq 1'         [avg]='printf "1\n2\n3\n" | avg'
  [awk]='awk "{print \$1}" f.txt'   [bg]='bg'
  [builtin]='builtin echo hi'       [calc]='calc 2+2'
  [cat]='cat f.txt'                 [chmod]='chmod 644 f.txt'
  [chown]='chown "$(id -un)" f.txt' [clear]='true'
  [command]='command echo hi'       [compgen]='compgen -b'
  [complete]='complete'             [continue]='true'
  [count]='printf "a\nb\n" | count' [cp]='cp f.txt cp.out'
  [declare]='declare X=1'           [df]='df .'
  [diff]='diff f.txt f.txt'         [dirs]='dirs'
  [disown]='disown'                 [du]='du -s .'
  [echo]='echo hi'                  [enable]='enable'
  [eval]='eval "echo hi"'           [exit]='true'
  [expect]='true; expect rc 0'      [export]='export X=1'
  [false]='false'                   [fc]='fc -l'
  [fg]='fg'                         [find]='find . -name "*.txt"'
  [getopts]='getopts ab OPT'        [grep]='grep alpha f.txt'
  [hash]='hash'                     [help]='help'
  [history]='history'               [jash_state]='jash_state'
  [json]='json echo hi'             [kill]='kill -0 $$'
  [let]='let X=1+1'                 [ln]='ln -s f.txt link.out'
  [local]='local X=1'               [logout]='true'
  [ls]='ls'                         [mapfile]='printf "a\nb\n" | mapfile A'
  [mkdir]='mkdir -p newdir'         [mv]='cp f.txt m.in && mv m.in m.out'
  [popd]='popd'                     [printf]='printf "%s\n" hi'
  [ps]='ps'                         [pushd]='pushd .'
  [pwd]='pwd'                       [read]='printf "x\n" | read V'
  [readarray]='printf "a\n" | readarray A'
  [readonly]='readonly RX=1'        [rm]='cp f.txt r.in && rm -f r.in'
  [rmdir]='mkdir -p rd && rmdir rd' [sed]='sed "s/alpha/A/" f.txt'
  [set]='set'                       [shift]='shift'
  [shopt]='shopt'                   [sortby]='printf "1\n2\n" | sortby'
  [stat]='stat f.txt'               [sum]='printf "1\n2\n" | sum'
  [suspend]='true'                  [times]='times'
  [touch]='touch t.out'             [trap]='trap'
  [true]='true'                     [type]='type echo'
  [typeset]='typeset X=1'           [ulimit]='ulimit -n'
  [umask]='umask'                   [unalias]='alias zz=echo; unalias zz'
  [uname]='uname -s'                [uniqby]='printf "a\na\n" | uniqby'
  [unset]='X=1; unset X'            [wait]='wait'
  [wc]='wc -l f.txt'                [which]='which echo'
  [xargs]='printf "hi\n" | xargs echo'
  [exec]='true'                     [trash]='cp f.txt t.in && trash t.in'
  [undo]='undo --dry'               [redo]='redo --dry'
  [rewind]='rewind --help'          [revert]='revert --help'
  [module]='module list'            [sigil]='sigil'
  [timeout]='timeout 2 true'        [contract]='contract --help'
  [where]='ls --json | where type=file'   [pick]='ls --json | pick name'
  [describe]='ls --json | describe'       [count]='ls --json | count'
  [sum]='ls --json | sum size'            [avg]='ls --json | avg size'
  [sortby]='ls --json | sortby name'      [uniqby]='ls --json | uniqby type'
  [adopt]='adopt status'                  [agent-guard]='agent-guard status'
  [array]='array A[0]=1'                  [config]='config show'
  [cost]='cost "echo hi"'                 [jash_state]='jash_state save'
  [kv]='kv set k v'                       [revert]='revert git commit --dry'
  [tdiff]='tdiff f.txt @nope'                  [weave]='weave {"a":"echo hi"}'
  [cd]='cd /tmp'                          [env]='env'
  [jobs]='jobs'                           [break]='for i in 1; do break; done'
  [return]='f() { return 0; }; f'         [source]='source /dev/null'
  [test]='test 1 -eq 1'                   [breathe]='breathe'
  [mistake]='mistake'
)

pass=0; crash=(); hang=(); missing=(); usage=()
# Ler a lista INTEIRA antes do laço: builtins que consomem stdin (read, cat,
# wc, mapfile...) engoliam o resto do arquivo quando o while lia direto dele —
# a primeira rodada deste sweep testou 38 dos 159 e reportou "tudo ok".
mapfile -t NAMES < "$LIST"
for b in "${NAMES[@]}"; do
  [ -z "$b" ] && continue
  cmd="${INV[$b]:-$b --help}"
  out=$(timeout "$TO" "$JASH" -c "$cmd" </dev/null 2>&1); rc=$?
  if [ $rc -eq 124 ]; then hang+=("$b"); continue; fi
  # rc >= 129 = morto por sinal (139 SIGSEGV, 134 SIGABRT). rc == 128 NAO:
  # e o codigo que o git usa para "fatal", e builtins que embrulham git o
  # propagam — `revert` fora de um repo devolve 128 sem nada ter quebrado.
  if [ $rc -ge 129 ] || grep -qiE 'segmentation|aborted|buffer overflow|stack smashing' <<<"$out"; then
      crash+=("$b :: rc=$rc :: $(head -1 <<<"$out" | cut -c1-60)"); continue; fi
  if grep -qiE 'command not found|not a builtin' <<<"$out"; then
      missing+=("$b"); continue; fi
  if [ $rc -ne 0 ] && grep -qiE '^usage|usage:' <<<"$out"; then
      usage+=("$b :: $(head -1 <<<"$out" | cut -c1-50)"); continue; fi
  pass=$((pass+1))
done

echo "════════════════════════════════════════════════"
echo "  BUILTIN SWEEP — $(wc -l < "$LIST") builtins"
echo "════════════════════════════════════════════════"
echo "  OK       : $pass"
echo "  CRASH    : ${#crash[@]}"
echo "  HANG     : ${#hang[@]}"
echo "  MISSING  : ${#missing[@]}"
echo "  USAGE    : ${#usage[@]}   (sinal fraco — pode ser invocação minha)"
echo
[ ${#crash[@]}   -gt 0 ] && { echo "── CRASH ──";   printf '  %s\n' "${crash[@]}"; }
[ ${#hang[@]}    -gt 0 ] && { echo "── HANG ──";    printf '  %s\n' "${hang[@]}"; }
[ ${#missing[@]} -gt 0 ] && { echo "── MISSING ──"; printf '  %s\n' "${missing[@]}"; }
[ ${#usage[@]}   -gt 0 ] && { echo "── USAGE ──";   printf '  %s\n' "${usage[@]}"; }
rm -rf "$SB" "$LIST"
[ ${#crash[@]} -eq 0 ] && [ ${#hang[@]} -eq 0 ] && [ ${#missing[@]} -eq 0 ] || exit 1
exit 0
