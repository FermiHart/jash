#!/usr/bin/env bash
# JINV-085 — o padrão do `case` é uma PALAVRA, e o `esac` do filho não é do pai.
#
# ═══ As quatro propriedades que este gate protege ═══════════════════════
#
# 1. **O branco em volta do `|` não pertence ao padrão.** POSIX XCU 2.9.4: a
#    lista de padrões é uma sequência de PALAVRAS separadas por `|`; o branco
#    entre elas é separador de token. O casador antigo recebia as alternativas
#    com o branco colado (`"NONE "`, `" ''"`), e uma palavra que termina em
#    espaço não casa com um texto que não termina em espaço — então **toda
#    cláusula com espaço em volta do `|` era morta em silêncio**.
#
# 2. **O `|` só separa quando é ESTRUTURA.** Citado ou escapado ele é DADO.
#    É a família JINV-016 / JINV-020 / JINV-028 / JINV-032 — um caractere que
#    era dado virando sintaxe — na última varredura de separador do repositório
#    que ainda estava nua (`while (*p && *p != '|') p++;`).
#
# 3. **A expressão de colchete é a do POSIX inteira**: `]` como primeiro membro
#    é literal (`[]]`, `[!]]`), e os nomes de classe `[:digit:]`, `[:alpha:]`,
#    `[:space:]` … valem. Faltavam as duas coisas.
#
# 4. **Um `case` aninhado tem os `;;` dele.** O leitor de fim-de-ramo já sabia
#    que um `;;` citado ou dentro de `$( )` não fecha o ramo (JINV-032);
#    faltava o terceiro esconderijo. Sem contar o aninhamento, o PRIMEIRO `;;`
#    do filho fechava o ramo do PAI, o corpo saía cortado no meio, e o resto
#    (`esac;; esac`) era relido como continuação do pai — rc 1, stderr vazio,
#    nada impresso. Morte silenciosa.
#
#    E o aninhamento é reconhecido pela régua do shell — **palavra reservada em
#    POSIÇÃO DE COMANDO** — não por ocorrência da palavra. Senão `echo esac`
#    fecharia o filho, e um argumento viraria estrutura: a propriedade 2 ao
#    contrário. As asserções `reservada-*` cobram exatamente isso.
#
# ═══ Por que isto não é teoria ══════════════════════════════════════════
#
# Estas quatro se encontram numa construção só, e ela está em TODO `configure`
# de autoconf que existe — a checagem de caminho absoluto do prefixo:
#
#     case $ac_val in
#       [\\/$]* | ?:[\\/]* )  continue;;
#       NONE | '' ) case $ac_var in *prefix ) continue;; esac;;
#     esac
#     as_fn_error $? "expected an absolute path for --$ac_var: $ac_val"
#
# Espaço em volta do `|` (1), alternativa citada `''` (2), colchete com escape
# (3), `case` aninhado no corpo do ramo (4). MEDIDO: das 38 invocações de
# `configure --help` / `--version` desta máquina, **12 morriam exatamente aí**,
# com `configure: error: expected an absolute path for --exec_prefix: NONE`.
#
# ═══ O que este gate NÃO acusa ══════════════════════════════════════════
#
# `;&` (fall-through) NÃO existe no /bin/bash 3.2 desta máquina — ele responde
# `syntax error near unexpected token '&'`. Sem oráculo não há asserção: a
# bateria confere isso em tempo de execução e se declara fora de alcance em vez
# de inventar um veredito. Não refile como defeito do jash.
#
# ═══ O oráculo e o cão de guarda ═══════════════════════════════════════
#
# TODA asserção compara contra `/bin/bash` rodando o MESMO arquivo no MESMO
# diretório. Nenhuma afirma nada por conta própria.
#
# Os dois shells rodam com `LC_ALL=C LANG=C`, e a razão é uma divergência real
# que este gate NÃO conserta e não esconde: sob o locale desta máquina
# (`pt_BR.UTF-8`) o /bin/bash usa a ordem de COLAÇÃO do locale para faixas de
# colchete, onde maiúsculas e minúsculas se intercalam (aAbB…zZ) — então
#
#     case M in [a-z]) echo SIM;; *) echo NAO;; esac
#       bash (pt_BR) -> SIM        bash (C) -> NAO        jash -> NAO
#
# O jash compara em ordem ASCII, sempre. Isso é **pré-existente e alheio ao
# JINV-085** (o binário anterior responde igual ao novo, verificado), e mexer
# nele mudaria a semântica de TODA faixa de colchete do shell — outro conserto,
# com outra medida. Comparar dois shells sob um locale que só um deles
# implementa mediria o locale, não a propriedade sob teste; por isso a variável
# é fixada em vez de sorteada. Quem for atacar a colação começa por aqui.
#
# O cão de guarda é `tools/deadline.sh`, NUNCA `timeout`: `timeout(1)` não
# existe nesta máquina e devolve 127 sob bash — um teste que veja rc≠124
# concluiria "não travou" de um travamento real. Pior, `command -v timeout`
# MENTE quando quem pergunta é o jash (lá é builtin). A asserção 0 prova que o
# cão está VIVO antes de qualquer asserção de travamento confiar nele.
#
# O binário sob teste vem de `JASH_UNDER_TEST` — **nunca de `JASH_BIN`**. O
# próprio jash exporta `JASH_BIN` no ambiente dos agentes, então um gate que a
# leia testa o binário instalado e passa verde contra QUALQUER mutante. Já
# mordeu três agentes; a prova negativa deste conserto depende disso.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# ── o juiz ─────────────────────────────────────────────────────────────
# Escreve o programa num ARQUIVO e roda os dois shells sobre ele. Arquivo, e
# não `-c`, porque metade destas propriedades só aparece em `case` de VÁRIAS
# LINHAS (o branco da borda esquerda de um padrão é uma NOVA LINHA), que é
# como todo script real escreve — e como o autoconf escreve.
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br
    printf '%s\n' "$prog" > "$W/p.sh"
    jo=$( (cd "$W" && LC_ALL=C LANG=C "$DEADLINE" 10 "$JASH"   "$W/p.sh" 2>/dev/null) ); jr=$?
    bo=$( (cd "$W" && LC_ALL=C LANG=C "$DEADLINE" 10 "$ORACLE" "$W/p.sh" 2>/dev/null) ); br=$?
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
# Sem isto as asserções de travamento (13) poderiam passar por 127.
"$DEADLINE" 2 sleep 30 >/dev/null 2>&1
if [ $? = 124 ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "watchdog inerte" \
        "deadline.sh 2 sleep 30 não devolveu 124 — as asserções de travamento estão sem valor"
fi

# ── 1. ALTERNAÇÃO: 2, 3 e 8 ramos ──────────────────────────────────────
oracle "alt 2 ramos (casa o 1º)"  'case a in a|b) echo SIM;; *) echo NAO;; esac'
oracle "alt 2 ramos (casa o 2º)"  'case b in a|b) echo SIM;; *) echo NAO;; esac'
oracle "alt 3 ramos (casa o meio)" 'case b in a|b|c) echo SIM;; *) echo NAO;; esac'
oracle "alt 3 ramos (casa o fim)"  'case c in a|b|c) echo SIM;; *) echo NAO;; esac'
oracle "alt 3 ramos (nenhum)"      'case z in a|b|c) echo SIM;; *) echo NAO;; esac'
oracle "alt 8 ramos (casa o 1º)"   'case a in a|b|c|d|e|f|g|h) echo SIM;; *) echo NAO;; esac'
oracle "alt 8 ramos (casa o 8º)"   'case h in a|b|c|d|e|f|g|h) echo SIM;; *) echo NAO;; esac'
oracle "alt 8 ramos (nenhum)"      'case z in a|b|c|d|e|f|g|h) echo SIM;; *) echo NAO;; esac'

# ── 2. ALTERNAÇÃO COM GLOBS ────────────────────────────────────────────
oracle "alt glob a*|b? (casa a*)" 'case axyz in a*|b?) echo SIM;; *) echo NAO;; esac'
oracle "alt glob a*|b? (casa b?)" 'case bq in a*|b?) echo SIM;; *) echo NAO;; esac'
oracle "alt glob a*|b? (nenhum)"  'case bqq in a*|b?) echo SIM;; *) echo NAO;; esac'
oracle "alt glob com colchete"    'case c9 in a*|[b-d][0-9]) echo SIM;; *) echo NAO;; esac'
oracle "alt glob e literal"       'case NONE in [/]*|NONE) echo SIM;; *) echo NAO;; esac'

# ── 3. O BRANCO EM VOLTA DO `|` — a propriedade que paga a conta ───────
# Cada uma destas cláusulas era MORTA: a alternativa saía com o branco colado
# e uma palavra terminada em espaço não casa com texto que não termina nele.
oracle "espaço antes do |"        'case NONE in NONE | x) echo SIM;; *) echo NAO;; esac'
oracle "espaço depois do |"       'case x in NONE | x) echo SIM;; *) echo NAO;; esac'
oracle "espaço dos dois lados"    'case m in a | m | z) echo SIM;; *) echo NAO;; esac'
oracle "espaço antes do )"        'case m in a | m ) echo SIM;; *) echo NAO;; esac'
oracle "tab em volta do |"        "$(printf 'case m in a\t|\tm) echo SIM;; *) echo NAO;; esac')"
oracle "nova linha (padrão multi-linha)" 'case foo in
    "") echo VAZIO;;
    */) echo BARRA;;
    *)  echo OUTRO;;
  esac'
