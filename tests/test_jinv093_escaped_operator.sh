#!/usr/bin/env bash
# JINV-093 — a barra invertida que segura um OPERADOR sobrevive à expansão.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# `\|` não é um cano. É o byte `|` como DADO — o autor citou, e citação é uma
# ordem: *isto não é estrutura*. A mesma frase vale para `\&`, `\;`, `\<`,
# `\>`, `\(` e `\)`.
#
# O jash decide "esta linha é um pipeline?" (`jash_line_has_pipe`,
# c/parser/pipeline.c) DEPOIS de reescrever a linha com as expansões. A
# varredura respeita `\\` corretamente — mas a passada de parâmetros
# (c/parser/param_expand.c) comia a barra antes, e o que chegava à decisão
# estrutural era um `|` nu, indistinguível de um cano de verdade.
#
# O detalhe que escondeu o defeito por tanto tempo: **só quebrava quando havia
# expansão na linha**. Sem `$`, a passada nem roda, o `\|` chega inteiro ao
# tokenizador e tudo funciona. Então
#
#     echo a \| b          → correto desde sempre
#     Z=z; echo $Z \| b    → o jash tentava executar `b`
#
# É DADO VIRANDO ESTRUTURA por texto reescrito — a família de JINV-016
# (argv[0] injetado), JINV-020 (separador em aspas aninhadas), JINV-028 (`>`
# perdido), JINV-032 (`;;` em aspas), JINV-083 (palavra-chave em aspas) e
# JINV-087 (separador de campo escapado). Em todas, a informação de citação
# EXISTIA e foi descartada antes do ponto que precisava dela; em todas, o
# conserto foi PRESERVAR a marca até lá, nunca filtrar depois.
#
# O conserto é literalmente o vizinho de bloco do `\'`/`\"` (BUG-059) e do
# separador de campo (JINV-087) em param_expand.c: os três dizem a mesma
# coisa — a barra é QUOTAÇÃO, quem a remove é o tokenizador, que tem o
# contexto.
#
# ═══ POR QUE ISTO NÃO É TEORIA ══════════════════════════════════════════
#
# É o idioma `X$0` do autoconf, a proteção clássica contra um valor que começa
# com `-` ser lido como opção. Está nos 19 `configure` desta máquina:
#
#     as_me=`$as_basename "$0" ||
#     $as_expr X/"$0" : '.*/\([^/][^/]*\)/*$' \| \
#              X"$0" : 'X\(//\)$' \| \
#              X"$0" : 'X\(/\)$' \| \
#              .     : '\(.\)' 2>/dev/null || ...`
#
# Aquele `\|` é o argumento `|` do `expr` (alternância), não um cano. O jash
# partia o comando ali e tentava executar `X./configure` — **10 das 38
# invocações do corpus morriam com `X./configure: command not found`**, a 2ª
# maior classe de falha medida.
#
# ═══ A RÉGUA: os dois lados do conserto ═════════════════════════════════
#
# Um conserto que só preservasse a barra quebraria a outra metade: um cano de
# VERDADE numa linha com expansão tem que continuar sendo um cano. Por isso as
# asserções vêm em pares — a citada NÃO é operador, a nua É:
#
#   Z=z; echo $Z \| b     → `z | b`      (dado)
#   Z=z; echo $Z | cat    → `z`          (estrutura)
#   Z=z; echo $Z \> f     → `z > f`      (dado, e NÃO cria o arquivo)
#   Z=z; echo $Z > f      → f contém `z` (estrutura)
#
# Só o par prova a propriedade. Uma metade sozinha passa com o código
# sabotado para o outro lado.
#
# ═══ O ORÁCULO E O CÃO DE GUARDA ════════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc≠124
# concluiria "não travou" de um travamento real. E `command -v timeout` MENTE
# quando quem pergunta é o jash (lá é builtin). A asserção 0 prova que o cão
# está VIVO antes que qualquer coisa confie nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia mede o binário INSTALADO e fica verde contra qualquer mutante. Já mordeu
# três agentes; a prova negativa deste conserto depende disso.
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
# diretório e com o mesmo locale. Arquivo, e não `-c`, porque é assim que um
# `configure` chega — e porque um `-c` passa por mais uma camada de citação do
# shell externo, que é justamente o que está sob teste.
#
# Cada programa roda num diretório VIRGEM: as asserções de `>` conferem se um
# arquivo foi ou não criado, e restos da rodada anterior mentiriam.
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
    # o que sobrou no diretório também é resultado — é como `\>` se distingue
    # de `>` quando a saída dos dois é vazia.
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
# Duas linhas. A primeira cria a expansão; a segunda cita um `|`.
oracle "cano citado + expansão de \$VAR" \
  'Z=zzz
echo $Z \| b'
oracle "cano citado + expansão \"\$VAR\"" \
  'Z=zzz
echo "$Z" \| b'
oracle "cano citado + expansão \${VAR}" \
  'Z=zzz
echo ${Z} \| b'
oracle "cano citado + expansão de posicional" \
  'set -- zzz
echo $1 \| b'
oracle "cano citado + \$? " \
  'true
echo $? \| b'

# ── 2. O PAR QUE PROVA — o cano NU continua sendo estrutura ────────────
# Sem estas, um conserto que simplesmente nunca mais visse `|` passaria.
oracle "cano NU com expansão AINDA é pipeline" \
  'Z=zzz
