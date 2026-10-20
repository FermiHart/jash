#!/usr/bin/env bash
# JINV-096 — o VALOR de uma expansão é DADO; nunca vira SINTAXE.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# POSIX XCU 2.6: sobre o RESULTADO de uma expansão não citada aplicam-se
# APENAS duas coisas — divisão de campos (`$IFS`) e expansão de nome de
# arquivo (glob). NUNCA reanálise sintática.
#
# Um `|` que veio de um VALOR é texto. Um `|` que o autor DIGITOU é
# estrutura. São o mesmo byte e origens opostas, e a origem é a única coisa
# que decide.
#
# ═══ O DEFEITO, medido no HEAD contra /bin/bash ═════════════════════════
#
#     P='dado | tee /tmp/X'; echo $P
#       bash:  imprime o texto literal, NÃO cria /tmp/X
#       jash:  imprime `dado`, CRIA /tmp/X          ← EXECUTOU o tee
#
#     V='dado > /tmp/Y'; echo $V
#       bash:  imprime o texto, NÃO cria /tmp/Y
#       jash:  saída vazia, CRIA /tmp/Y             ← redirecionou de verdade
#
# Com `|`, um comando nomeado apenas em DADO executa. Qualquer script que
# faça `echo $LINHA` ou `cmd $ARG` sobre conteúdo vindo de arquivo, de rede,
# de argumento de linha de comando ou de variável de ambiente EXECUTAVA o que
# estivesse ali. Isso não é incompatibilidade: é execução de comando a partir
# de dado não confiável.
#
# Medido no binário construído do HEAD puro — não era regressão de onda
# nenhuma. Vazavam `|`, `<`, `>`, `>>`, `2>` (e `||`, por tabela, porque
# contém `|`) e o til `~`. Ficavam corretamente inertes `;`, `&`, `&&`,
# `` ` ``, `$( )` e `$(( ))` — os três últimos por causa dos JINV-089/090,
# os três primeiros porque o divisor de listas roda ANTES da expansão.
#
# ═══ ONDE A ORIGEM SE PERDIA ════════════════════════════════════════════
#
# O jash decide "esta linha tem cano?" (`jash_line_has_pipe`, pipeline.c) e
# "esta linha redireciona?" (`jash_line_has_redir`) DEPOIS de reescrever a
# linha com as expansões — no instante em que as duas origens já viraram o
# mesmo byte. É a família do JINV-016/020/032/083/087/089/090/093: a
# informação "isto é dado" existe no ponto da emissão e morre antes do ponto
# que precisa dela.
#
# ═══ POR QUE NÃO É O JINV-093 ═══════════════════════════════════════════
#
# O JINV-093 é o caso ESCAPADO (`\|`): lá a marca de citação EXISTIA (a
# barra) e era comida pela passada de parâmetros; o conserto foi PRESERVÁ-LA.
# Aqui nunca houve marca nenhuma — o byte sai do valor idêntico ao byte que o
# autor digitaria. A marca teve que ser CRIADA no ponto da emissão, que é o
# único lugar onde ainda se sabe de onde o byte veio.
#
# ═══ A RÉGUA: os dois lados ═════════════════════════════════════════════
#
# Um conserto que simplesmente parasse de ver `|` quebraria o shell. Por isso
# cada asserção de "dado" tem a irmã de "estrutura":
#
#   V='a | tee X'; echo $V     → texto, e NENHUM arquivo criado
#   V=a;           echo $V | tr a A  → cano de verdade
#   V='a > f';     echo $V     → texto, e NENHUM arquivo criado
#   V=a;           echo $V > f → f contém `a`
#   V='*';         echo $V     → o glob EXPANDE (é a regra POSIX, não confunda)
#
# ═══ A ASSERÇÃO CENTRAL É O DISCO, NÃO O STDOUT ═════════════════════════
#
# `echo $V` com `V='dado > Y'` imprime NADA nos dois shells se o único
# critério for stdout — o jash redirecionou, o bash… também imprimiria o
# texto, mas um teste desatento compara duas saídas e vê duas strings. O que
# separa os dois é o ARQUIVO Y. Por isso o juiz fotografa o diretório de
# trabalho antes e depois em CADA caso e exige que o retrato do jash seja
# idêntico ao do bash. Divergência ali não é diferença de formatação: é um
# comando que rodou.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# tipo de sandbox. Nenhuma afirma nada por conta própria.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe como binário nesta máquina (só como builtin do jash) e devolve 127
# sob bash — um teste que veja rc≠124 concluiria "não travou" de um
# travamento real. A asserção 0 prova que o cão está VIVO antes que qualquer
# coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante. Já
# mordeu três agentes; a prova negativa deste conserto depende disso.
#
# `JASH_MEMO=0` é obrigatório: a memoização do jash intercepta comparações e
# devolve resultado antigo.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binário sem o conserto> deixa este gate
# VERMELHO. Mutantes registrados em SRC_MUTANTS (tools/mutation-check.sh).
#
# ═══ O QUE ESTE GATE NÃO ACUSA (medido, PRÉ-EXISTENTE) ══════════════════
#
# A expansão de CHAVES (`{a,b}`) ainda roda sobre o resultado de uma
# expansão — `V='{a,b}'; echo $V` dá `a b` no jash e `{a,b}` no bash. É a
# mesma regra e a mesma camada de palavra do til, mas NÃO é execução de
# comando (nenhum programa roda), e o conserto dela é outro par de
# sentinelas com outra medida. Fica registrado, não asserido: asserir um
# defeito que este conserto não fez faria a prova negativa mentir.
#
# Gate: bash tests/test_jinv096_value_not_syntax.sh

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
export JASH_ABSORB=0
export PATH="/usr/bin:/bin:$PATH"
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# ── 0. ANTI-INÉRCIA: o cão de guarda está vivo? ────────────────────────
"$DEADLINE" 2 /bin/sleep 30 >/dev/null 2>&1
if [ $? = 124 ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "cão de guarda inerte" \
        "deadline.sh 2 sleep 30 não devolveu 124 — as asserções de prazo estão sem valor"
fi

# ── o juiz ─────────────────────────────────────────────────────────────
# Cada caso roda num par de diretórios VIRGENS e IDÊNTICOS. Compara três
# coisas, e a terceira é a que importa:
#   1. stdout   2. código de saída   3. O RETRATO DO DIRETÓRIO
# `find` inclui tamanho porque `>>` num arquivo que já existe não muda o
# nome, só o conteúdo — sem o tamanho, um append vazado passaria batido.
fixture() {
    mkdir -p "$1/sub"
    printf 'alpha\nbeta\n' > "$1/dados.txt"
    : > "$1/ja-existe"
}
retrato() { (cd "$1" && find . -type f -exec /bin/ls -ld -- {} \; 2>/dev/null \
                | /usr/bin/awk '{print $5, $NF}' | LC_ALL=C sort); }

julgar() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br js bs
    rm -rf "$W/j" "$W/b"; mkdir -p "$W/j" "$W/b"
    fixture "$W/j"; fixture "$W/b"
    printf '%s\n' "$prog" > "$W/j/p.sh"; cp "$W/j/p.sh" "$W/b/p.sh"
    # stderr DESCARTADO, nunca fundido (JINV-061): o jash escreve avisos de UX
    # no stderr real com contador PERSISTENTE entre invocações; fundi-los faria
    # o veredito depender do contador.
    jo=$( (cd "$W/j" && LC_ALL=C LANG=C "$DEADLINE" 15 "$JASH"   ./p.sh 2>/dev/null) ); jr=$?
    bo=$( (cd "$W/b" && LC_ALL=C LANG=C "$DEADLINE" 15 "$ORACLE" ./p.sh 2>/dev/null) ); br=$?
    js=$(retrato "$W/j"); bs=$(retrato "$W/b")
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (15 s) — programa: $(printf '%s' "$prog" | tr '\n' '|')"
    elif [ "$br" = 124 ]; then
        ok "$label (o próprio bash travou — fora de alcance)"
    elif [ "$js" != "$bs" ]; then
        bad "$label — O DISCO DIVERGIU (dado virou comando)" \
            "prog: $(printf '%s' "$prog" | tr '\n' '|')
        só no bash: [$(comm -23 <(printf '%s\n' "$bs") <(printf '%s\n' "$js") | tr '\n' ' ')]
        só no jash: [$(comm -13 <(printf '%s\n' "$bs") <(printf '%s\n' "$js") | tr '\n' ' ')]"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$label"
    else
        bad "$label" "prog: $(printf '%s' "$prog" | tr '\n' '|')
        jash=[$(printf '%s' "$jo" | tr '\n' '|')] rc=$jr  ≠  bash=[$(printf '%s' "$bo" | tr '\n' '|')] rc=$br"
    fi
}