oracle "continuação com barra invertida" 'case m in \
   a | \
   m ) echo SIM;; \
   *) echo NAO;; esac'
oracle "glob com espaço em volta do |"   'case /usr/x in [/]* | ?:[/]* ) echo ABS;; *) echo REL;; esac'

# ── 4. O `|` CITADO É DADO (família JINV-016/020/028/032) ─────────────
oracle "| dentro de aspas simples"  "case 'a|b' in 'a|b') echo SIM;; *) echo NAO;; esac"
oracle "| dentro de aspas duplas"   'case "a|b" in "a|b") echo SIM;; *) echo NAO;; esac'
oracle "| escapado com barra"       'case "a|b" in a\|b) echo SIM;; *) echo NAO;; esac'
oracle "| citado NÃO casa o pedaço" "case a in 'a|b') echo SIM;; *) echo NAO;; esac"
oracle "citado e nu na mesma lista" "case b in 'a|b' | b) echo SIM;; *) echo NAO;; esac"
oracle "espaço CITADO sobrevive"    'case "a b" in "a b") echo SIM;; *) echo NAO;; esac'
oracle "espaço ESCAPADO sobrevive"  'case "a b" in a\ b) echo SIM;; *) echo NAO;; esac'

# ── 5. EXPRESSÃO DE COLCHETE ───────────────────────────────────────────
oracle "[abc] casa"        'case b in [abc]) echo SIM;; *) echo NAO;; esac'
oracle "[abc] não casa"    'case z in [abc]) echo SIM;; *) echo NAO;; esac'
oracle "[a-z] faixa"       'case m in [a-z]) echo SIM;; *) echo NAO;; esac'
oracle "[a-z] fora da faixa" 'case M in [a-z]) echo SIM;; *) echo NAO;; esac'
oracle "[!abc] negado"     'case z in [!abc]) echo SIM;; *) echo NAO;; esac'
oracle "[!abc] negado casa" 'case a in [!abc]) echo SIM;; *) echo NAO;; esac'
oracle "[^abc] negado"     'case z in [^abc]) echo SIM;; *) echo NAO;; esac'
oracle "[]] fecha-colchete literal"  'case "]" in []]) echo SIM;; *) echo NAO;; esac'
oracle "[]] não casa outro"          'case a in []]) echo SIM;; *) echo NAO;; esac'
oracle "[!]] negado com ] literal"   'case a in [!]]) echo SIM;; *) echo NAO;; esac'
oracle "[!]] negado rejeita ]"       'case "]" in [!]]) echo SIM;; *) echo NAO;; esac'
oracle "[/] barra"         'case /x in [/]*) echo SIM;; *) echo NAO;; esac'
oracle "[/] barra não casa" 'case ax in [/]*) echo SIM;; *) echo NAO;; esac'
oracle "[a-] hífen final literal" 'case - in [a-]) echo SIM;; *) echo NAO;; esac'
oracle "[\\\\/\$]* do autoconf"   'case /x in [\\/$]*) echo ABS;; *) echo NAO;; esac'
oracle "[\\\\/\$]* casa \$"       'case \$x in [\\/$]*) echo ABS;; *) echo NAO;; esac'
oracle "[\\\\/\$]* rejeita letra" 'case zx in [\\/$]*) echo ABS;; *) echo NAO;; esac'
oracle "?:[\\\\/]* do autoconf"   'case c:/x in ?:[\\/]*) echo ABS;; *) echo NAO;; esac'

