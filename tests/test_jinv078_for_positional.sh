#!/usr/bin/env bash
# JINV-078 — `for VAR; do … done` (sem `in`) percorre os parametros posicionais.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   POSIX XCU 2.9.4.2: omitir a lista `in …` num `for` e EQUIVALENTE a
#   escrever `for VAR in "$@"`. O laco percorre os parametros posicionais —
#   com aspas, portanto elemento com espaco continua sendo UM elemento e
#   lista vazia executa o corpo ZERO vezes.
#
#   E como toda funcao de shell itera sobre os proprios argumentos, e como
#   todo `configure` de autoconf le a linha de comando:
#
#       for ac_option
#       do
#         case $ac_option in ... esac
#       done
#
# ═══ O DEFEITO ═══
#
#     $ jash -c 'set -- a b; for x; do echo "[$x]"; done'
#     jash: syntax error: for requires 'in' keyword. Did you mean: for x; in ...
#
#   Repare no `for x;` da mensagem: o nome da variavel tinha engolido o `;`,
#   porque a extracao parava so em espaco. O parser nem conseguia NOMEAR o
#   que tinha lido — sinal de que a forma nao era desconhecida, era invisivel.
#
#   E pior que uma linha errada: em script (nao-interativo) esse caminho
#   pedia `exit_requested`. **Uma ocorrencia matava o ARQUIVO INTEIRO.**
#
# ═══ POR QUE ESTE GATILHO NAO ESTAVA NA AUDITORIA ═══
#
#   `audits/BENCHMARK-scripts-reais.md` listou `export: usage` como o
#   bloqueador dominante dos 19 `configure`. Ele era tao dominante que
#   ESCONDIA este: so depois de o `export` deixar de abortar e que 8 das 38
#   invocacoes passaram a morrer aqui. Um bloqueador dominante esconde o
#   proximo — por isso a medicao se refaz a cada onda, nao so no fim.
#
# ═══ COMO FOI CONSERTADO (e por que assim) ═══
#
#   Sem `in`, a lista de palavras vira o texto literal `"$@"` e segue pelo
#   MESMO caminho ja existente e ja coberto. Nao e atalho: e literalmente a
#   definicao do POSIX, e garante que as duas formas nao possam divergir
#   depois (aspas, espaco no elemento, lista vazia, glob). A asercao (5)
#   deste gate afirma exatamente essa equivalencia.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real, e um gate que lesse
# JASH_BIN passaria verde contra qualquer mutante.
#
# ORACULO: /bin/bash em toda asercao.
# Gate: bash tests/test_jinv078_for_positional.sh

set -u
cd "$(dirname "$0")/.." || exit 2

JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "FAIL: $DL ausente (cao de guarda obrigatorio)"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

# `deadline.sh`, nunca `timeout`: timeout(1) NAO EXISTE nesta maquina e sob
# bash devolve 127, fazendo o teste concluir "nao travou" quando travou. E um
# laco `for` mal fechado e exatamente o que trava.
jout() { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jrc()  { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

oracle() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jout "$code"); jr=$(jrc "$code")
    bo=$(bout "$code"); br=$(brc "$code")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s): $code"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"
    fi
}

# Mesma comparacao, mas rodando um ARQUIVO — e assim que `configure` chega, e
# e no modo arquivo que o `exit_requested` matava o resto do script.
#
# O rc sai por FUNCAO (jfrc/bfrc) e nao por `var=$( … 2>&1 )` inline: a captura
# inline com os dois canais fundidos e o achado MERGED-EXACT do JINV-061 — um
# aviso de UX no stderr entraria na comparacao e o veredito passaria a depender
# de sorte. Aqui os dois canais vao para /dev/null e so o `$?` atravessa.
jfout() { (cd "$SB" && "$DL" 20 "$JASH"   ./case.sh "$@" 2>/dev/null); }
bfout() { (cd "$SB" && "$DL" 20 /bin/bash ./case.sh "$@" 2>/dev/null); }
jfrc()  { (cd "$SB" && "$DL" 20 "$JASH"   ./case.sh "$@" >/dev/null 2>&1); printf '%s' "$?"; }
bfrc()  { (cd "$SB" && "$DL" 20 /bin/bash ./case.sh "$@" >/dev/null 2>&1); printf '%s' "$?"; }

oracle_file() {
    local desc="$1" body="$2"; shift 2
    local jo bo jr br
    printf '%s\n' "$body" > "$SB/case.sh"
    jo=$(jfout "$@"); jr=$(jfrc "$@")
    bo=$(bfout "$@"); br=$(bfrc "$@")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo]"
    fi
}

echo "── (1) as quatro grafias da forma sem 'in' ──────────────────────────"
oracle "for x; do … done"      'set -- a b c; for x; do echo "[$x]"; done'
oracle "for x do … done (sem ;)" 'set -- p q; for x do echo "[$x]"; done'
oracle "for x;do … done (colado)" 'set -- p q; for x;do echo "[$x]";done'
oracle_file "for x + newline + do (a grafia do autoconf)" 'set -- um dois
for x
do
  echo "[$x]"
done
echo fim'

