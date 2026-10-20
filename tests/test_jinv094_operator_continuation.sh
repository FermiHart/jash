#!/usr/bin/env bash
# JINV-094 — o OPERADOR no fim da linha PEDE a linha seguinte.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
#     echo abc |          bash: `ABC`
#     tr a A              jash: `abc`   ← O CANO SOME, em silêncio, rc 0
#
# Um cano é ESTRUTURA. Ele foi escrito, foi perdido, e o shell devolveu rc 0
# com a saída do lado esquerdo — que passa adiante como se fosse o resultado
# certo. É a décima primeira aparição da mesma família: JINV-016 (argv[0]
# injetado), JINV-020 (separador em aspas aninhadas), JINV-026 (linha longa),
# JINV-028 (`>` perdido), JINV-032 (`;;` em aspas), JINV-083 (palavra-chave em
# aspas), JINV-085 (padrão de `case`), JINV-087 (separador de campo escapado),
# JINV-093 (`\|` virando cano). Em todas, ESTRUTURA sumiu calada; em todas, o
# conserto foi ler o texto UMA vez, com consciência de aspas, no momento em que
# a informação ainda existe.
#
# ═══ POR QUE NÃO CABIA NO JINV-071 ══════════════════════════════════════
#
# O jash já sabia juntar `\`+nova-linha (JINV-071). Não é a mesma regra:
#
#   `\`+nova-linha  → junção LÉXICA: os DOIS bytes somem da entrada.
#   operador no fim → continuação SINTÁTICA: o operador FICA, some só a
#                     nova-linha (e o branco/comentário que sobrou, e as
#                     linhas em branco seguintes).
#
# E não é toda pontuação: `;` e `&` sozinho FECHAM o enunciado no bash. As
# asserções da seção 3 existem para que um conserto largo demais — "toda linha
# que acaba em pontuação continua" — fique VERMELHO.
#
# ═══ ONDE MORA, E POR QUE ALI ═══════════════════════════════════════════
#
# `jash_join_operator_lines` (c/parser/quotes.c), chamada de
# `jash_run_multiline` (c/main.c) sobre o PROGRAMA INTEIRO, logo depois do
# pré-processamento de heredoc.
#
# Não no avaliador: quando `jash_eval_line` recebe `echo abc |`, o corte em
# linhas lógicas já aconteceu e a nova-linha não existe mais para ser removida.
# A decisão tem que vir ANTES do corte. Uma passada só sobre o texto todo
# conserta topo, corpo de bloco, corpo de `$( )` e de crase — por isso a seção
# 4 e a 7 asseveram os quatro contra o mesmo binário: se algum precisasse de um
# segundo analisador, o conserto estaria errado.
#
# Depois do heredoc de propósito: os corpos já viraram arquivos temporários,
# então um `|` no fim de uma linha de DADO de heredoc (seção 3) não pode ser
# confundido com estrutura.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria — inclusive as que o autor
# achava óbvias: foi assim que se descobriu que `echo a &` + `echo b` NÃO
# continua (ambos rodam) e que `true ||` + `echo yes` NÃO imprime nada.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc≠124
# concluiria "não travou" de um travamento real. A asserção 0 prova que o cão
# está VIVO antes que qualquer coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binário sem o conserto> deixa este gate
# VERMELHO (JINV-061). Registrado em SRC_MUTANTS de tools/mutation-check.sh.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
export JASH_ABSORB=0
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# ── o juiz ─────────────────────────────────────────────────────────────
# Escreve o programa num ARQUIVO e roda os dois shells sobre ele, no mesmo
# diretório virgem e com o mesmo locale. Arquivo, e não `-c`, porque é assim
# que um `configure` chega — e porque `-c` passa por mais uma camada de citação
# do shell externo, que é justamente o que está sob teste.
#
# O rc entra no veredito porque metade desta propriedade É o rc: `false &&` +
# `echo yes` tem que sair 1 e mudo, e o jash saía 0 com `yes`.
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br jd bd
    jd="$W/j"; bd="$W/b"
    rm -rf "$jd" "$bd"; mkdir -p "$jd" "$bd"
    printf '%s\n' "$prog" > "$jd/p.sh"; cp "$jd/p.sh" "$bd/p.sh"
    # stderr DESCARTADO, nunca fundido (JINV-061): o jash escreve avisos de UX
    # no stderr real, com contador PERSISTENTE entre invocações — fundi-los na
    # captura faz o veredito depender do contador. O que esta propriedade
    # afirma vive no stdout, no rc e no que sobrou no diretório.
    jo=$( (cd "$jd" && LC_ALL=C LANG=C "$DEADLINE" 10 "$JASH"   ./p.sh 2>/dev/null) ); jr=$?
    bo=$( (cd "$bd" && LC_ALL=C LANG=C "$DEADLINE" 10 "$ORACLE" ./p.sh 2>/dev/null) ); br=$?
    # o que sobrou no diretório também é resultado — é como um redirecionamento
    # que aconteceu se distingue de um que não aconteceu quando as duas saídas
    # visíveis são vazias.
    jo="${jo}§$( (cd "$jd" && ls | tr '\n' ',') )"
    bo="${bo}§$( (cd "$bd" && ls | tr '\n' ',') )"
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (10 s) — programa: $(printf '%s' "$prog" | tr '\n' '|')"
    elif [ "$br" = 124 ]; then
        ok "$label (o próprio bash travou — fora de alcance)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$label"
    else
        bad "$label" "prog: $(printf '%s' "$prog" | tr '\n' '|')
        jash=[$(printf '%s' "$jo" | tr '\n' '|')] rc=$jr  ≠  bash=[$(printf '%s' "$bo" | tr '\n' '|')] rc=$br"
    fi
}