echo "── A. O REPRODUTOR — as duas linhas que abriram o caso ───────────────"
#
# Estas duas não passam pelo juiz genérico de propósito: elas conferem o
# arquivo PELO NOME, para que a mensagem de falha diga exatamente qual
# comando rodou. As demais seções usam o retrato completo do diretório.

rm -rf "$W/rep"; mkdir -p "$W/rep"
printf "%s\n" "P='dado | tee EVIDENCIA-TEE'" 'echo $P' > "$W/rep/p.sh"
(cd "$W/rep" && "$DEADLINE" 15 "$JASH" ./p.sh >/dev/null 2>&1)
if [ -e "$W/rep/EVIDENCIA-TEE" ]; then
    bad "o \`tee\` nomeado apenas em DADO EXECUTOU" \
        "P='dado | tee EVIDENCIA-TEE'; echo \$P criou o arquivo — o valor virou cano"
else
    ok "P='dado | tee ARQ'; echo \$P NÃO executa o tee (nenhum arquivo criado)"
fi

rm -rf "$W/rep2"; mkdir -p "$W/rep2"
printf "%s\n" "V='dado > EVIDENCIA-REDIR'" 'echo $V' > "$W/rep2/p.sh"
(cd "$W/rep2" && "$DEADLINE" 15 "$JASH" ./p.sh >/dev/null 2>&1)
if [ -e "$W/rep2/EVIDENCIA-REDIR" ]; then
    bad "o redirecionamento que só existia em DADO ACONTECEU" \
        "V='dado > EVIDENCIA-REDIR'; echo \$V criou o arquivo"