echo
echo "── (2) lista VAZIA: o corpo roda zero vezes e a cauda continua ──────"
oracle "sem posicionais, corpo nao roda"  'for x; do echo "NUNCA[$x]"; done; echo depois'
oracle "set -- limpa e o laco fica vazio" 'set -- a b; set --; for x; do echo "NUNCA[$x]"; done; echo depois'
oracle "rc do laco vazio"                 'set --; for x; do echo n; done; echo rc=$?'
oracle_file "script inteiro sobrevive ao laco vazio" 'echo antes
for opt; do echo "NUNCA[$opt]"; done
echo depois-1
echo depois-2'

echo
echo "── (3) o que vinha DEPOIS do laco ainda existe ──────────────────────"
# O defeito nao era so a linha do `for`: em script ele pedia exit_requested e
# o arquivo inteiro morria. Esta e a asercao que separa "erra a linha" de
# "mata o arquivo".
oracle_file "as linhas apos o for continuam rodando" 'set -- a
for x; do echo "corpo=$x"; done
echo linha-1
echo linha-2
echo linha-3'

echo
echo "── (4) DENTRO de funcao: usa o \$@ DA FUNCAO, nao o do script ───────"
oracle_file "funcao itera os proprios argumentos" 'set -- script-a script-b
f() { for y; do echo "f[$y]"; done; }
f um dois tres
echo "apos: $*"'
oracle "funcao sem argumentos"  'set -- x y; f() { for y; do echo "f[$y]"; done; echo "f-fim"; }; f'

echo
echo "── (5) EQUIVALENCIA com \`for VAR in \"\$@\"\` — a definicao do POSIX ──"
# Nao basta "funciona": tem que ser a MESMA coisa. Se as duas formas
# divergirem, uma delas esta errada e o gate diz qual.
for caso in \
    'set -- a b c' \
    'set -- "a b" c' \
    'set --' \
    'set -- "" x' \
    'set -- "*" y' \
    'set -- um' ; do
    sem=$(jout "$caso; for v; do echo \"<\$v>\"; done")
    com=$(jout "$caso; for v in \"\$@\"; do echo \"<\$v>\"; done")
    ora=$(bout "$caso; for v; do echo \"<\$v>\"; done")
    if [ "$sem" = "$com" ] && [ "$sem" = "$ora" ]; then
        ok "equivalente a for v in \"\$@\" — $caso"
    else
        bad "equivalencia $caso" "sem-in=[$sem] com-in=[$com] bash=[$ora]"
    fi
done

echo
echo "── (6) shift, aninhamento, break/continue ───────────────────────────"
oracle "apos shift"            'set -- a b c; shift; for x; do echo "[$x]"; done'
oracle "shift dentro do corpo" 'set -- a b c; for x; do echo "[$x]"; done; shift 2; for y; do echo "y[$y]"; done'
oracle "aninhado com for..in"  'set -- a b; for i; do for j in x y; do echo "$i$j"; done; done'
oracle "for..in aninhado com for nu" 'set -- p q; for j in 1 2; do for i; do echo "$j$i"; done; done'
oracle "continue"              'set -- a b c; for x; do [ "$x" = b ] && continue; echo "[$x]"; done'
oracle "break"                 'set -- a b c; for x; do [ "$x" = b ] && break; echo "[$x]"; done'
oracle "break 2 de dentro"     'set -- a b; for i; do for j in x y; do break 2; done; echo NUNCA; done; echo fim'
oracle "cauda depois do done"  'set -- a; for x; do echo "[$x]"; done && echo cauda'

echo
echo "── (7) o autoconf de verdade: for + case + shift ────────────────────"
oracle_file "laco de opcoes estilo configure" 'ac_help=no
ac_ver=no
for ac_option
do
  case $ac_option in
    --help) ac_help=yes ;;
    --version) ac_ver=yes ;;
    *) echo "outro: $ac_option" ;;
  esac
done
echo "help=$ac_help ver=$ac_ver"' --help zzz

echo
echo "── (8) o que NAO pode ter mudado ────────────────────────────────────"
oracle "for..in normal"        'for i in 1 2 3; do echo "[$i]"; done'
oracle "for..in lista vazia"   'for i in; do echo "NUNCA[$i]"; done; echo depois'
oracle "for..in com expansao"  'L="a b"; for i in $L; do echo "[$i]"; done'
oracle "for..in citado"        'for i in "a b" c; do echo "[$i]"; done'
oracle "for c-style"           'for ((i=0;i<3;i++)); do echo "[$i]"; done'
oracle "for..in multi-linha"   'for i in 1 2
do
  echo "[$i]"
done'
oracle "variavel chamada 'do'?? nao: for..in com palavra do no corpo" \
       'for i in 1; do echo "do-do"; done'
# `for x y` continua sendo erro de sintaxe nos DOIS (o conserto nao pode ter
# transformado sintaxe invalida em silencio — familia JINV-045/069).
jr=$(jrc 'for x y; do echo hi; done'); br=$(brc 'for x y; do echo hi; done')
jo=$(jout 'for x y; do echo hi; done'); bo=$(bout 'for x y; do echo hi; done')
if [ "$jr" != "0" ] && [ "$br" != "0" ] && [ "$jo" = "" ] && [ "$bo" = "" ]; then
    ok "for x y continua erro de sintaxe (jash rc=$jr, bash rc=$br), sem executar o corpo"
else
    bad "for x y" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] — sintaxe invalida virou silencio?"
fi

printf '\n=== JINV-078: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