# ── 0. ANTI-INÉRCIA: o cão de guarda está vivo? ────────────────────────
"$DEADLINE" 2 sleep 30 >/dev/null 2>&1
if [ $? = 124 ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "watchdog inerte" \
        "deadline.sh 2 sleep 30 não devolveu 124 — as asserções de prazo estão sem valor"
fi

# ── 1. O DEFEITO, na sua forma mínima ──────────────────────────────────
# Duas linhas. A primeira acaba em operador; a segunda é o outro lado dele.
oracle "cano no fim da linha" \
  'echo abc |
tr a A'
oracle "E-lógico no fim da linha, esquerda FALSA (curto-circuito)" \
  'false &&
echo yes'
oracle "OU-lógico no fim da linha, esquerda VERDADEIRA (curto-circuito)" \
  'true ||
echo yes'
oracle "E-lógico no fim da linha, esquerda VERDADEIRA (roda)" \
  'true &&
echo yes'
oracle "OU-lógico no fim da linha, esquerda FALSA (roda)" \
  'false ||
echo yes'
# As duas de cima passavam ANTES do conserto — por SORTE, não por
# curto-circuito: "rodar a linha seguinte incondicionalmente" coincide com o
# resultado certo quando o operador não corta. Ficam aqui para que o conserto
# não as derrube.

# ── 2. O OPERADOR ATRAVESSA O QUE NÃO É COMANDO ────────────────────────
# Comentário, linha em branco e linha só-de-comentário somem junto com a
# nova-linha (bash). O rc de `false &&` é o que separa "atravessou" de
# "engoliu a linha seguinte no comentário".
oracle "cano + comentário na mesma linha" \
  'echo abc | # nota
tr a A'
oracle "cano + UMA linha em branco" \
  'echo abc |

tr a A'
oracle "cano + TRÊS linhas em branco" \
  'echo abc |



tr a A'
oracle "E-lógico + comentário (curto-circuito preservado)" \
  'false && # nota
echo yes'
oracle "E-lógico + linha em branco (curto-circuito preservado)" \
  'false &&

echo yes'
oracle "OU-lógico + linha INTEIRA de comentário" \
  'true ||
# nota
echo yes'
oracle "cano + comentário + linha em branco + comentário" \
  'echo abc | # um

# dois
tr a A'

# ── 3. O PAR QUE PROVA — o que NÃO continua ────────────────────────────
# Sem esta seção, um conserto largo demais ("toda linha que acaba em pontuação
# continua") passaria. As três formas abaixo FECHAM o enunciado no bash.
oracle "ponto-e-vírgula no fim NÃO é continuação" \
  'echo a ;
echo b'
oracle "e-comercial sozinho (segundo plano) NÃO é continuação" \
  'echo a &
wait
echo b'
oracle "linha completa seguida de outra (controle)" \
  'echo a
echo b'

# ── 4. O OPERADOR COMO DADO — citado nunca continua ────────────────────
# A informação de citação tem que sobreviver até a decisão. Estas são o lado
# "não é estrutura" do par; sem elas, um conserto que ignorasse aspas passaria.
oracle "cano em aspas DUPLAS é dado" \
  'echo "abc |"
echo depois'
oracle "cano em aspas SIMPLES é dado" \
  "echo 'abc |'
echo depois"
oracle "cano ESCAPADO é dado" \
  'echo abc \|
echo depois'
oracle "E-lógico em aspas duplas é dado" \
  'echo "a &&"
echo depois'
oracle "cano dentro de COMENTÁRIO é dado" \
  'echo a # b |
echo depois'
oracle "cano dentro de HEREDOC é dado" \
  'cat <<EOF
abc |
EOF
echo depois'
oracle "cano dentro de heredoc CITADO é dado" \
  "cat <<'EOF'
abc |
EOF
echo depois"
oracle "cano dentro de aspas simples de VÁRIAS LINHAS é dado" \
  "echo 'abc |
def'
echo depois"

# ── 5. DENTRO DE TODA CONSTRUÇÃO COMPOSTA ──────────────────────────────
# O corpo de um bloco é lido por outro caminho que o topo. Se o conserto
# morasse no avaliador (depois do corte em linhas), esta seção ficaria
# vermelha e a de cima verde — é o que a distingue.
oracle "cano no fim da linha dentro de if" \
  'if true; then
echo abc |
tr a A
fi'
oracle "cano no fim da linha dentro de for" \
  'for i in 1 2; do
echo a$i |
tr a A
done'
oracle "cano no fim da linha dentro de while" \
  'i=0
while [ $i -lt 2 ]; do
i=$((i+1))
echo a |
tr a A
done'
oracle "cano no fim da linha dentro de função" \
  'f() {
echo abc |
tr a A
}
f'
oracle "cano no fim da linha dentro de case" \
  'case x in
