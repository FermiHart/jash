#!/usr/bin/env bash
# JINV-086 — corpo de funcao e valor de variavel: nenhum teto MUDO.
#
# ═══ A PROPRIEDADE ═══
#
#   Nenhum estagio pode entregar um resultado CORTADO com cara de sucesso.
#   Ou o dado passa inteiro, ou o shell RECUSA ALTO (stderr + rc != 0).
#   Nao existe terceira opcao aceitavel.
#
# ═══ POR QUE TRUNCAR CALADO E PIOR QUE FALHAR ═══
#
#   Um comando que FALHA para o trabalho no ponto do erro. Um comando que
#   trunca em silencio devolve rc 0, stderr vazio, e um resultado PLAUSIVEL E
#   ERRADO — e quem chamou segue trabalhando em cima dele. O erro aparece
#   muito depois, longe da causa, e a evidencia do que aconteceu ja sumiu.
#
#   No caso de uma FUNCAO isso e ainda pior que na maioria dos buffers desta
#   familia, e por uma razao especifica: o que se perde num corte e sempre o
#   FINAL, e o final de um corpo de funcao continua sintaticamente valido.
#   Um `for` cortado grita `missing done` (JINV-080); uma funcao decapitada
#   nao tem como gritar — ela simplesmente faz menos do que foi escrito.
#   `declare -f` chegava a IMPRIMIR o corpo inteiro enquanto a execucao rodava
#   so a metade de cima, o que engana ate quem vai inspecionar.
#
#   E o mesmo vale para o VALOR de uma variavel: quando a cauda cortada
#   carregava um `|`, um `> arquivo` ou um `&&` ainda nao expandido, o comando
#   executado deixa de ser o comando escrito. Foi assim que o corpo de um
#   heredoc virou comandos (JINV-022), que um `| wc -c` sumiu (JINV-026) e que
#   um `> arquivo` sumiu em 1 KB (JINV-028).
#
# ═══ OS PENHASCOS MEDIDOS (antes do conserto) ═══
#
#   corpo de funcao        1835 B ok  ·  2735 B perde a cauda, rc 0, mudo
#   V=<literal>            1023 B ok  ·  1024 B vira 1023, rc 0, mudo
#   readonly V=<literal>    254 B                 · e a variavel fica IMUTAVEL
#                                                   com o valor errado
#   export V=<literal>     8191 B (JASH_ENV_VAL_LEN - 1)
#   local V=<literal>      4095 B
#   A[0]=<literal>         1023 B
#
#   Eram TRES tetos em serie no caminho de funcao — parse (`fbody[2048]` em
#   parser/eval.c), armazenamento (`body[2048]` em function_defs.h) e CHAMADA
#   (`malloc(2048)` em parser/eval.c) — e consertar um so nao move nada.
#
# ═══ O CONSERTO ═══
#
#   Idioma do JINV-080/081/082: o tamanho antigo vira LIMIAR DE DESEMPENHO
#   (pilha/inline, zero malloc no caso comum), heap do tamanho exato acima,
#   porta de saida unica para nao vazar, e OOM = recusa ALTA. Nenhum teto novo
#   de dado foi criado; o unico numero declarado (JASH_FN_BODY_INLINE) e
#   verificado contra core/limits.h em tempo de compilacao.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real, e um gate que a leia passa
# verde contra qualquer mutante. Ja mordeu tres agentes.
#
# ORACULO: /bin/bash em toda asercao — nenhum numero deste gate foi escolhido
# a mao, todos vem do bash rodando o MESMO arquivo.
#
# CAO DE GUARDA: tools/deadline.sh, nunca `timeout` — timeout(1) NAO EXISTE
# nesta maquina e devolve 127 sob bash, o que faz o teste concluir "nao
# travou" quando travou. `command -v timeout` mente (no jash e builtin).
#
# Gate: bash tests/test_jinv086_silent_caps.sh

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

# O cao de guarda tem que estar VIVO antes de qualquer asercao depender dele.
"$DL" 2 /bin/sleep 5 >/dev/null 2>&1
[ $? = 124 ] || { echo "FAIL: deadline.sh nao protege (rc != 124)"; exit 1; }

