#!/usr/bin/env bash
# JINV-088 — o TEXTO DO COMANDO dentro de `$( )` nao e cortado.
#
# ═══ Por que este era o pior teto que restava ═══
#
# Os tetos das ondas anteriores cortavam DADO. Este cortava o ENUNCIADO DO
# COMANDO A EXECUTAR. A diferenca nao e de grau:
#
#     echo $(printf %s <500 a's> | wc -c)
#
# O `| wc -c` estava DEPOIS do byte 511, entao ele simplesmente nao existia.
# O jash rodava `printf %s aaa…`, imprimia 500 letras, e devolvia rc 0. Nao
# houve perda de dado — houve TROCA DE COMANDO. O shell executou um programa
# que ninguem escreveu, calado e com cara de sucesso.
#
# Isso e a familia JINV-016/020/022/026/032 (dado virando estrutura, estrutura
# deixando de ser estrutura), e o final de uma linha e exatamente onde moram o
# `|`, o `> arquivo` e o `&&`. Cortar o fim e apagar o significado.
#
# ═══ Os TRES tetos, em serie ═══
#
# A armadilha medida no JINV-086 (nove tetos, uns escondendo os outros)
# repetiu-se aqui em tres. Consertar um nao movia o penhasco um byte:
#
#   1. `inner`   — ANALISE de `$( … )`     `c/parser/cmdsub.c`  (clamp)
#   2. `cleaned` — REMOCAO DE COMENTARIOS  `c/parser/cmdsub.c`  (guarda ci+2)
#   3. `inner`   — ANALISE de ` … `        `c/parser/cmdsub.c`  (guarda ilen+1)
#
# Medidos ANTES do conserto, com `V=$(printf %s <n a's>); echo ${#V}`:
# 500 ok · 505 devolvia 500 (tetos 1+2) · a crase devolvia 501 (teto 3).
#
# ═══ O que este gate protege, e o que NAO ═══
#
# Protege: nenhum tamanho de enunciado dentro de `$( )` / ` … ` muda o comando
# executado. Acima de ~16 KB por comando o jash tem um teto DECLARADO de outro
# estagio (`JEVAL_SEG_CAP`, eval.c) e recusa ALTO, com mensagem no stderr.
# Divergir do bash ali e legitimo; divergir EM SILENCIO nao e. E essa a regra
# que a secao G escreve como assercao e nao como comentario.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO. Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash exporta
# JASH_BIN apontando para o binario real, e um gate que a leia passa verde
# contra qualquer mutante. Ja mordeu tres agentes.
# Os tres mutantes estao registrados em SRC_MUTANTS (tools/mutation-check.sh),
# nao so neste comentario.
#
# ORACULO: /bin/bash em TODA assercao. Nenhum numero deste arquivo foi
# escolhido a mao — todos vem do bash rodando o MESMO arquivo.
# A conformidade NAO serve de prova aqui: a linha mais longa de
# `tests/conformance/corpus.txt` tem 105 bytes e o menor teto desta onda era
# 511. Ela nao pode, por construcao, ver esta familia.
#
# CAO DE GUARDA: tools/deadline.sh, nunca `timeout` — timeout(1) NAO EXISTE
# nesta maquina e devolve 127 sob bash, o que faz o teste concluir "nao
# travou" quando travou. `command -v timeout` mente (no jash e builtin).
#
# Gate: bash tests/test_jinv088_cmdsub_body.sh

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

# `rep N` — N bytes de 'a'. Via awk porque a linha de comando nao aguenta
# 100 KB de argumento (`Argument list too long` mata o gate sem explicar, e
# faz os dois lados falharem IGUAL, produzindo um `ok` que nao vale nada).
rep() { awk -v n="$1" 'BEGIN{s="";while(length(s)<n)s=s "a";printf "%s", substr(s,1,n)}'; }