else
    ok "V='dado > ARQ'; echo \$V NÃO redireciona (nenhum arquivo criado)"
fi

echo "── B. A MATRIZ — cada metacaractere × cada forma de uso ──────────────"
#
# Toda combinação roda nos dois shells e os retratos do diretório têm que
# bater. Os valores nomeiam um arquivo DIFERENTE por metacaractere, então o
# retrato divergente diz qual deles vazou.

# rótulo::valor  (o valor entra no script entre aspas SIMPLES)
VALORES='
cano::dado | tee EV-cano
redir::dado > EV-redir
append::dado >> ja-existe
lt::dado < dados.txt
err2::dado 2> EV-err2
amp::dado & touch EV-amp
semi::dado ; touch EV-semi
andand::dado && touch EV-and
oror::dado || touch EV-or
crase::dado `touch EV-crase`
cmdsub::dado $(touch EV-cmdsub)
arith::dado $((6*7))
til::~
tilcaminho::~/x
glob::*
globdir::*.txt
paren::dado (touch EV-paren)
canoduplo::a | tee EV-c1 | tee EV-c2
'

# forma::molde  (%s recebe o valor; o molde é impresso com printf)
forma_prog() { # forma  valor
    case "$1" in
        echo)     printf "V='%s'\necho \$V\n" "$2" ;;
        externo)  printf "V='%s'\n/bin/echo \$V\n" "$2" ;;
        aspas)    printf "V='%s'\necho \"\$V\"\n" "$2" ;;
        chaves)   printf "V='%s'\necho \${V}\n" "$2" ;;
        chasp)    printf "V='%s'\necho \"\${V}\"\n" "$2" ;;
        for)      printf "V='%s'\nfor x in \$V; do echo \"[\$x]\"; done\n" "$2" ;;
        colchete) printf "V='%s'\nif [ \$V ]; then echo T; else echo F; fi\n" "$2" ;;
        atrib)    printf "V='%s'\nA=\$V\necho \"[\$A]\"\n" "$2" ;;
        case)     printf "V='%s'\ncase \$V in *) echo M;; esac\n" "$2" ;;
        meio)     printf "V='%s'\necho antes \$V depois\n" "$2" ;;
    esac
}
FORMAS='echo externo aspas chaves chasp for colchete atrib case meio'

