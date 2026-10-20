#!/usr/bin/env bash
# JINV-089 — o `$` que NAO abre expansao e DADO.
#
# ═══ A regra, e por que ela e uma regra e nao um caso ═══
#
# POSIX XCU 2.6 reconhece exatamente tres formas depois do `$`: `${`, `$(` e
# `$nome` (inclusive os especiais `$?`, `$#`, `$@`, `$1`…). QUALQUER outro
# vizinho deixa o `$` inerte: ele e um byte de texto, igual a um `a`.
#
# O jash expande REESCREVENDO TEXTO em passadas, e a substituicao de comando
# roda ANTES da expansao de parametros. Dai o buraco: no instante em que a
# crase e trocada pela saida do comando, um `$` que era inerte SO PORQUE o
# byte seguinte era uma crase passa a encostar num NOME. A passada de
# parametros, que ja so enxerga a cadeia pronta, nao tem como saber que
# aquele `$` nunca abriu nada — e expande.
#
#     exec_prefix=NONE; bindir='${exec_prefix}/bin'
#     eval ac_val=$`echo bindir`
#       bash: ${exec_prefix}/bin      jash: NONE/bin    ← duas voltas
#
# E a forma dos JINV-016/020/032/083/085/087: a informacao "isto e dado"
# EXISTIA no ponto da decisao e morreu antes do ponto que precisava dela. O
# conserto GRAVA a marca onde ainda se sabe — o mesmo sentinela que blinda a
# SAIDA da substituicao (SECURITY #15), aplicado ao `$` da FONTE.
#
# ═══ Os DOIS sitios, em serie ═══
#
#   1. `c/parser/cmdsub.c`   — o `$` antes de crase e BLINDADO na fonte
#   2. `c/parser/case_stmt.c` — o restauro do `$` blindado nao pode ser
#      PREMATURO; ele acontece depois da passada de parametros, senao a
#      blindagem e desfeita exatamente onde ela existe para proteger:
#        V=VAL; case $`echo V` in '$V') echo LIT;; *) echo OTRO;; esac
#          bash -> LIT        jash -> OTRO   (expandiu $V de novo)
#
# Consertar um so nao fecha o outro: sao mutantes separados em SRC_MUTANTS.
#
# ═══ O que este gate protege, e o que NAO ═══
#
# Protege a regra INTEIRA nos dois sentidos, e o segundo importa tanto quanto
# o primeiro: um escudo que blindasse `$` demais mataria a expansao de
# verdade. A secao C e o controle POSITIVO — `$V`, `${V}`, `$1`, `$#` TEM que
# continuar expandindo. Um escudo largo demais fica vermelho ali.
#
# NAO protege o teto de tamanho do corpo de `$( )`: isso e o JINV-088. A
# secao G so confere que o escudo nao REINTRODUZIU um teto.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO. Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash exporta
# JASH_BIN apontando para o binario real, e um gate que a leia passa verde
# contra qualquer mutante. Ja mordeu tres agentes.
# Os dois mutantes estao registrados em SRC_MUTANTS (tools/mutation-check.sh),
# nao so neste comentario.
#
# ORACULO: /bin/bash em TODA assercao — o MESMO arquivo roda nos dois. Nenhum
# resultado deste arquivo foi escrito a mao.
# A conformidade NAO serve de prova aqui: a linha mais longa de
# `tests/conformance/corpus.txt` tem 105 bytes e nenhum caso dela combina `$`
# com crase. Ela nao pode, por construcao, ver esta familia.
#
# CAO DE GUARDA: tools/deadline.sh, nunca `timeout` — timeout(1) NAO EXISTE
# nesta maquina e devolve 127 sob bash, o que faz o teste concluir "nao
# travou" quando travou.
#
# Gate: bash tests/test_jinv089_dollar_as_data.sh

set -u
cd "$(dirname "$0")/.." || exit 2

JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
export PATH="/usr/bin:/bin:$PATH"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "FAIL: $DL ausente (cao de guarda obrigatorio)"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

# O cao de guarda tem que estar VIVO antes de qualquer assercao depender dele.
"$DL" 2 /bin/sleep 5 >/dev/null 2>&1
[ $? = 124 ] || { echo "FAIL: deadline.sh nao protege (rc != 124)"; exit 1; }

rep() { awk -v n="$1" 'BEGIN{s="";while(length(s)<n)s=s "a";printf "%s", substr(s,1,n)}'; }

