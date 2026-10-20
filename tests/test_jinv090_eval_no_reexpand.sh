#!/usr/bin/env bash
# JINV-090 — o VALOR expandido NAO volta a ser sintaxe.
#
# ═══ A regra ═══
#
# POSIX XCU 2.6, ultimo paragrafo: o resultado de uma expansao NAO e
# reexaminado para novas expansoes. O que `$V` devolveu e TEXTO FINAL. O
# `eval` nao e excecao a isso: ele reanalisa UMA vez, sobre o texto JA
# expandido dos seus argumentos — nao e expansao recursiva.
#
#     exec_prefix=NONE
#     bindir='${exec_prefix}/bin'
#     for ac_var in bindir; do eval ac_val=$`echo $ac_var`; echo "[$ac_val]"; done
#       bash: [${exec_prefix}/bin]      jash: [NONE/bin]     ← duas voltas
#
# Bloqueava 10 das 38 invocacoes de `configure`.
#
# ═══ Por que este e o irmao do JINV-089, e nao ele ═══
#
# O JINV-089 e a regra da FONTE: um `$` que nao abre expansao e dado, e por
# isso e blindado onde ainda se sabe disso. O JINV-090 e a regra do VALOR: a
# blindagem tem que SOBREVIVER ate depois da ultima passada que poderia
# reexpandi-la. Sao a mesma marca em dois tempos, e cada metade quebra
# sozinha — por isso dois gates e mutantes separados.
#
# ═══ O ponto que estava furado: o CANO ═══
#
# `eval.c` restaurava o `$` blindado ANTES de despachar o cano; `pipeline.c`
# entao expandia OUTRA VEZ, por segmento. Medido:
#
#     V=VAL; echo "[$(echo '$V')]"        -> [$V]   (certo, sem cano)
#     V=VAL; echo "[$(echo '$V')]" | cat  -> [VAL]  (errado, com cano)
#
# A segunda linha e a de cima com quatro bytes a mais. O escudo do SECURITY
# #15 — que existe desde a auditoria e protege a SAIDA de um comando de ser
# relida como sintaxe — simplesmente nao atravessava um cano. E a forma da
# familia JINV-016/020/032/083/085/087/089: a marca "isto e dado" existia e
# morreu antes do ponto que precisava dela.
#
# O conserto e o mesmo do JINV-089 em case_stmt.c: o restauro MUDOU DE LUGAR,
# nao sumiu. Acontece em `pipeline.c`, depois da expansao de cada segmento —
# em `build_argv_local` (externos) e nos dois caminhos de builtin.
#
# ═══ SEGURANCA — a secao D nao e sobre compatibilidade ═══
#
# Se um valor pudesse virar sintaxe, `V='$(rm -rf alvo)'; echo $V` EXECUTARIA.
# A secao D nao confere so a saida: ela compara o CONTEUDO DO SANDBOX depois
# de rodar o mesmo script nos dois shells. Divergencia ali nao e divergencia
# de compatibilidade — e execucao de dado como comando.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO. Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash exporta
# JASH_BIN apontando para o binario real, e um gate que a leia passa verde
# contra qualquer mutante. Ja mordeu tres agentes.
# Os mutantes estao registrados em SRC_MUTANTS (tools/mutation-check.sh).
#
# ORACULO: /bin/bash em TODA assercao — o MESMO arquivo roda nos dois, e nas
# assercoes de sistema de arquivos o MESMO sandbox e comparado lado a lado.
# A conformidade NAO serve de prova aqui: a linha mais longa de
# `tests/conformance/corpus.txt` tem 105 bytes e nenhum caso dela combina um
# valor com `$` atravessando um cano.
#
# CAO DE GUARDA: tools/deadline.sh, nunca `timeout` — timeout(1) NAO EXISTE
# nesta maquina e devolve 127 sob bash, o que faz o teste concluir "nao
# travou" quando travou.
#
# Gate: bash tests/test_jinv090_eval_no_reexpand.sh

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

"$DL" 2 /bin/sleep 5 >/dev/null 2>&1
[ $? = 124 ] || { echo "FAIL: deadline.sh nao protege (rc != 124)"; exit 1; }