x)
echo abc |
tr a A
;;
esac'
oracle "cano no fim da linha dentro de grupo de chaves" \
  '{
echo abc |
tr a A
}'
oracle "cano no fim da linha dentro de subshell" \
  '(
echo abc |
tr a A
)'
oracle "curto-circuito dentro de if" \
  'if true; then
false &&
echo yes
echo fim
fi'
oracle "OU-lógico em duas linhas DENTRO da condição de if" \
  'if false ||
   true; then echo T; fi'
oracle "E-lógico em duas linhas DENTRO da condição de if" \
  'if true &&
   false; then echo T; else echo F; fi'

# ── 6. COMBINADO COM A JUNÇÃO LÉXICA (JINV-071) ────────────────────────
# As duas regras se cruzam no mesmo texto e são de naturezas diferentes. Se
# uma passada refizesse o trabalho da outra, uma destas quebraria.
oracle "cano + barra-invertida na MESMA linha (já funcionava)" \
  'echo abc | \
tr a A'
oracle "E-lógico + barra-invertida (já funcionava)" \
  'true && \
echo yes'
oracle "cano no fim, e a linha seguinte usa barra-invertida" \
  'echo abc |
tr a \
A'
oracle "barra-invertida junta, e a linha juntada acaba em cano" \
  'echo \
abc |
tr a A'

# ── 7. CADEIAS, CANOS DE VERDADE E SUBSTITUIÇÃO ────────────────────────
oracle "dois canos, os dois no fim da linha" \
  'echo abc |
tr a A |
tr b B'
oracle "E-lógico e OU-lógico encadeados em linhas" \
  'false &&
echo A ||
echo B'
oracle "OU-lógico e E-lógico encadeados em linhas" \
  'true ||
echo A &&
echo B'
oracle "cano no fim + redirecionamento na linha seguinte" \
  'echo abc |
tr a A > alvo.txt
cat alvo.txt'
oracle "cano no fim dentro de \$( )" \
  'V=$(echo abc |
tr a A)
echo "[$V]"'
oracle "cano no fim dentro de crase" \
  'V=`echo abc |