while IFS= read -r linha; do
    [ -n "$linha" ] || continue
    rot="${linha%%::*}"; val="${linha#*::}"
    for f in $FORMAS; do
        julgar "$f/$rot" "$(forma_prog "$f" "$val")"
    done
done <<VALEOF
$VALORES
VALEOF

echo "── C. O PAR QUE PROVA — o operador DIGITADO continua sendo estrutura ─"
#
# Sem estas, um conserto que simplesmente nunca mais visse `|` passaria.

julgar 'cano DIGITADO com expansão AINDA é pipeline' \
  'V=zzz
echo $V | tr z Z'
julgar 'dois canos digitados com expansão' \
  'V=zzz
echo $V | tr z Z | tr Z y'
julgar 'redirecionamento DIGITADO com expansão AINDA redireciona' \
  'V=zzz
echo $V > alvo.txt
cat alvo.txt'
julgar 'append DIGITADO com expansão' \
  'V=zzz
echo $V >> ja-existe
cat ja-existe'
julgar 'entrada DIGITADA com expansão' \
  'V=dados.txt
cat < $V'
julgar 'ponto-e-vírgula DIGITADO ainda separa' \
  'V=zzz
echo $V ; touch DIGITADO-SEMI'
julgar 'e-comercial-duplo DIGITADO ainda encadeia' \
  'V=zzz
echo $V && touch DIGITADO-AND'
julgar 'barra-dupla DIGITADA ainda encadeia' \
  'false || touch DIGITADO-OR'
julgar 'cano digitado E valor com cano na MESMA linha' \
  'V="a | tee NAO-DEVE-EXISTIR"
echo $V | tr a A'
julgar 'redirecionamento digitado E valor com redirecionamento' \
  'V="a > NAO-DEVE-EXISTIR"
echo $V > saiu.txt
cat saiu.txt'
julgar 'operador CITADO com barra continua dado (cruzamento JINV-093)' \
  'Z=zzz
echo $Z \| b
echo $Z \> naoCria.txt'

echo "── D. O GLOB EXPANDE — é a outra metade da regra POSIX ───────────────"
#
# Sobre o resultado de uma expansão aplicam-se DUAS coisas: divisão de campos
# e expansão de nome de arquivo. A segunda TEM que continuar acontecendo —
# blindar o `*` seria o bug oposto.

julgar 'valor `*` glob EXPANDE' \
  'V="*"
echo $V'
julgar 'valor `*.txt` glob EXPANDE' \
  'V="*.txt"
echo $V'
julgar 'valor com glob CITADO não expande' \
  'V="*.txt"
echo "$V"'
julgar 'saída de comando com glob EXPANDE' \
  'echo $(printf "%s" "*.txt")'
julgar 'glob no `for` sobre um valor' \
  'V="*.txt"
for f in $V; do echo "[$f]"; done'
# ── e o nome de arquivo que o glob DEVOLVE também é dado ───────────────
# Medido antes do conserto: com um arquivo chamado `a|tee GX` no diretório,
# `echo *` EXECUTAVA o tee. Mesma família, com o glob no lugar da variável.
julgar 'nome de arquivo com cano vindo do glob é DADO' \
  ': > "a|tee EV-globcano"
echo *'
julgar 'nome de arquivo com maior-que vindo do glob é DADO' \
  ': > "a>EV-globredir"
echo *'
julgar 'nome de arquivo com espaço vindo do glob (cruzamento JINV-009)' \
  ': > "dois nomes.txt"
for f in *.txt; do echo "[$f]"; done'