jout() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh 2>/dev/null); }
jrc()  { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

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

mk() { cat > "$SB/t.sh"; }

echo "── A. O REPRODUTOR DO \`configure\` — as formas de \`eval\` ─────────────"

mk <<'EOF'
exec_prefix=NONE
bindir='${exec_prefix}/bin'
for ac_var in bindir; do eval ac_val=$`echo $ac_var`; echo "[$ac_val]"; done
EOF
igual 'eval ac_val=$`echo $ac_var` (a forma exata do configure)'

mk <<'EOF'
exec_prefix=NONE
bindir='${exec_prefix}/bin'
ac_var=bindir
eval ac_val=\$$ac_var;   echo "[$ac_val]"
eval "ac_val=\$$ac_var"; echo "[$ac_val]"
eval ac_val="\$$ac_var"; echo "[$ac_val]"
eval "echo [\$$ac_var]"
EOF
igual 'as outras quatro formas de indirecao por eval'

mk <<'EOF'
exec_prefix=NONE
datadir='${exec_prefix}/share'
libdir='${exec_prefix}/lib'
for ac_var in datadir libdir; do
  eval ac_val=$`echo $ac_var`
  echo "$ac_var=[$ac_val]"
done
EOF
igual 'o laco do configure sobre varias variaveis'

echo "── B. O VALOR DE UMA VARIAVEL NAO E REEXAMINADO ──────────────────────"

mk <<'EOF'
X=SEGREDO
V='$X'
echo "[$V]"
echo [$V]
W='${X}'
echo "[$W]"
Y='$(echo executou)'
echo "[$Y]"
Z='`echo executou`'
echo "[$Z]"
A='$((6*7))'
echo "[$A]"
EOF
igual 'valor com $X / ${X} / $( ) / crase / $(( )) fica LITERAL'

mk <<'EOF'
X=SEGREDO
V='$X'
U=$V
echo "[$U]"
T="$V"
echo "[$T]"
EOF
igual 'copiar o valor de uma variavel para outra nao expande'

mk <<'EOF'
X=SEGREDO
V='$X'
set -- "$V"
echo "[$1]"
for i in "$V"; do echo "[$i]"; done
EOF
igual 'posicionais e `for` recebem o valor literal'

echo "── C. ATRAVESSANDO O CANO — o ponto que estava furado ────────────────"
#
# Cada caso desta secao e um da secao B com um `| cat` no fim. Se o restauro
# do escudo voltar a ser prematuro, so esta secao fica vermelha — e essa
# separacao e o ponto: quatro bytes a mais nao podem mudar a regra.

mk <<'EOF'
V=VAL
echo "[$(echo '$V')]"
echo "[$(echo '$V')]" | cat
echo "[$(echo '$V')]" | cat | cat
EOF
igual 'a SAIDA de um comando continua inerte com e sem cano (SECURITY #15)'

mk <<'EOF'
V=VAL
echo "[$`echo V`]"
echo "[$`echo V`]" | cat
EOF
igual 'o $ inerte da FONTE sobrevive ao cano (cruzamento com JINV-089)'

mk <<'EOF'
X=SEGREDO
V='$X'
echo "[$V]" | cat
echo [$V] | cat
echo "[$V]" | cat | cat
EOF
igual 'o valor literal atravessa um e dois canos'

mk <<'EOF'
V=VAL
echo "[$(echo '$V')]" | grep -c .
echo "[$(echo '$V')]" | wc -c
EOF
igual 'com builtins/externos que CONTAM — o tamanho do texto nao muda'

mk <<'EOF'
V=VAL
if true; then echo "[$(echo '$V')]" | cat; fi
while :; do echo "[$(echo '$V')]" | cat; break; done
{ echo "[$(echo '$V')]" ; } | cat
EOF
igual 'dentro de if / while / grupo, com cano'

mk <<'EOF'
V=VAL
echo "[$(echo '$V')]" > saida.txt
cat saida.txt
rm -f saida.txt
EOF
igual 'com redirecionamento em vez de cano'

# ═══ O texto blindado como ARGV DE UM EXTERNO ═══
#
# As assercoes acima passam o texto pelo STDOUT de um builtin; quem restaura
# ali e o caminho de builtin. O `argv` de um comando EXTERNO dentro de um cano
# e montado por OUTRA funcao (`build_argv_local`), e ela tem o proprio
# restauro. Sem um caso que chegue ate la, remover aquele restauro nao muda
# nada que este gate meca — foi exatamente o que aconteceu: o mutante
# SOBREVIVEU, e o par gate↔mutante estava certo; faltava o caso. Estas quatro
# linhas sao esse caso: o `/bin/echo` recebe o texto como ARGUMENTO.
mk <<'EOF'
V=VAL
X=SEGREDO
W='$X'
true | /bin/echo "[$(echo '$V')]"
true | /bin/echo "[$W]"
true | /bin/echo "[$`echo V`]"
/bin/echo "[$W]" | cat
/bin/echo "[$(echo '$V')]" | cat
true | /bin/echo "[$W]" | cat
EOF
igual 'um EXTERNO recebe o texto blindado como argv, dentro do cano'

echo "── D. SEGURANCA — o valor nao EXECUTA (assercao de SISTEMA DE ARQUIVOS)"
#
# Esta secao nao confia na saida. Ela roda o MESMO script nos dois shells,
# cada um no seu sandbox, e compara o que SOBROU no disco. Se o jash executar
# o que o bash imprimiu, os dois sandboxes divergem — e isso nao e
# incompatibilidade, e execucao de dado como comando.

cat > "$SB/sec.sh" <<'EOF'
D="$1"
mkdir -p "$D/alvo"
: > "$D/alvo/sentinela"
V='$(rm -rf '"$D"'/alvo)'
echo $V
echo "$V"
echo $V | cat
echo "$V" | cat
W='`rm -rf '"$D"'/alvo`'
echo $W
echo "$W"
echo $W | cat
echo "$W" | cat
M='$(date > '"$D"'/marca)'
echo $M
echo "$M" | cat
eval echo "\$V"
eval echo "\$W"
R=$(printf '%s' '$(rm -rf '"$D"'/alvo)')
echo "$R"
echo "$R" | cat
EOF

sec_run() {   # $1=shell  $2=sandbox
    mkdir -p "$2"
    (cd "$2" && "$DL" 60 "$1" "$SB/sec.sh" "$2" >"$2/.out" 2>"$2/.err")
    printf '%s' "$?"
}
sec_state() { (cd "$1" && find . -not -name '.out' -not -name '.err' | LC_ALL=C sort); }

SEC_B="$SB/secb"; SEC_J="$SB/secj"
brc_s=$(sec_run /bin/bash "$SEC_B")
jrc_s=$(sec_run "$JASH"   "$SEC_J")
st_b=$(sec_state "$SEC_B"); st_j=$(sec_state "$SEC_J")

if [ "$jrc_s" = "124" ]; then
    bad "bateria de seguranca" "TRAVOU (deadline 60s)"
elif [ "$st_b" = "$st_j" ]; then
    ok "o disco depois do jash e IDENTICO ao disco depois do bash"
else
    bad "o valor VIROU COMANDO — efeito de disco divergiu do bash" \
        "so no bash: [$(comm -23 <(printf '%s\n' "$st_b") <(printf '%s\n' "$st_j") | tr '\n' ' ')] / so no jash: [$(comm -13 <(printf '%s\n' "$st_b") <(printf '%s\n' "$st_j") | tr '\n' ' ')]"
fi

if [ -e "$SEC_J/alvo/sentinela" ]; then
    ok "a sentinela sobreviveu: nenhum \$( ) / crase de VALOR executou rm"
else
    bad "SENTINELA DESTRUIDA" "o jash EXECUTOU um rm que so existia como DADO"
fi

if [ -s "$SEC_J/marca" ]; then
    bad "COMANDO EXECUTOU" "o \`date\` que so existia dentro de um VALOR rodou e escreveu em disco"
else
    ok "o \`date\` dentro de um valor nao rodou"
fi

ob=$(cat "$SEC_B/.out" 2>/dev/null | sed "s#$SEC_B#SB#g")
oj=$(cat "$SEC_J/.out" 2>/dev/null | sed "s#$SEC_J#SB#g")
if [ "$ob" = "$oj" ]; then
    ok "e a saida da bateria de seguranca tambem e identica ao bash"
else
    bad "saida da bateria de seguranca divergiu" "jash=[$(printf '%s' "$oj" | head -c 90 | tr '\n' '/')] bash=[$(printf '%s' "$ob" | head -c 90 | tr '\n' '/')]"
fi

echo "── E. \`eval\` REANALISA UMA VEZ — nem zero, nem duas ──────────────────"
#
# O outro lado do penhasco: um conserto que apenas blindasse tudo faria o
# `eval` parar de reanalisar, e isso quebra o `eval` inteiro. As duas
# assercoes seguintes so passam juntas se a conta for exatamente UMA volta.

mk <<'EOF'
X=oi
V='$X'
eval "echo [$V]"
eval echo "[$V]"
EOF
igual 'eval REANALISA o texto ja expandido (uma volta, e ela acontece)'

mk <<'EOF'
X=oi
V='$X'
W='$V'
eval "echo [$W]"
EOF
igual 'duas camadas: o eval desce UMA, nao ate o fundo'

mk <<'EOF'
eval 'echo [dentro]'
eval echo '[dentro]'
CMD='echo [via-variavel]'
eval "$CMD"
eval $CMD
EOF
igual 'o eval continua executando o que lhe mandaram executar'

echo "── F. O SENTINELA NUNCA VAZA — inclusive no caminho do cano ──────────"

mk <<'EOF'
V=VAL
echo "[$(echo '$V')]" | cat
echo "[$`echo V`]" | cat
X=SEGREDO; W='$X'
echo "[$W]" | cat
echo "[$(echo '`x`')]" | cat
true | /bin/echo "[$W]"
true | /bin/echo "[$(echo '$V')]"
/bin/echo "[$W]" | cat
EOF
raw=$(jout | tr -d '\001\002\003\004\005\006' | wc -c | tr -d ' ')
all=$(jout | wc -c | tr -d ' ')
if [ "$raw" = "$all" ]; then
    ok "nenhum byte de sentinela (0x01..0x06) escapou por um cano"
else
    bad "sentinela vazou pelo cano" "$((all-raw)) byte(s) de controle na saida"
fi

echo
echo "=== JINV-090: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