# ── 6. CLASSES POSIX ───────────────────────────────────────────────────
for cls in digit alpha space upper lower alnum punct xdigit blank graph print cntrl; do
    oracle "[[:$cls:]] contra amostra" "for t in 9 a ' ' A , f _ ; do
  case \$t in [[:$cls:]]) printf 'S';; *) printf 'N';; esac
done; echo"
done
oracle "[[:digit:]] no meio de glob" 'case x9y in *[[:digit:]]*) echo S;; *) echo N;; esac'
oracle "[[:digit:]] não casa"        'case xyz in *[[:digit:]]*) echo S;; *) echo N;; esac'
oracle "duas classes seguidas"       'case a1 in [[:alpha:]][[:digit:]]) echo S;; *) echo N;; esac'
oracle "classe negada"               'case a in [![:digit:]]) echo S;; *) echo N;; esac'
oracle "classe negada rejeita"       'case 7 in [![:digit:]]) echo S;; *) echo N;; esac'
oracle "classe + membro no mesmo []" 'case _ in [_[:digit:]]) echo S;; *) echo N;; esac'
oracle "classe + faixa no mesmo []"  'case q in [[:digit:]m-r]) echo S;; *) echo N;; esac'
oracle "nome de classe desconhecido" 'case a in [[:bogus:]]) echo S;; *) echo N;; esac'
oracle "classe em alternação"        'case 5 in [[:alpha:]] | [[:digit:]]) echo S;; *) echo N;; esac'