jout() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh 2>/dev/null); }
jrc()  { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

# A assercao central: identico ao bash, saida E rc. Nenhum valor esperado
# aparece neste arquivo — quem responde e o oraculo.
igual() {
    local desc="$1" jo bo jr br
    jo=$(jout); jr=$(jrc)
    bo=$(bout); br=$(brc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 60s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr [$(printf '%s' "$jo" | head -c 90 | tr '\n' '/')] != bash rc=$br [$(printf '%s' "$bo" | head -c 90 | tr '\n' '/')]"
    fi
}

# Escreve $SB/t.sh a partir do stdin (bash puro: heredoc citado, sem expansao).
mk() { cat > "$SB/t.sh"; }

echo "── A. O CASO CENTRAL: \$ + crase — o \$ nao abre nada, e DADO ─────────"
#
# O `$` esta colado numa crase. Depois que a crase virar a saida do comando,
# ele vai encostar num NOME. Se a marca "isto e dado" nao existir, a passada
# de parametros expande — e o valor e expandido MAIS UMA VEZ.

mk <<'EOF'
V=VAL
echo "[$`echo V`]"
EOF
igual 'echo "[$`echo V`]" — o $ colado na crase nao expande V'

mk <<'EOF'
V=VAL
echo [$`echo V`]
EOF
igual 'o mesmo FORA de aspas duplas'

mk <<'EOF'
exec_prefix=NONE
bindir='${exec_prefix}/bin'
echo "[$`echo bindir`]"
EOF
igual 'o $ + crase nao vira o VALOR de bindir (configure --bindir)'

mk <<'EOF'
V=VAL
echo "[$`echo V``echo V`]"
EOF
igual 'duas crases em sequencia depois do $'

# O `$$` e UM parametro (o PID), nao dois sigilos: so o TERCEIRO `$` e o
# inerte. Um escudo que casasse com o segundo `$` de `$$` apagaria o PID —
# foi exatamente o que a primeira versao do conserto fez, e e o que esta
# assercao prende. O PID difere entre os dois shells, entao o proprio script
# o mascara: os dois lados passam pelo MESMO sed.
mk <<'EOF'
V=VAL
echo "[$$$`echo V`]" | sed 's/[0-9][0-9]*/PID/'
EOF
igual 'o $$ (PID) seguido de $ + crase — so o TERCEIRO $ e dado'

mk <<'EOF'
V=VAL
echo "[$$`echo V`]" | sed 's/[0-9][0-9]*/PID/'
EOF
igual 'o $$ colado na crase continua sendo o PID (escudo nao pode ser largo)'

mk <<'EOF'
V=VAL
echo "[$`echo V` $`echo V`]"
EOF
igual 'dois $ + crase na mesma linha'

mk <<'EOF'
V=VAL
echo "[${V}$`echo V`]"
EOF
igual 'expansao de verdade e $ inerte lado a lado'

mk <<'EOF'
V=VAL
echo '[$`echo V`]'
EOF
igual 'dentro de aspas SIMPLES nada roda e nada expande'

echo "── B. \$ INERTE POR VIZINHO — a regra vale para todo vizinho invalido ─"

# `-` NAO entra nesta lista: `$-` e um parametro especial de VERDADE (as
# flags correntes do shell, POSIX XCU 2.5.2), nao um vizinho invalido. O jash
# ainda nao o implementa (devolve vazio onde o bash devolve as flags) — e uma
# lacuna REAL, medida, mas de outra familia: ela nao tem nada a ver com o `$`
# inerte, e um gate desta onda que a incluisse estaria vermelho por um motivo
# que este invariante nao promete consertar. Fica registrada aqui, nao
# escondida.
for viz in '' ' ' '%' '^' '=' '.' ',' '/' ':' ']' '>' '<' '|' '&' ';' '!' '~' '+'; do
    printf 'echo "[$%s]"\n' "$viz" > "$SB/t.sh"
    igual "\$ seguido de [${viz:-<fim>}] e dado dentro de aspas duplas"
done

mk <<'EOF'
echo "[a$]"
echo "[$]"
echo [a$]
echo "[$ ]"
printf "[%s]\n" "$"
printf "[%s]\n" "a\$b"
EOF
igual 'o $ no FIM da string, e sozinho, nos dois modos de citacao'

mk <<'EOF'
echo "[\$X]"
echo "[\${X}]"
echo "[\$(echo nao)]"
EOF
igual 'o $ ESCAPADO nunca abre expansao (nem de comando)'

mk <<'EOF'
echo '[`echo x`]'
echo '[$`echo x`]'
echo '[$(echo x)]'
EOF
igual 'crase e $( ) dentro de aspas simples sao texto'

echo "── C. CONTROLE POSITIVO: o escudo NAO pode matar a expansao de verdade"
#
# Esta secao e a metade que impede o conserto de virar o bug oposto. Um escudo
# largo demais (blindar todo `$`) passaria em A e B e ficaria VERMELHO aqui.

mk <<'EOF'
V=VAL
echo "[$V]"
echo "[${V}]"
echo "[${V}x]"
echo "[$Vx]"
echo "[${V:-alt}]"
echo "[${NAO:-alt}]"
echo "[${#V}]"
EOF
igual '$V / ${V} / ${V:-alt} / ${#V} continuam expandindo'

mk <<'EOF'
set -- um dois tres
echo "[$1][$2][$#][$@]"
echo "custa $5"
EOF
igual 'os posicionais $1 $2 $# $@ e o $5 vazio continuam expandindo'

mk <<'EOF'
echo "[$(echo dentro)]"
echo "[`echo dentro`]"
V=$(echo atrib)
echo "[$V]"
W=`echo atrib2`
echo "[$W]"
EOF
igual '$( ) e ` ` continuam SENDO substituicao de comando'

mk <<'EOF'
echo "[$((2+3))]"
echo "[$(( 7 * 6 ))]"
EOF
igual 'a aritmetica $(( )) continua funcionando'

mk <<'EOF'
true
echo "[$?]"
false
echo "[$?]"
EOF
igual 'o $? continua sendo o status'

mk <<'EOF'
echo "[$$]" | sed 's/[0-9][0-9]*/PID/'
echo "[$$x]" | sed 's/[0-9][0-9]*/PID/'
echo "[a$$]" | sed 's/[0-9][0-9]*/PID/'
EOF
igual 'o $$ continua sendo o PID em todas as vizinhancas'

echo "── D. O CAMINHO DO \`case\` — o restauro nao pode ser PREMATURO ────────"
#
# case_stmt.c desfazia a blindagem ANTES da passada de parametros. Blindar na
# fonte e desblindar cedo demais da no mesmo: o `$` volta vivo bem a tempo de
# ser expandido. Mutante separado.

mk <<'EOF'
V=VAL
case $`echo V` in '$V') echo LIT;; *) echo OTRO;; esac
EOF
igual 'case $`echo V` casa o padrao LITERAL $V'