echo $Z | tr z Z'
oracle "cano NU e cano citado na MESMA linha" \
  'Z=zzz
echo $Z \| b | tr z Z'
oracle "dois canos nus com expansão" \
  'Z=zzz
echo $Z | tr z Z | tr Z y'

# ── 3. O MESMO, para o REDIRECIONAMENTO ────────────────────────────────
# `\>` é dado: não pode criar arquivo. `>` cria. A checagem de `ls` no juiz
# é o que separa os dois quando a saída visível de ambos é igual.
oracle "redirecionamento citado + expansão NÃO cria arquivo" \
  'Z=zzz
echo $Z \> alvo.txt'
oracle "redirecionamento NU com expansão AINDA redireciona" \
  'Z=zzz
echo $Z > alvo.txt
cat alvo.txt'
oracle "menor-que citado + expansão" \
  'Z=zzz
echo $Z \< b'

# ── 4. OS OUTROS OPERADORES CITADOS ────────────────────────────────────
oracle "e-comercial citado + expansão"   'Z=zzz
echo $Z \& b'
oracle "ponto-e-vírgula citado + expansão" 'Z=zzz
echo $Z \; b'
oracle "parênteses citados + expansão"   'Z=zzz
echo $Z \( b \)'
oracle "e-comercial NU com expansão (fundo) ainda separa" \
  'Z=zzz
echo $Z ; echo depois'

# ── 5. CONTROLE: o caminho SEM expansão nunca esteve quebrado ──────────
# Se estas ficarem vermelhas, o conserto quebrou o que já funcionava.
oracle "cano citado SEM expansão (controle)"  'echo a \| b'
oracle "redir citado SEM expansão (controle)" 'echo a \> b'
oracle "cano NU sem expansão (controle)"      'echo a | tr a A'

# ── 6. O IDIOMA `X$0` DO AUTOCONF, VERBATIM ────────────────────────────
# A forma exata que mata 10 das 38 invocações de `configure` do corpus: um
# `expr` com alternância `\|`, quebrado em linhas com barra-invertida, dentro
# de uma substituição por crase de várias linhas.
oracle "autoconf as_me — expr X\"\$0\" com \\| e continuação" \
  'as_expr=expr
V=`$as_expr X"$0" : '"'"'X\(.*\)'"'"' \| \
	 X"$0" : '"'"'X\(.\)'"'"'`
echo "[$V]"'
# ═══ O QUE ESTE GATE NÃO ACUSA (medido, PRÉ-EXISTENTE, não é desta família) ══
#
# A forma COMPLETA do `as_me` do autoconf continua divergindo, e a causa NÃO é
# o operador citado — é um defeito independente, presente igualmente no
# binário anterior a este conserto (medido: mesma saída antes e depois):
#
#     **um `&&`, `||` ou `|` no FIM DA LINHA não continua na linha seguinte.**
#
#     echo first ||            bash: `first`
#     echo second              jash: `first` E `second`  (não curto-circuita)
#
#     false &&                 bash: nada, rc 1
#     echo second              jash: `second`, rc 0
#
#     echo abc |               bash: `Abc`
#     tr a A                   jash: `abc`   ← O CANO SOME, em silêncio, rc 0
#
#     false || {               bash: `dentro`
#       echo dentro            jash: `{: command not found` … `}: command not
#     }                              found`, rc 127 — e o corpo roda MESMO ASSIM
#
# Medido em 21 formas: 9 divergem. As que passam, passam por SORTE — são
# exatamente aquelas em que "rodar a linha seguinte incondicionalmente"
# coincide com o resultado certo (`|| ` com esquerda falsa, `&&` com esquerda
# verdadeira). Não há curto-circuito nenhum; há coincidência.
#
# É o próximo bloqueador medido dos `configure`: é ele que faz o autoconf
# re-executar o script inteiro pela via `. ./$as_me.lineno` (o grupo
# `|| { … }` roda quando deveria ser pulado), e o `$1` da segunda passada
# chega corrompido (`--help` → `--helph`), matando 10 dos 19 arquivos.
#
# Não é asserido aqui porque seria outro conserto, com outra medida, noutro
# arquivo (a junção de linhas mora em `jash_strip_comments_and_joins`,
# c/parser/quotes.c, chamada de c/parser/eval.c). Asserir um defeito alheio
# neste gate faria a prova negativa deste conserto mentir.
oracle "expr com \\| em POSIÇÃO DE ARGUMENTO, uma linha" \
  'Z=abc
expr X"$Z" : '"'"'X\(.*\)'"'"' \| X"$Z" : '"'"'X\(.\)'"'"''

# ── 7. A CITAÇÃO POR ASPAS diz a mesma coisa que a barra ───────────────
# `"|"` e `'"'"'|'"'"'` já funcionavam; entram aqui para que um conserto
# futuro na mesma vizinhança não os derrube junto.
oracle "cano em aspas duplas + expansão"   'Z=zzz
echo $Z "|" b'
oracle "cano em aspas simples + expansão"  'Z=zzz
echo $Z '"'"'|'"'"' b'
oracle "redir em aspas simples + expansão" 'Z=zzz
echo $Z '"'"'>'"'"' b'

echo ""
echo "=== JINV-093: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