jout() { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 90 /bin/bash ./t.sh 2>/dev/null); }
jerr() { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh 2>&1 >/dev/null); }
jrc()  { (cd "$SB" && "$DL" 90 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 90 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

# A assercao central: identico ao bash, saida E rc.
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

# Tamanhos EXTREMOS: acima do teto DECLARADO de outro estagio, divergir e
# legitimo — desde que o jash DIGA. "Ou cresce, ou recusa alto."
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

echo "── A. OS DOIS LADOS DO PENHASCO DE 512 — corpo de \$( ) ───────────────"

# `printf %s ` custa 10 bytes, entao o texto INTERNO tem n+10 bytes: a
# varredura abaixo atravessa 502 (onde o texto cruza 512), 500 (o penhasco
# real do `cleaned`, medido) e 501 (o da crase). Nenhum numero e chute.
for n in 100 495 499 500 501 502 505 510 511 512 513 520 1000 5000; do
    printf 'V=$(printf %%s %s)\necho ${#V}\n' "$(rep "$n")" > "$SB/t.sh"
    igual "corpo de \$( ) com $n bytes de argumento (texto interno $((n+10)) B)"
done

echo "── B. A CAUDA ESTRUTURAL — o \`|\`, o \`>\` e o \`&&\` do fim ──────────────"
#
# Esta e a secao que descreve a DOENCA. Nas de cima o sintoma e um numero
# menor; aqui o sintoma e OUTRO COMANDO. Se a cauda cair, `wc -c` nunca roda
# e o jash imprime as proprias letras — rc 0, stderr vazio.

for n in 480 500 505 1000 5000; do
    printf 'echo $(printf %%s %s | wc -c)\n' "$(rep "$n")" > "$SB/t.sh"
    igual "o \`| wc -c\` no fim de um \$( ) de $((n+10)) B ainda existe"
done

for n in 500 5000; do
    printf 'echo $(printf %%s %s > saida.txt; wc -c < saida.txt)\n' "$(rep "$n")" > "$SB/t.sh"
    igual "o \`> arquivo\` no fim de um \$( ) de $((n+20)) B ainda existe"
    rm -f "$SB/saida.txt"
done

for n in 500 5000; do
    printf 'echo $(printf %%s %s >/dev/null && echo CAUDA_VIVA)\n' "$(rep "$n")" > "$SB/t.sh"
    igual "o \`&& echo\` no fim de um \$( ) de $((n+30)) B ainda existe"
done

for n in 500 5000; do
    printf 'echo $(printf %%s %s >/dev/null; echo DEPOIS_DO_PONTO)\n' "$(rep "$n")" > "$SB/t.sh"
    igual "o \`; echo\` no fim de um \$( ) de $((n+30)) B ainda existe"
done

printf 'echo $(printf %%s %s >/dev/null || echo NAO_DEVIA)\n' "$(rep 5000)" > "$SB/t.sh"
igual "o \`|| echo\` no fim de um \$( ) grande ainda existe"

printf 'echo $(printf %%s %s | cat | wc -c)\n' "$(rep 5000)" > "$SB/t.sh"
igual "cano DUPLO no fim de um \$( ) grande"

echo "── C. CRASE — o mesmo teto, o mesmo estrago ──────────────────────────"

for n in 495 500 501 502 505 512 1000 5000; do
    printf 'V=`printf %%s %s`\necho ${#V}\n' "$(rep "$n")" > "$SB/t.sh"
    igual "corpo de \` \` com $n bytes de argumento"
done

for n in 500 5000; do
    printf 'echo `printf %%s %s | wc -c`\n' "$(rep "$n")" > "$SB/t.sh"
    igual "a cauda \`| wc -c\` dentro de crase de $((n+10)) B"
done

printf 'echo `printf %%s %s >/dev/null && echo CAUDA_VIVA`\n' "$(rep 5000)" > "$SB/t.sh"
igual "a cauda \`&& echo\` dentro de crase grande"

echo "── D. ANINHADO — o corte de fora estraga a estrutura de dentro ───────"
#
# Aqui o sintoma antigo nao era um numero menor: era saida VAZIA. O corte caia
# no meio do `$(` interno e deixava um parenteses sem par.

for n in 480 505 1000 5000; do
    printf 'echo $(echo $(printf %%s %s | wc -c))\n' "$(rep "$n")" > "$SB/t.sh"
    igual "\$( ) aninhado 2 niveis, $n bytes"
done

printf 'echo $(echo $(echo $(printf %%s %s | wc -c)))\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) aninhado 3 niveis, 5000 bytes"

printf 'echo $(echo `printf %%s %s | wc -c`)\n' "$(rep 5000)" > "$SB/t.sh"
igual "crase aninhada dentro de \$( ), 5000 bytes"

printf 'echo "$(echo "$(printf %%s %s | wc -c)")"\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) aninhado COM aspas nos dois niveis, 5000 bytes"

echo "── E. ASPAS DENTRO — o corte nao pode virar aspa nao terminada ───────"

for n in 500 5000; do
    printf 'echo $(printf %%s "%s" | wc -c)\n' "$(rep "$n")" > "$SB/t.sh"
    igual "aspas DUPLAS dentro de \$( ) de $n bytes"
done

for n in 500 5000; do
    printf "echo \$(printf %%s '%s' | wc -c)\n" "$(rep "$n")" > "$SB/t.sh"
    igual "aspas SIMPLES dentro de \$( ) de $n bytes"
done

printf 'echo $(printf %%s "a)b%s" | wc -c)\n' "$(rep 5000)" > "$SB/t.sh"
igual "um \`)\` CITADO dentro de \$( ) grande continua sendo dado (BUG-075)"

printf 'echo $(printf %%s %s | wc -c  # comentario que termina na nova-linha\n)\n' "$(rep 5000)" > "$SB/t.sh"
igual "comentario dentro de \$( ) grande (o segundo teto, o do \`cleaned\`)"

echo "── F. CONTEXTOS — atribuicao, argumento, condicao, corpo de laco ─────"

printf 'V=$(printf %%s %s | wc -c)\necho $V\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande em ATRIBUICAO"

printf 'printf "[%%s]\\n" "$(printf %%s %s | wc -c)"\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande em ARGUMENTO citado"

printf 'V=$(printf %%s %s | wc -c)\nif [ "$V" -eq 5000 ]; then echo SIM; else echo NAO; fi\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande alimentando uma CONDICAO"

# O SUJEITO de um `case` cabe num `char expr[256]` (c/parser/case_stmt.c:322),
# um teto PRE-EXISTENTE proprio — medido e registrado no fim deste arquivo.
# Acima de ~270 B de sujeito o `case` cai no ramo ERRADO, calado. A assercao
# aqui usa um sujeito ABAIXO dele: o que se prova e que o caminho
# `case "$( )"` nao regrediu. O sujeito GRANDE (via variavel) vem logo abaixo.
printf 'case "$(printf %%s %s | wc -c | tr -d " ")" in\n  200) echo CASOU ;;\n  *) echo NAO ;;\nesac\n' "$(rep 200)" > "$SB/t.sh"
igual "\$( ) dentro de uma condicao de case (sujeito abaixo do teto do case)"

printf 'V=$(printf %%s %s)\ncase "$V" in\n  a*) echo CASOU ;;\n  *) echo NAO ;;\nesac\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) GRANDE alimentando um case por variavel (sujeito de 5000 B)"

# O `[` do jash tem um penhasco PRE-EXISTENTE proprio (~292 B de linha, medido
# e registrado no fim deste arquivo), entao aqui a assercao usa um corpo
# ABAIXO dele: o que se prova e que o caminho `[ "$( )" ]` nao regrediu.
printf 'if [ "$(printf %%s %s | wc -c)" -eq 200 ]; then echo SIM; else echo NAO; fi\n' "$(rep 200)" > "$SB/t.sh"
igual "\$( ) direto dentro de \`[ … ]\` (corpo abaixo do penhasco do \`[\`)"

printf 'while [ -z "${D:-}" ]; do D=$(printf %%s %s | wc -c); done\necho $D\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande no CORPO de um while"

printf 'for i in 1 2; do echo $(printf %%s %s | wc -c); done\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande no CORPO de um for (duas voltas, sem acumular)"

printf 'f() {\n  echo $(printf %%s %s | wc -c)\n}\nf\nf\n' "$(rep 5000)" > "$SB/t.sh"
igual "\$( ) grande dentro de FUNCAO, chamada duas vezes"

printf 'echo $(cat <<EOF | wc -c\n%s\nEOF\n)\n' "$(rep 5000)" > "$SB/t.sh"
igual "HEREDOC dentro de \$( ) grande"

printf 'echo $(cat <<EOF | wc -c\n%s\nEOF\n) $(printf %%s %s | wc -c)\n' "$(rep 3000)" "$(rep 4000)" > "$SB/t.sh"
igual "heredoc dentro de \$( ) e um SEGUNDO \$( ) grande na mesma linha"

printf 'echo $(printf %%s %s | wc -c) $(printf %%s %s | wc -c)\n' "$(rep 5000)" "$(rep 3000)" > "$SB/t.sh"
igual "DOIS \$( ) grandes na mesma linha (o de dentro nao come o de fora)"

echo "── G. O FLUXO DE SAIDA — a regressao que a conformidade nao pega ─────"
#
# Unificar os `return` de um ramo numa porta so e o que torna o conserto
# seguro contra vazamento de heap — e e tambem a chance de ENGOLIR um
# comando. Colapsar "a expansao ABORTOU" e "o comando terminou != 0" num
# codigo so foi exatamente o que quebrou `A=$(false) echo hi` no JINV-086.
# A conformidade ficou VERDE durante aquela regressao (o corpus nao tem o
# caso). Estas assercoes existem por isso, e sao sobre o FLUXO, nao sobre o
# dado que a porta estava la para proteger.

printf 'A=$(false) echo hi\n' > "$SB/t.sh"
igual "A=\$(false) echo hi — o echo RODA (prefixo de atribuicao, nao abortou)"

printf 'A=$(false)\necho $?\n' > "$SB/t.sh"
igual "A=\$(false) sozinho — o \$? e o do comando de dentro"

printf 'A=$(true) echo hi\n' > "$SB/t.sh"
igual "A=\$(true) echo hi"

printf 'A=$(exit 7) echo hi\n' > "$SB/t.sh"
igual "A=\$(exit 7) echo hi — o exit de dentro nao mata a linha"

printf 'A=$(printf %%s %s >/dev/null; false) echo hi\n' "$(rep 5000)" > "$SB/t.sh"
igual "A=\$(… grande …; false) echo hi — o mesmo, com corpo acima do teto antigo"

# DOIS prefixos, o primeiro com corpo grande. A assercao e sobre o FLUXO — o
# `echo` tem que RODAR — e nao sobre o valor: o jash tem uma divergencia
# PRE-EXISTENTE (e alheia ao cmdsub: acontece com `A=um B=dois echo $A $B`)
# em que o prefixo de atribuicao vaza para a expansao da propria linha.
# Registrada no fim deste arquivo; medi-la aqui mediria aquele bug, nao este.
printf 'A=$(printf %%s %s | wc -c) B=$(echo dois) echo depois\n' "$(rep 5000)" > "$SB/t.sh"
igual "DOIS prefixos de atribuicao, o primeiro com \$( ) grande — o comando RODA"

printf 'V=$(exit 3)\necho $?\n' > "$SB/t.sh"
igual "V=\$(exit 3) — o rc atravessa a porta unica"

printf 'echo A$(printf %%s %s | wc -c)B\n' "$(rep 5000)" > "$SB/t.sh"
igual "prefixo e sufixo LITERAIS em volta de um \$( ) grande sobrevivem"

echo "── H. CRASE SEM FECHO e outras entradas malformadas ──────────────────"
#
# A porta unica trocou um `break` no meio do ramo por um salto — se a intencao
# original (parar de montar a linha) tivesse se perdido, uma crase sem fecho
# passaria a rodar como comando.

printf 'echo `printf %%s abc\n' > "$SB/t.sh"
igual_ou_alto "crase sem fecho nao vira comando"

printf 'echo $(printf %%s abc\n' > "$SB/t.sh"
igual_ou_alto "\$( sem fecho nao vira comando"

printf 'echo `printf %%s %s\n' "$(rep 5000)" > "$SB/t.sh"
igual_ou_alto "crase sem fecho com corpo GRANDE (o caminho do heap)"

printf 'echo $(printf %%s %s\n' "$(rep 5000)" > "$SB/t.sh"
igual_ou_alto "\$( sem fecho com corpo GRANDE (o caminho do heap)"

printf 'echo $()\n' > "$SB/t.sh"
igual "\$( ) vazio"

printf 'echo ``\n' > "$SB/t.sh"
igual "crase vazia"

echo "── I. TAMANHOS EXTREMOS — ou cresce, ou recusa ALTO ──────────────────"
#
# Acima de ~16 KB por comando vale o teto DECLARADO de outro estagio
# (JEVAL_SEG_CAP, c/parser/eval.c). Divergir do bash ali e legitimo. Divergir
# CALADO nao e — e essa a unica coisa que estas assercoes exigem.

for n in 8000 16000 20000 32000 100000; do
    printf 'V=$(printf %%s %s)\necho ${#V}\n' "$(rep "$n")" > "$SB/t.sh"
    igual_ou_alto "corpo de \$( ) com $n bytes"
done

for n in 20000 100000; do
    printf 'echo $(printf %%s %s | wc -c)\n' "$(rep "$n")" > "$SB/t.sh"
    igual_ou_alto "cauda \`| wc -c\` num \$( ) de $n bytes"
done

echo "── J. A REGRA GERAL: se divergiu, TEM que ter dito algo ──────────────"
#
# A assercao transversal do invariante, escrita como assercao e nao como
# comentario. Para cada tamanho, ou a saida bate com o bash, ou o stderr do
# jash nao esta vazio. Um "nao" aqui e exatamente a morte silenciosa que este
# arquivo existe para impedir.

mudo=0; mudo_qual=""
for n in 500 502 505 510 511 512 513 600 1000 2048 4096 8192 16000; do
    printf 'V=$(printf %%s %s)\necho ${#V}\n' "$(rep "$n")" > "$SB/t.sh"
    if [ "$(jout)" != "$(bout)" ] && [ -z "$(jerr)" ]; then
        mudo=$((mudo+1)); mudo_qual="$mudo_qual paren=$n"
    fi
    printf 'V=`printf %%s %s`\necho ${#V}\n' "$(rep "$n")" > "$SB/t.sh"
    if [ "$(jout)" != "$(bout)" ] && [ -z "$(jerr)" ]; then
        mudo=$((mudo+1)); mudo_qual="$mudo_qual crase=$n"
    fi
    printf 'echo $(printf %%s %s | wc -c)\n' "$(rep "$n")" > "$SB/t.sh"
    if [ "$(jout)" != "$(bout)" ] && [ -z "$(jerr)" ]; then
        mudo=$((mudo+1)); mudo_qual="$mudo_qual cauda=$n"
    fi
done
[ "$mudo" = "0" ] \
    && ok "39 tamanhos varridos: nenhuma divergencia MUDA" \
    || bad "morte silenciosa" "$mudo caso(s) divergiram do bash com stderr VAZIO:$mudo_qual"

echo "── K. O TETO NOMEADO DERIVA DE core/limits.h ─────────────────────────"
#
# O JINV-081 registrou o erro de categoria: um teto declarado como numero
# solto volta a divergir dos vizinhos na onda seguinte. O unico numero novo
# desta onda e um LIMIAR DE PILHA e esta travado contra `core/limits.h` em
# tempo de compilacao. Este par de assercoes existe para que apagar o
# `#error` seja VERMELHO, e nao um detalhe que ninguem revisa.

if grep -q '#define JASH_CMDSUB_INNER_STACK' c/parser/cmdsub.c; then
    ok "o limiar de pilha do cmdsub tem NOME (JASH_CMDSUB_INNER_STACK)"
else
    bad "teto sem nome" "JASH_CMDSUB_INNER_STACK sumiu de c/parser/cmdsub.c"
fi

if grep -q 'JASH_CMDSUB_INNER_STACK > JASH_LINE_CAP_STACK' c/parser/cmdsub.c &&
   grep -q '#error' c/parser/cmdsub.c; then
    ok "o limiar e verificado contra core/limits.h em tempo de compilacao"
else
    bad "derivacao perdida" "o #if/#error contra JASH_LINE_CAP_STACK sumiu"
fi

if grep -q 'core/limits.h' c/parser/cmdsub.c; then
    ok "cmdsub.c continua incluindo core/limits.h (JINV-030)"
else
    bad "derivacao perdida" "cmdsub.c nao inclui mais core/limits.h"
fi

echo ""
echo "=== JINV-088: $pass ok / $fail fail ==="
#
# ── TETOS VIZINHOS MEDIDOS NESTA ONDA E AINDA ABERTOS ────────────────────
# Registrados aqui para nao sumirem (mesma disciplina do fim do JINV-081).
# Nenhum e o texto do comando; cada um precisa do seu conserto e do seu gate.
#
#   0. `c/parser/case_stmt.c:322` `char expr[256]` — o SUJEITO de um `case`.
#      Acima de ~270 bytes de sujeito o `case` escolhe o ramo ERRADO, com rc 0
#      e stderr VAZIO. Penhasco medido entre 264 B e 284 B de linha de `case`.
#      E o pior dos quatro: nao ha numero menor para notar, ha OUTRO RAMO
#      executando. MEDIDO PRE-EXISTENTE (identico no binario mutante desta
#      onda), portanto nao e regressao daqui — mas e a mesma familia e o
#      proximo alvo obvio. Um sujeito grande vindo de VARIAVEL passa ileso
#      (assercao na secao F), o que confirma que o teto e do `expr`, nao do
#      casamento de padrao.
#
#   1. `[ "$( )" … ]` — a linha inteira acima de ~292 bytes faz o `[` receber
#      um argumento cortado: `jash: [: missing ']'` ate ~230 B e
#      `[: unterminated quote` acima disso, e a condicao cai no ramo ERRADO.
#      MEDIDO PRE-EXISTENTE (identico no binario mutante), portanto nao e
#      regressao desta onda. Penhasco entre 292 B e 297 B de linha total.
#
#   1b. Prefixo de atribuicao vaza para a expansao da PROPRIA linha:
#      `A=um B=dois echo $A $B` imprime `um dois` no jash e VAZIO no bash
#      (POSIX: o prefixo so entra no ambiente do comando, nao na expansao das
#      palavras dele). Alheio ao cmdsub — acontece com literais.
#
#   2. `JEVAL_SEG_CAP` (16383 B, c/parser/eval.c) — um comando acima disso e
#      recusado com mensagem no stderr (correto, JINV-021), mas o rc da linha
#      continua 0: a recusa nao chega ao codigo de saida.
#
#   3. `dqsave[64]` / `ndq[64]` (c/parser/cmdsub.c) — teto de PROFUNDIDADE de
#      aninhamento de `$( )` dentro de aspas, nao de tamanho. Acima de 64
#      niveis o estado de citacao deixa de ser restaurado corretamente.
#
#   4. `char path[512]` / `char unq[512]` no ramo `$(<arquivo)` — o NOME DO
#      ARQUIVO e cortado, o que abre outro arquivo. Nao foi possivel medir o
#      penhasco neste macOS (o proprio FS recusa componentes acima de 255 B),
#      mas o corte existe no codigo para caminhos longos com varios
#      componentes, e depois da expansao de `$( <"$f" )`.
[ "$fail" -eq 0 ]