# ── 7. `case` ANINHADO NO CORPO DO RAMO ────────────────────────────────
oracle "aninhado simples"        'case a in a) case b in b) echo S;; esac;; esac'
oracle "aninhado que não casa"   'case a in a) case b in c) echo X;; *) echo Y;; esac;; esac'
oracle "aninhado + cauda no pai" 'case a in a) case b in b) echo S;; esac; echo T;; esac'
oracle "aninhado + ramo seguinte do pai" 'case z in
  a) case b in b) echo S;; esac;;
  z) echo Z;;
  *) echo D;;
esac'
oracle "dois aninhados no mesmo ramo" 'case a in a) case b in b) echo 1;; esac; case c in c) echo 2;; esac;; esac'
oracle "aninhamento de 3 níveis"  'case a in a) case b in b) case c in c) echo TRES;; esac;; esac;; esac'
oracle "aninhado dentro de for"   'case rel in
  /*) echo ABS;;
  *) for d in a b
do
  case $d in
    "") d=./ ;;
    */) ;;
    *)  d=$d/ ;;
  esac
  echo "$d"
done
   ;;
esac'
oracle "aninhado com continue"    'for v in a b; do case $v in a) case $v in a) echo A; continue;; esac;; *) echo B;; esac; echo cauda; done'
oracle "aninhado dentro de if"    'case a in a) if true; then case b in b) echo S;; esac; fi;; esac'