tr a A`
echo "[$V]"'
oracle "cano NU numa linha só ainda é cano (controle)" \
  'echo abc | tr a A'
oracle "cano NU com expansão ainda é cano (controle)" \
  'Z=abc
echo $Z | tr a A'
# `${#VAR}` — a armadilha que este conserto pisou ao nascer. O `${` abre
# fronteira de palavra, então o `#` do comprimento tinha fronteira à esquerda e
# virava COMENTÁRIO: o resto da linha ficava inerte, o `}` não fechava a pilha,
# e o cano depois dele não era visto. A guarda é `cdep == 0` (o mesmo BUG-100
# que o irmão desta passada já respeitava).
oracle "\${#VAR} não abre comentário, e o cano depois dele continua" \
  'V=abcdef
echo ${#V} |
tr 6 X'
oracle "\${VAR#padrão} não abre comentário" \
  'V=abcdef
echo ${V#abc} |
tr d D'

# ── 8. O CASO DE ERRO: operador no FIM DO ARQUIVO ──────────────────────
# Sem linha seguinte não há continuação: o bash dá `syntax error: unexpected
# end of file`, rc 2, e NADA roda — nem o lado esquerdo. Executar `echo abc`
# de um cano que não existe é exatamente a mentira que este invariante mata.
# Estas asserções são o que impede um conserto que apenas "come a nova-linha".
oracle "cano no fim do ARQUIVO é erro de sintaxe" \
  'echo abc |'
oracle "E-lógico no fim do ARQUIVO é erro de sintaxe" \
  'echo abc &&'
oracle "OU-lógico no fim do ARQUIVO é erro de sintaxe" \
  'echo abc ||'
oracle "cano no fim + só um comentário depois é erro de sintaxe" \
  'echo abc |
# nada'
oracle "cano no fim + só linhas em branco depois é erro de sintaxe" \
  'echo abc |

'
oracle "e-comercial sozinho no fim do arquivo NÃO é erro" \
  'echo abc &'
oracle "ponto-e-vírgula no fim do arquivo NÃO é erro" \
  'echo abc ;'

# ── 9. O IDIOMA DO AUTOCONF, VERBATIM ──────────────────────────────────
# É esta forma — o grupo `|| { … }` e o `expr … \| …` quebrado em linhas — que
# faz o `configure` reentrar em si mesmo por `. ./$as_me.lineno`.
oracle "expr do autoconf com \\| e continuação por barra" \
  'as_expr=expr
V=`$as_expr X"$0" : '"'"'X\(.*\)'"'"' \| \
	 X"$0" : '"'"'X\(.\)'"'"'`
echo "[$V]"'
oracle "cadeia de teste em linhas, idioma as_fn do autoconf" \
  'test -z "" &&
test -n "x" &&
echo ambos'
oracle "OU-lógico no fim + comando composto na linha seguinte" \
  'false ||
if true; then echo dentro; fi'

# ═══ O QUE ESTE GATE NÃO ACUSA (medido, PRÉ-EXISTENTE, outra família) ══════
#
# Medido com o binário ANTERIOR a este conserto e com o atual: MESMA saída nos
# dois. Não é regressão desta onda e não é asserido aqui, porque asseverar
# defeito alheio faria a prova negativa deste conserto mentir.
#
# 1. `{ … }` como LADO DIREITO de `||`/`&&`, na mesma linha:
#
#        false || {          bash: `dentro`, rc 0
#          echo dentro       jash: `{: command not found` … `}: command not
#        }                         found`, rc 127 — e o corpo roda MESMO ASSIM
#
#    O grupo de chaves só entra no acumulador quando ABRE a linha (JINV-084);
#    depois de um operador ele volta a ser lido como nome de programa. É o
#    idioma `as_fn_*` do autoconf. `false || (…)` (parênteses) já funciona, o
#    que isola o defeito no `{`.
#
# 2. `\&` no fim da linha ainda é lido como SEGUNDO PLANO:
#
#        echo a \&           bash: `a &` depois `depois`
#        echo depois         jash: `depois` depois `a \` — e em segundo plano
#
#    A barra sobrevive à decisão de cano/redirecionamento (JINV-093) mas não à
#    de `&`. Mesma família, outro ponto de decisão.
#
# 3. O MODO INTERATIVO não continua. `jash_join_operator_lines` roda em
#    `jash_run_multiline` — arquivo, `-c` multi-linha, `source`, `eval`. A REPL
#    lê e avalia UMA linha por vez (c/readline/loop.c), então digitar
#    `echo abc |` + Enter ainda executa o lado esquerdo em vez de pedir a
#    continuação como o bash faz com o `> `. Não foi feito aqui de propósito:
#    a REPL tem o próprio detector de "falta entrada" (`quotes_open`) e enxertar
#    a regra lá seria a SEGUNDA leitura da mesma coisa — exatamente o padrão que
#    este repo já matou dez vezes. O conserto certo é a REPL passar a consultar
#    esta função; é outro commit, com outra medida.

echo ""
echo "=== JINV-094: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