# `rep N` — N bytes de 'a'. Via awk porque a linha de comando nao aguenta
# 100 KB de argumento (`Argument list too long` mata o gate sem explicar).
rep() { awk -v n="$1" 'BEGIN{s="";while(length(s)<n)s=s "a";printf "%s", substr(s,1,n)}'; }

jout() { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 90 /bin/bash ./t.sh 2>/dev/null); }
jerr() { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh 2>&1 >/dev/null); }
jrc()  { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 90 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

# A asercao central: identico ao bash, saida E rc.
igual() {
    local desc="$1" jo bo jr br
    jo=$(jout); jr=$(jrc)
    bo=$(bout); br=$(brc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 90s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr [$(printf '%s' "$jo" | head -c 90 | tr '\n' '/')] != bash rc=$br [$(printf '%s' "$bo" | head -c 90 | tr '\n' '/')]"
    fi
}

# A asercao dos tamanhos EXTREMOS (100 KB): acima de um certo ponto o jash tem
# tetos DECLARADOS de outros estagios (expansao de linha, JASH_LINE_CAP_STACK).
# Divergir ali e legitimo; divergir EM SILENCIO nao e. Esta e a propriedade
# que o invariante realmente protege — "ou cresce, ou recusa alto".
igual_ou_alto() {
    local desc="$1" jo bo je jr
    jo=$(jout); bo=$(bout); je=$(jerr); jr=$(jrc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 90s)"
    elif [ "$jo" = "$bo" ]; then
        ok "$desc (identico ao bash)"
    elif [ -n "$je" ]; then
        ok "$desc (divergiu mas RECUSOU ALTO: $(printf '%s' "$je" | head -c 60 | tr '\n' ' '))"
    else
        bad "$desc" "divergiu do bash EM SILENCIO — stderr vazio, rc=$jr; jash=[$(printf '%s' "$jo" | head -c 70 | tr '\n' '/')] bash=[$(printf '%s' "$bo" | head -c 70 | tr '\n' '/')]"
    fi
}

# ── gerador de corpo de funcao com N linhas de recheio ──────────────────
corpo() { # $1 = n linhas
    local i=0
    while [ "$i" -lt "$1" ]; do
        printf '  : recheio_%s_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n' "$i"
        i=$((i+1))
    done
}

echo "── A. CORPO DE FUNCAO — os dois lados do penhasco de 2048 B ──────────"

# 40 linhas ~ 1810 B de corpo (ABAIXO); 60 linhas ~ 2710 B (ACIMA).
for nl in 40 60; do
    { printf 'f() {\n  echo INICIO\n'; corpo "$nl"; printf '  echo FIM\n}\nf\n'; } > "$SB/t.sh"
    igual "corpo simples, $(wc -c < "$SB/t.sh" | tr -d ' ') B de arquivo"
done

# A cauda perdida tem que ser observavel pelo CONTEUDO, nao so pelo tamanho:
# a ultima linha do corpo e a que some primeiro.
{ printf 'f() {\n  echo INICIO\n'; corpo 60; printf '  echo CAUDA_VIVA\n}\nf\n'; } > "$SB/t.sh"
igual "a ULTIMA linha do corpo continua executando (2.7 KB)"

# `declare -f` e a execucao tem que concordar. Enquanto o teto vivia so na
# CHAMADA, `declare -f` imprimia o corpo INTEIRO e a execucao rodava metade —
# um gate que olhasse so para `declare -f` passaria verde com o bug vivo.
#
# A contagem e de LINHAS de recheio, nao de bytes: o formato de `declare -f`
# diverge do bash (indentacao das chaves) e comparar tamanho mediria o
# formato, nao o conteudo. `grep -c` mede exatamente o que importa — quantas
# linhas do corpo sobreviveram — e continua sendo o bash o oraculo do numero.
{ printf 'f() {\n  echo INICIO\n'; corpo 60; printf '  echo CAUDA_VIVA\n}\ndeclare -f f | grep -c recheio_\nf\n'; } > "$SB/t.sh"
igual "declare -f mostra as 60 linhas do corpo, e a execucao concorda"

echo "── B. CORPO DE FUNCAO — as formas que um script real usa ─────────────"

{ printf 'f() {\n  echo INICIO\n  if [ 1 -eq 1 ]; then\n'; corpo 80
  printf '    echo DENTRO_IF\n  fi\n  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual "corpo com if dentro ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'f() {\n  echo INICIO\n  for i in 1 2 3; do\n'; corpo 80
  printf '    echo IT\n  done\n  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual "corpo com for dentro ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'f() {\n  echo INICIO\n  case abc in\n    abc)\n'; corpo 80
  printf '      echo CASOU\n      ;;\n    *)\n      echo NAO\n      ;;\n  esac\n  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual "corpo com case dentro ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'f() {\n  echo INICIO\n  n=0\n  while [ $n -lt 2 ]; do\n'; corpo 80
  printf '    n=$((n+1))\n  done\n  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual "corpo com while dentro ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'f() {\n  if [ "$1" -le 0 ]; then echo BASE; return 0; fi\n'; corpo 60
  printf '  f $(( $1 - 1 ))\n  echo NIVEL$1\n}\nf 3\n'; } > "$SB/t.sh"
igual "funcao RECURSIVA de corpo grande ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'outer() {\n  echo OUTER_INICIO\n  inner() {\n'; corpo 60
  printf '    echo INNER_FIM\n  }\n  inner\n  echo OUTER_FIM\n}\nouter\n'; } > "$SB/t.sh"
igual "funcao ANINHADA (define outra no corpo, $(wc -c < "$SB/t.sh" | tr -d ' ') B)"

{ printf 'function f {\n  echo INICIO\n'; corpo 60; printf '  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual "palavra-chave 'function' sem () ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

# Corpo numa LINHA SO, acima do teto: o `;` nao pode salvar nem condenar.
printf 'f() { echo INICIO; : %s; echo FIM; }\nf\n' "$(rep 2500)" > "$SB/t.sh"
igual "corpo de UMA linha acima do teto (2.5 KB)"

# Redefinicao: o corpo velho no heap tem que ser trocado, nao acumulado nem
# usado depois de liberado.
{ printf 'f() { echo VELHO; : %s; }\n' "$(rep 3000)"
  printf 'f() { echo NOVO; : %s; }\n' "$(rep 4000)"
  printf 'f\n'; } > "$SB/t.sh"
igual "redefinir uma funcao de corpo grande usa o corpo NOVO"

{ printf 'i=0\nwhile [ $i -lt 25 ]; do\n'
  printf '  f() { echo VOLTA; : %s; }\n' "$(rep 5000)"
  printf '  i=$((i+1))\ndone\nf\n'; } > "$SB/t.sh"
igual "redefinir 25x um corpo de 5 KB nao corrompe nem trava"

# unset + redefine: exercita a posse do heap no delete (o slot vago nao pode
# ficar com uma copia viva do ponteiro).
{ printf 'f() { echo UM; : %s; }\n' "$(rep 3000)"
  printf 'g() { echo DOIS; : %s; }\n' "$(rep 3000)"
  printf 'unset -f f\n'
  printf 'h() { echo TRES; : %s; }\n' "$(rep 3000)"
  printf 'g\nh\n'; } > "$SB/t.sh"
igual "unset -f de corpo grande nao leva as outras funcoes junto"

# Funcao COM REDIRECT — outro caminho de execucao (parser/builtin_redir.c),
# que tinha o seu proprio teto por LINHA de corpo.
{ printf 'f() {\n  echo INICIO\n'; corpo 80
  printf '  echo FIM\n}\nf > saida.txt\ncat saida.txt\n'; } > "$SB/t.sh"
igual "funcao com redirect '> arquivo' ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

# Funcao num CANO — terceiro caminho (parser/pipeline.c).
{ printf 'f() {\n  echo INICIO\n'; corpo 80
  printf '  echo FIM\n}\nf | cat\n'; } > "$SB/t.sh"
igual "funcao dentro de um cano ($(wc -c < "$SB/t.sh" | tr -d ' ') B)"

echo "── C. VALOR DE VARIAVEL — os dois lados do penhasco de 1023 B ────────"

# 1023 = ultimo tamanho que ja funcionava; 1024 = o primeiro que sumia.
# O comprimento e medido com ${#V}, nao pela saida: uma saida truncada e
# indistinguivel de uma saida curta, o comprimento nao e.
for n in 1023 1024 2000; do
    printf 'V=%s\necho ${#V}\n' "$(rep $n)"      > "$SB/t.sh"; igual "V=<literal $n>            ${n}"
    printf 'V="%s"\necho ${#V}\n' "$(rep $n)"    > "$SB/t.sh"; igual "V=\"<literal $n>\"          ${n}"
    printf "V='%s'\necho \${#V}\n" "$(rep $n)"   > "$SB/t.sh"; igual "V='<literal $n>'          ${n}"
    printf 'export V=%s\necho ${#V}\n' "$(rep $n)"   > "$SB/t.sh"; igual "export V=<literal $n>     ${n}"
    printf 'readonly V=%s\necho ${#V}\n' "$(rep $n)" > "$SB/t.sh"; igual "readonly V=<literal $n>   ${n}"
    printf 'g() { local V=%s; echo ${#V}; }\ng\n' "$(rep $n)" > "$SB/t.sh"; igual "local V=<literal $n>      ${n}"
    printf 'A[0]=%s\necho ${#A[0]}\n' "$(rep $n)"    > "$SB/t.sh"; igual "A[0]=<literal $n>         ${n}"
    printf 'V=x\nV+=%s\necho ${#V}\n' "$(rep $n)"    > "$SB/t.sh"; igual "V+=<literal $n>           ${n}"
    printf 'A=%s B=%s\necho $((${#A}+${#B}))\n' "$(rep $n)" "$(rep $n)" > "$SB/t.sh"
    igual "A=<$n> B=<$n> multi-atribuicao"
done

# 8191/8192: o penhasco do `export` (JASH_ENV_VAL_LEN - 1), um degrau acima.
for n in 8191 8192; do
    printf 'export V=%s\necho ${#V}\n' "$(rep $n)" > "$SB/t.sh"; igual "export V=<literal $n> (teto de JASH_ENV_VAL_LEN)"
    printf 'V="%s"\necho ${#V}\n' "$(rep $n)"      > "$SB/t.sh"; igual "V=\"<literal $n>\" (remocao de aspas acima de 8 KiB)"
done

# 4095/4096: o penhasco do `local`.
for n in 4095 4096; do
    printf 'g() { local V=%s; echo ${#V}; }\ng\n' "$(rep $n)" > "$SB/t.sh"
    igual "local V=<literal $n> (teto do unquote de local)"
done

echo "── D. VALOR DE VARIAVEL — as formas que mudam o caminho de analise ───"

# Escape: a barra invertida faz o valor passar pela remocao de aspas, que
# tinha o SEU proprio teto um degrau acima do buffer de valor.
printf 'V=%s\\ %s\necho ${#V}\n' "$(rep 2000)" "$(rep 5)" > "$SB/t.sh"
igual "V=<2000>\\ <5>  (escape leva ao caminho do unquote)"

printf 'V="%s\\"%s"\necho ${#V}\n' "$(rep 2000)" "$(rep 5)" > "$SB/t.sh"
igual "V=\"<2000>\\\"<5>\"  (aspa escapada dentro de aspas)"

# Aspas MISTAS: o valor nao e um par cercando tudo.
printf "V=%s'%s'%s\necho \${#V}\n" "$(rep 900)" "$(rep 900)" "$(rep 900)" > "$SB/t.sh"
igual "V=<900>'<900>'<900>  (aspas parciais, 2700 B)"

# Substituicao de comando: o valor vem DEPOIS do buffer de analise, entao
# este caso continuava certo enquanto o literal estava errado — e por isso
# ele so prova alguma coisa AO LADO dos literais acima.
rep 20000 > "$SB/dados.txt"
printf 'V=$(cat dados.txt)\necho ${#V}\n' > "$SB/t.sh"
igual "V=\$(cat arquivo de 20 KB)"

printf 'V="$(cat dados.txt)"\necho ${#V}\n' > "$SB/t.sh"
igual "V=\"\$(cat arquivo de 20 KB)\""

# Valor com quebras de linha (o corte cai no meio de uma linha).
{ printf 'V="linha1\n'; corpo 60; printf 'ultima"\necho ${#V}\n'; } > "$SB/t.sh"
igual "V com quebras de linha, 2.7 KB"

# Duas atribuicoes grandes na MESMA linha: o buffer e reusado entre elas,
# entao a segunda nao pode herdar o tamanho da primeira nem vice-versa.
printf 'A=%s B=%s\necho ${#A}\necho ${#B}\n' "$(rep 3000)" "$(rep 50)" > "$SB/t.sh"
igual "A=<3000> B=<50> — a pequena depois da grande"
printf 'A=%s B=%s\necho ${#A}\necho ${#B}\n' "$(rep 50)" "$(rep 3000)" > "$SB/t.sh"
igual "A=<50> B=<3000> — a grande depois da pequena"

# Atribuicao seguida de comando (o caminho que reentra no avaliador).
printf 'V=%s\necho ${#V}\n' "$(rep 3000)" > "$SB/t.sh"
igual "V=<3000> seguida de outro enunciado (reentrada)"

# Prefixo inline `VAR=v cmd`: o valor grande tem que CHEGAR no comando e o
# antigo tem que voltar depois. Os dois lados sao medidos separadamente
# porque `${#V}` nos ARGUMENTOS do proprio comando prefixado diverge do bash
# por outro motivo (ordem de expansao vs. aplicacao do prefixo) — uma
# divergencia que ja existia e que nao e de tamanho: ela aparece igual com um
# valor de 5 bytes. Misturar as duas coisas numa asercao so faria este gate
# acusar o defeito errado.
printf 'V=antigo\nV=%s true\necho ${#V}\n' "$(rep 3000)" > "$SB/t.sh"
igual "V=<3000> cmd (prefixo inline) restaura o valor antigo"

printf 'V=antigo\nV=%s /bin/sh -c "echo \${#V}"\necho ${#V}\n' "$(rep 3000)" > "$SB/t.sh"
igual "V=<3000> cmd — o valor INTEIRO chega no comando prefixado"

echo "── E. O QUE A PORTA UNICA DE SAIDA NAO PODE QUEBRAR ──────────────────"

# Trocar sete `return` por uma porta unica (`assign_done`) e o que torna o
# conserto seguro contra vazamento — e e tambem a chance de engolir um
# comando. "A atribuicao ABORTOU" e "a atribuicao terminou com rc != 0" sao
# coisas diferentes, e colapsar as duas apaga o comando de cauda.
#
# Estas asercoes nasceram de uma regressao REAL introduzida por este proprio
# conserto: `A=$(false) echo hi` deixou de rodar o `echo hi` porque o rc do
# `$(false)` foi lido como aborto. A conformidade nao pegou (o corpus nao tem
# o caso) — so a comparacao direta com o bash pegou.
printf 'A=$(false) echo hi\necho rc=$?\n' > "$SB/t.sh"
igual "A=\$(false) cmd — o comando roda mesmo com o cmdsub falhando"
printf 'A=$(true) echo hi\necho rc=$?\n' > "$SB/t.sh"
igual "A=\$(true) cmd — o comando roda e o rc e do comando"
printf 'A=$(false)\necho rc=$?\n' > "$SB/t.sh"
igual "A=\$(false) sozinha — o rc VEM do cmdsub"
printf 'A[0]=$(false) echo hi\necho rc=$?\n' > "$SB/t.sh"
igual "A[0]=\$(false) cmd — idem no caminho de array"
printf 'A=1 B=2 echo hi\necho rc=$?\necho A=$A B=$B\n' > "$SB/t.sh"
igual "A=1 B=2 cmd — prefixo inline e restaurado depois"
printf 'readonly R=1\nR=2\necho rc=$?\necho R=$R\n' > "$SB/t.sh"
igual "atribuir a readonly aborta com rc 1 e NAO muda o valor"

echo "── F. TAMANHOS EXTREMOS (100 KB) — cresce ou recusa ALTO, nunca calado ─"

{ printf 'f() {\n  echo INICIO\n'; corpo 2000; printf '  echo FIM\n}\nf\n'; } > "$SB/t.sh"
igual_ou_alto "corpo de funcao de ~100 KB (multilinha)"

printf 'f() { echo INICIO; : %s; echo FIM; }\nf\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "corpo de funcao de 100 KB numa LINHA so"

printf 'V=%s\necho ${#V}\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "V=<literal de 100 KB>"

printf 'V="%s"\necho ${#V}\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "V=\"<literal de 100 KB>\""

printf 'A[0]=%s\necho ${#A[0]}\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "A[0]=<literal de 100 KB>"

printf 'export V=%s\necho ${#V}\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "export V=<literal de 100 KB>"

printf 'readonly V=%s\necho ${#V}\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "readonly V=<literal de 100 KB>"

printf 'g() { local V=%s; echo ${#V}; }\ng\n' "$(rep 100000)" > "$SB/t.sh"
igual_ou_alto "local V=<literal de 100 KB>"

rep 100000 > "$SB/big.txt"
printf 'V=$(cat big.txt)\necho ${#V}\n' > "$SB/t.sh"
igual_ou_alto "V=\$(cat arquivo de 100 KB)"

# A tabela de funcoes tem 32 lugares e o bash nao tem teto nenhum. O retorno
# de `jash_function_define` era DESCARTADO: a 33a definicao sumia sem uma
# palavra e a chamada caia em "command not found" — ou, pior, num externo de
# mesmo nome. O teto continua existindo (crescer a tabela muda um struct
# publico e e onda propria), mas AGORA ELE FALA. Esta asercao e o que impede
# o silencio de voltar.
{ i=1; while [ "$i" -le 40 ]; do printf 'f%d() { echo F%d; }\n' "$i" "$i"; i=$((i+1)); done
  printf 'f1\necho FIM\n'; } > "$SB/t.sh"
igual_ou_alto "40 funcoes (tabela de 32): o teto que resta nao e MUDO"

echo "── G. A REGRA GERAL: se divergiu, TEM que ter dito algo ──────────────"

# A asercao transversal do invariante, escrita como asercao e nao como
# comentario. Para cada tamanho da varredura, ou a saida bate com o bash, ou
# o stderr do jash nao esta vazio. Um "nao" aqui e exatamente a morte
# silenciosa que este arquivo existe para impedir.
mudo=0; mudo_qual=""
for n in 1000 1023 1024 1100 2048 2049 4095 4096 8191 8192 16384; do
    printf 'V=%s\necho ${#V}\n' "$(rep $n)" > "$SB/t.sh"
    if [ "$(jout)" != "$(bout)" ] && [ -z "$(jerr)" ]; then
        mudo=$((mudo+1)); mudo_qual="$mudo_qual V=$n"
    fi
    { printf 'f() {\n  echo INICIO\n  : %s\n  echo FIM\n}\nf\n' "$(rep $n)"; } > "$SB/t.sh"
    if [ "$(jout)" != "$(bout)" ] && [ -z "$(jerr)" ]; then
        mudo=$((mudo+1)); mudo_qual="$mudo_qual func=$n"
    fi
done
[ "$mudo" = "0" ] \
    && ok "22 tamanhos varridos: nenhuma divergencia MUDA" \
    || bad "morte silenciosa" "$mudo caso(s) divergiram do bash com stderr VAZIO:$mudo_qual"

echo ""
echo "=== JINV-086: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