echo "── E. AS OUTRAS PORTAS POR ONDE UM VALOR ENTRA NA LINHA ──────────────"
#
# A variável é a porta mais visível, não a única. Cada uma destas emitia o
# valor por um caminho DIFERENTE no código, e um caminho esquecido é um
# buraco inteiro.

julgar 'saída de $( ) com cano' 'echo $(printf "%s" "a | tee EV-cs")'
julgar 'saída de crase com cano' 'echo `printf "%s" "a | tee EV-cb"`'
julgar 'saída de $( ) com redirecionamento' 'echo $(printf "%s" "a > EV-csr")'
julgar 'saída de $( ) com til' 'echo $(printf "%s" "~")'
julgar 'valor lido de ARQUIVO por read' \
  'printf "a | tee EV-read\n" > entrada.txt
read L < entrada.txt
echo $L'
julgar 'valor lido linha a linha por while read' \
  'printf "a | tee EV-while\n" > entrada.txt
while read L; do echo $L; done < entrada.txt'
julgar 'argumento de FUNÇÃO' \
  'f() { echo $1; }
f "a | tee EV-func"'
julgar 'elemento de array' \
  'A=("a | tee EV-arr")
echo ${A[0]}'
julgar 'array inteiro' \
  'A=("a | tee EV-arr2")
echo ${A[@]}'
julgar 'valor padrão de ${V:-…}' \
  'unset V
echo ${V:-a | tee EV-def}'
julgar 'valor padrão vindo de outra variável' \
  'W="a | tee EV-def2"
echo ${V:-$W}'
julgar 'sufixo removido de um valor' \
  'V="a | tee EV-sufz"
echo ${V%z}'
julgar 'substituição dentro de um valor' \
  'V="aXtee EV-sub"