# ── 8. PALAVRA RESERVADA SÓ EM POSIÇÃO DE COMANDO ─────────────────────
# A guarda do aninhamento não pode transformar um ARGUMENTO em estrutura —
# seria a propriedade 2 invertida.
oracle "reservada: 'echo esac' não fecha o filho" 'case a in a) case b in b) echo esac;; esac;; esac'
oracle "reservada: 'echo case' não abre nada"     'case a in a) echo case;; esac'
oracle "reservada: esac como texto no pai"        'case a in a) echo "esac esac";; esac'
oracle "reservada: for i in case não abre"        'case a in a) for i in case; do echo "$i"; done;; esac'
oracle "reservada: variável chamada casex"        'case a in a) casex=1; echo "$casex";; esac'
oracle "reservada: palavra esacx não fecha"       'case a in a) echo esacx;; esac'

# ── 9. `;;` CITADO CONTINUA DADO (não-regressão do JINV-032) ──────────
oracle ";; dentro de aspas duplas"  'case x in x) echo "a;;b";; esac'
oracle ";; dentro de aspas simples" "case x in x) echo 'a;;b';; esac"
oracle ";; dentro de \$( )"         'case x in x) echo "$(echo "p;;q")";; esac'
oracle ";; citado com aninhado"     'case a in a) case b in b) echo "i;;j";; esac;; esac'

# ── 10. PADRÃO VAZIO e DEFAULT ────────────────────────────────────────
oracle 'padrão "" casa vazio'    'v=; case "$v" in "") echo VAZIO;; *) echo NAO;; esac'
oracle "padrão '' casa vazio"    "v=; case \"\$v\" in '') echo VAZIO;; *) echo NAO;; esac"
oracle "padrão '' na alternação" "v=; case \"\$v\" in NONE | '') echo OK;; *) echo NAO;; esac"
oracle 'padrão "" não casa texto' 'case abc in "") echo VAZIO;; *) echo NAO;; esac'
oracle "*) default pega tudo"     'case qualquer in a) echo A;; *) echo D;; esac'
oracle "*) default depois de alternação" 'case z in a|b|c) echo ABC;; *) echo D;; esac'

# ── 11. FALL-THROUGH `;&` — só se o oráculo souber ────────────────────
if "$ORACLE" -c 'case a in a) : ;& *) : ;; esac' >/dev/null 2>&1; then
    oracle "fall-through ;&" 'case a in a) echo 1;& *) echo 2;; esac'
else
    ok "';&' não existe no /bin/bash 3.2 desta máquina — sem oráculo, sem asserção"
fi

# ── 12. A CONSTRUÇÃO DO AUTOCONF, INTEIRA ─────────────────────────────
# É a medida que decide: 12 das 38 invocações de `configure --help`/`--version`
# morriam exatamente aqui, com "expected an absolute path for --exec_prefix".
oracle "bloco --exec_prefix do autoconf" 'for ac_var in exec_prefix prefix bindir libdir; do
  case $ac_var in
    exec_prefix) ac_val=NONE ;;
    prefix)      ac_val=NONE ;;
    bindir)      ac_val='"'"'${exec_prefix}/bin'"'"' ;;
    libdir)      ac_val= ;;
  esac
  case $ac_val in
    [\\/$]* | ?:[\\/]* )  echo "$ac_var: ABS"; continue;;
    NONE | '"'"''"'"' ) case $ac_var in *prefix ) echo "$ac_var: PREFIX-OK"; continue;; esac;;
  esac
  echo "ERRO: expected an absolute path for --$ac_var: $ac_val"