mk <<'EOF'
V=VAL
case "$`echo V`" in '$V') echo LIT;; *) echo OTRO;; esac
EOF
igual 'o mesmo com o sujeito entre aspas duplas'

mk <<'EOF'
V=VAL
case $`echo V` in "\$V") echo LIT;; *) echo OTRO;; esac
EOF
igual 'o mesmo com o padrao escapado em vez de aspas simples'

mk <<'EOF'
V=VAL
case $`echo V` in $V) echo EXPANDIU;; '$V') echo LIT;; *) echo OTRO;; esac
EOF
igual 'o padrao que EXPANDE e o literal na mesma clausula, em ordem'

mk <<'EOF'
exec_prefix=NONE
bindir='${exec_prefix}/bin'
case $`echo bindir` in '$bindir') echo LIT;; *) echo OTRO;; esac
EOF
igual 'o caso do configure atravessando o case'

echo "── E. A SAIDA DA SUBSTITUICAO CONTINUA INERTE (SECURITY #15) ─────────"
#
# O escudo do `$` da FONTE nao pode enfraquecer o escudo que ja existia para a
# SAIDA. Um `$` que o comando IMPRIMIU e dado, sempre.

mk <<'EOF'
V=VAL
echo "[$(echo '$V')]"
echo "[`echo '$V'`]"
echo "[$(printf '%s' '${V}')]"
echo "[$(printf '%s' '$(echo nao)')]"
echo "[$(printf '%s' '`echo nao`')]"
EOF
igual 'o $ que veio da SAIDA de um comando nao e reexpandido'

mk <<'EOF'
V=VAL
X=$(echo '$V')
echo "[$X]"
EOF
igual 'guardado numa variavel, o $ da saida continua inerte'

echo "── F. O SENTINELA NUNCA VAZA PARA A SAIDA ────────────────────────────"
#
# A blindagem e um byte de controle (0x01..0x06) que existe apenas no texto
# transitorio. Se ele aparecer no stdout, o conserto trocou um bug silencioso
# por outro — e o `igual` acima nao veria diferenca se o bash tambem nao
# imprimisse nada visivel. Aqui a assercao e sobre os BYTES.

mk <<'EOF'
V=VAL
echo "[$`echo V`]"
echo "[$]"
echo "[a$]"
case $`echo V` in '$V') echo LIT;; *) echo OTRO;; esac
EOF
raw=$(jout | tr -d '\001\002\003\004\005\006' | wc -c | tr -d ' ')
all=$(jout | wc -c | tr -d ' ')
if [ "$raw" = "$all" ]; then
    ok "nenhum byte de sentinela (0x01..0x06) escapou para o stdout"
else
    bad "sentinela vazou para o stdout" "$((all-raw)) byte(s) de controle na saida"
fi

echo "── G. TAMANHO NAO MUDA A REGRA (o escudo nao reintroduziu teto) ──────"
#
# JINV-088 matou tres tetos de 512 no corpo de `$( )`. O escudo do JINV-089
# escreve um byte a mais no fluxo de saida; se ele tivesse trazido um teto de
# volta, seria aqui.

for n in 600 5000; do
    { printf 'V=VAL\necho "[$`echo '; rep "$n" | tr -d '\n'; printf ' >/dev/null; echo V`]"\n'; } > "$SB/t.sh"
    igual "\$ + crase com corpo de $n bytes"
done

for n in 600 5000; do
    { printf 'echo "[$('; printf 'printf %%s '; rep "$n" | tr -d '\n'; printf ' | wc -c)]"\n'; } > "$SB/t.sh"
    igual "\$( ) de $n bytes com cauda \`| wc -c\` (cruzamento com JINV-088)"
done

echo
echo "=== JINV-089: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