echo ${V//X/| }'
julgar 'subshell recebe o valor' \
  'V="a | tee EV-sub2"
( echo $V )'
julgar 'valor dentro de if/while/grupo' \
  'V="a | tee EV-blk"
if true; then echo $V; fi
while :; do echo $V; break; done
{ echo $V ; }'

# ── posicionais: a porta mais exposta que existe ──────────────────────
# `./script.sh "$LINHA_DE_FORA"` — o argumento vem de FORA do script.
POSPROG='echo "1=[$1]"
echo "todos=[$@]"
echo $1
echo $@
echo $*'
rm -rf "$W/pj" "$W/pb"; mkdir -p "$W/pj" "$W/pb"
fixture "$W/pj"; fixture "$W/pb"
printf '%s\n' "$POSPROG" > "$W/pj/p.sh"; cp "$W/pj/p.sh" "$W/pb/p.sh"
pj=$( (cd "$W/pj" && "$DEADLINE" 15 "$JASH"   ./p.sh 'a | tee EV-pos' 2>/dev/null) )
pb=$( (cd "$W/pb" && "$DEADLINE" 15 "$ORACLE" ./p.sh 'a | tee EV-pos' 2>/dev/null) )
sj=$(retrato "$W/pj"); sb=$(retrato "$W/pb")
if [ "$sj" = "$sb" ] && [ "$pj" = "$pb" ]; then
    ok "argumento de linha de comando (\$1 / \$@ / \$*) é DADO"
else
    bad "argumento de linha de comando virou comando" \
        "jash=[$(printf '%s' "$pj" | tr '\n' '|')] disco=[$(printf '%s' "$sj" | tr '\n' ' ')]
        bash=[$(printf '%s' "$pb" | tr '\n' '|')] disco=[$(printf '%s' "$sb" | tr '\n' ' ')]"
fi

# ── variável de AMBIENTE: vem de fora do processo ─────────────────────
rm -rf "$W/ej" "$W/eb"; mkdir -p "$W/ej" "$W/eb"
fixture "$W/ej"; fixture "$W/eb"
printf '%s\n' 'echo $EXTERNA' > "$W/ej/p.sh"; cp "$W/ej/p.sh" "$W/eb/p.sh"
ej=$( (cd "$W/ej" && EXTERNA='a | tee EV-env' "$DEADLINE" 15 "$JASH"   ./p.sh 2>/dev/null) )
eb=$( (cd "$W/eb" && EXTERNA='a | tee EV-env' "$DEADLINE" 15 "$ORACLE" ./p.sh 2>/dev/null) )
sj=$(retrato "$W/ej"); sb=$(retrato "$W/eb")
if [ "$sj" = "$sb" ] && [ "$ej" = "$eb" ]; then
    ok "variável de AMBIENTE é DADO (veio de fora do processo)"
else
    bad "variável de ambiente virou comando" \
        "jash=[$ej] disco=[$(printf '%s' "$sj" | tr '\n' ' ')] ≠ bash=[$eb] disco=[$(printf '%s' "$sb" | tr '\n' ' ')]"
fi

echo "── F. \`eval\` CONTINUA transformando dado em sintaxe — UMA volta ──────"
#
# O outro lado do penhasco. `eval` existe exatamente para reanalisar texto;
# se a blindagem sobrevivesse até lá, o `eval` morria. Ela é desfeita pela
# remoção de aspas do POSIX (core/argv.c), que é a porta do próprio eval.

julgar 'eval com cano no valor AINDA canaliza' \
  'CMD="echo aa | tr a A"
eval "$CMD"'
julgar 'eval com redirecionamento no valor AINDA redireciona' \
  'CMD="echo aa > ev.txt"
eval "$CMD"
cat ev.txt'
julgar 'eval sem aspas' \
  'CMD="echo aa | tr a A"
eval $CMD'
julgar 'eval com ponto-e-vírgula' \
  'CMD="echo a; echo b"
eval "$CMD"'
julgar 'eval com til' \
  'CMD="echo ~"
eval "$CMD"'
julgar 'eval desce UMA camada, não até o fundo (cruzamento JINV-090)' \
  'X=oi
V='"'"'$X'"'"'
W='"'"'$V'"'"'
eval "echo [$V]"
eval "echo [$W]"'
julgar 'o valor literal continua literal sem eval (cruzamento JINV-090)' \
  'X=SEGREDO
V='"'"'$X'"'"'
echo "[$V]"
echo [$V]
echo "[$V]" | cat'

echo "── G. O SENTINELA NUNCA VAZA ─────────────────────────────────────────"
#
# A blindagem é feita de bytes de controle. Um ponto de restauro esquecido os
# entrega ao usuário — foi exatamente o que aconteceu com o 0x06 no JINV-033,
# e o nome do arquivo saiu corrompido em silêncio. Esta seção varre a saída
# de tudo que a matriz exercita e exige ZERO byte de controle.

rm -rf "$W/g"; mkdir -p "$W/g"; fixture "$W/g"
cat > "$W/g/p.sh" <<'GEOF'
V='a | tee X > Y < Z & W ; Q ( R ) ~'
echo $V
echo "$V"
echo ${V}
/bin/echo $V
for x in $V; do echo "[$x]"; done
A=$V; echo "[$A]"
case $V in *) echo "[$V]";; esac
echo $V | cat
printf '%s\n' "$V"
echo $(printf '%s' "$V")
set -- "$V"; echo "$1"; echo $1
B=("$V"); echo "${B[0]}"
GEOF
graw=$( (cd "$W/g" && "$DEADLINE" 15 "$JASH" ./p.sh 2>/dev/null) )
gclean=$(printf '%s' "$graw" | LC_ALL=C tr -d '\001-\010\013\014\016-\037')
if [ "$graw" = "$gclean" ]; then
    ok "nenhum byte de controle (sentinela) escapou para a saída"
else
    bad "SENTINELA VAZOU para a saída do usuário" \
        "$(( ${#graw} - ${#gclean} )) byte(s) de controle em: [$(printf '%s' "$graw" | LC_ALL=C tr -c '\040-\176\n' '?' | tr '\n' '|')]"
fi
# e a saída inteira tem que ser a do bash, byte a byte
julgar 'a bateria de sentinela inteira bate com o bash' "$(cat "$W/g/p.sh")"

echo ""
echo "=== JINV-096: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