done'
oracle "despacho de opção do autoconf" 'for ac_option in --exec-prefix=/x --prefix=/y --outra; do
  case $ac_option in
    -exec-prefix=* | --exec_prefix=* | --exec-prefix=*) echo EP;;
    -prefix=* | --prefix=*) echo P;;
    *) echo OTHER;;
  esac
done'
oracle "case \$0 aninhado do autoconf" 'case rel in #((
  *[\\/]* ) echo abs ;;
  *) for as_dir in p q
do
  case $as_dir in #(((
    '"'"''"'"') as_dir=./ ;;
    */) ;;
    *) as_dir=$as_dir/ ;;
  esac
  echo "$as_dir"
done
     ;;
esac'

# ── 13. NÃO-REGRESSÃO DO JINV-065 ─────────────────────────────────────
# O casador desta porta colapsa a CORRIDA de `*` (`*****` É `*`) e tem um
# ORÇAMENTO de passos que RECUSA em voz alta em vez de pendurar. Estender o
# casador (colchetes, classes, alternação) não pode ter comido nenhum dos dois:
# sem o colapso, a corrida volta a ser exponencial; sem o orçamento, a próxima
# forma patológica pendura o shell. Estas asserções são o alarme.
T41='xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
(cd "$W" && "$DEADLINE" 10 "$JASH" -c "case '$T41' in **********Q9) :;; esac" >/dev/null 2>&1)
[ $? != 124 ] && ok "JINV-065: corrida de 10 asteriscos não trava (colapso intacto)" \
              || bad "JINV-065 REGREDIU: colapso da corrida de '*'" \
                     "case '$T41' in **********Q9) não terminou em 10 s"

(cd "$W" && "$DEADLINE" 10 "$JASH" -c "case '$T41' in ****************************Q9) :;; esac" >/dev/null 2>&1)
[ $? != 124 ] && ok "JINV-065: corrida de 28 asteriscos não trava" \
              || bad "JINV-065 REGREDIU: corrida longa" \
                     "case com 28 asteriscos não terminou em 10 s"

deep=""; i=0
while [ $i -lt 20 ]; do deep="${deep}diretorio$i/"; i=$((i+1)); done
deep="${deep}arquivo.txt"
(cd "$W" && "$DEADLINE" 15 "$JASH" -c "case '$deep' in **/**/**/*.c) :;; esac" >/dev/null 2>&1)
[ $? != 124 ] && ok "JINV-065: globstar contra caminho de ${#deep} chars não trava" \
              || bad "JINV-065 REGREDIU: globstar" \
                     "case '\$p' in **/**/**/*.c) não terminou em 15 s"

# o orçamento continua RECUSANDO ALTO (nunca um "não casou" mudo)
A60='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
PEXP='*a*a*a*a*a*a*a*a*a*a*a*ab'
err=$( (cd "$W" && "$DEADLINE" 20 "$JASH" -c "case '$A60' in $PEXP) printf M;; esac" 2>&1 >/dev/null) )
if printf '%s' "$err" | grep -qi "recusado por custo"; then
    ok "JINV-065: o orçamento ainda GRITA no stderr ao estourar"
else
    bad "JINV-065 REGREDIU: recusa silenciosa" \
        "o padrão exponencial não produziu diagnóstico — o shell responderia rápido e ERRADO. stderr='$err'"
fi

# e o teto não pode passar a recusar trabalho HONESTO por causa das extensões
noise=""
for praw in '*' '**********' '**/**/**/*.c' '*[a-c]*' '*[[:digit:]]*' 'a | b' '[]]'; do
    e=$( (cd "$W" && "$DEADLINE" 15 "$JASH" -c "case '$deep' in $praw) :;; esac" 2>&1 >/dev/null) )
    printf '%s' "$e" | grep -qi "recusado por custo" && { noise="$praw"; break; }
done
[ -z "$noise" ] && ok "JINV-065: padrão honesto nunca é recusado (nem os novos)" \
                || bad "teto baixo demais" "o padrão '$noise' foi recusado — é trabalho legítimo"

echo ""
echo "=== JINV-085: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
