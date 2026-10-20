#!/usr/bin/env bash
# JINV-082 — o corpo de `while`/`until` e o de um ramo de `case` nao podem
#            ser truncados em SILENCIO.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   O corpo de um laco `while`/`until` e o corpo de um ramo de `case` nao tem
#   teto arbitrario de tamanho — nem a CONDICAO do laco tem. Se algum dia
#   faltar memoria, a recusa e ALTA (stderr + rc != 0 + o corpo NAO roda).
#   O que nao pode existir e o meio-termo: rodar um programa DIFERENTE do que
#   estava escrito, com rc 0 e stderr vazio.
#
# ═══ O DEFEITO ═══
#
#   Os dois gemeos exatos do JINV-080 (`for`), consertado no mesmo dia, nos
#   arquivos ao lado, e deixados vivos:
#
#     c/parser/while_loop.c:110   char body[1024];   // + char cond[512]
#     c/parser/case_stmt.c:452    char cmd[1024];
#
#     if (body_len >= sizeof(body)) body_len = sizeof(body) - 1;   /* mudo */
#
#   Sintoma PIOR que o do `for`: o `for` ao menos morria alto as vezes (o
#   acumulador de main.c reclamava de `done` faltando). Aqui nao. Medido:
#
#     while  2051 B de arquivo  → perde `echo MARCA`  │ rc 0, stderr VAZIO
#     case   2044 B de arquivo  → perde `echo MARCA`  │ rc 0, stderr VAZIO
#
#   1024 bytes de corpo sao ~15 linhas de comando reais. Todo laco de verdade
#   passa disso — e `while`/`case` sao justamente as formas que um `configure`
#   de autoconf usa para varrer argumentos.
#
# ═══ O CONSERTO ═══
#
#   Identico ao JINV-080: pilha ate 1023 bytes (caso comum, zero malloc),
#   heap do tamanho exato acima, porta de saida unica (`while_body_done`) e
#   OOM = recusa alta. No `case` o buffer e POR RAMO e e liberado no fim de
#   cada volta, entao varrer 50 ramos nao acumula nada.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real, e um gate que a leia passa
# verde contra qualquer mutante.
#
# ORACULO: /bin/bash em toda asercao.
# Gate: bash tests/test_jinv082_loop_body_heap.sh

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

# O cao de guarda tem que estar VIVO (timeout(1) nao existe nesta maquina).
"$DL" 2 /bin/sleep 5 >/dev/null 2>&1
[ $? = 124 ] || { echo "FAIL: deadline.sh nao protege (rc != 124)"; exit 1; }

jfout() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>/dev/null); }
bfout() { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh 2>/dev/null); }
jferr() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>&1 >/dev/null); }
jfrc()  { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
bfrc()  { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

comparado() {
    local desc="$1" jo bo jr br
    jo=$(jfout); jr=$(jfrc)
    bo=$(bfout); br=$(bfrc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 60s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[${jo:0:120}] | bash rc=$br out=[${bo:0:120}]"
    fi
}

# Gera um laco/ramo cujo CORPO tem $1 linhas de ~68 bytes.
# $2 = while | until | case | while-arith
gera() {
    local n="$1" k="$2" i=0
    {
        case "$k" in
          while)       printf 'n=0\nwhile [ $n -lt 1 ]\ndo\n  n=1\n' ;;
          until)       printf 'n=0\nuntil [ $n -gt 0 ]\ndo\n  n=1\n' ;;
          case)        printf 'v=a\ncase $v in\n  a)\n' ;;
          while-arith) printf 'n=0\nwhile [ $n -lt 3 ]\ndo\n  n=$((n+1))\n' ;;
        esac
        while [ "$i" -lt "$n" ]; do
            printf '  r%s=rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr%s\n' "$i" "$i"
            i=$((i + 1))
        done
        printf '  echo MARCA\n'
        case "$k" in
          while|until|while-arith) printf 'done\n' ;;
          case)                    printf '  ;;\nesac\n' ;;
        esac
        printf 'echo cauda\n'
    } > "$SB/t.sh"
}

echo "── (1) os dois lados do corte de 1024 bytes de CORPO ────────────────"
# 14 linhas ficam ABAIXO de 1024 bytes de corpo (passava antes tambem);
# 15 e a primeira que ultrapassa — era ali que a cauda sumia, calada.
for k in while until case while-arith; do
    for n in 10 14 15 16 20 40; do
        gera "$n" "$k"
        sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
        comparado "$k com $n linhas de corpo (arquivo de $sz B)"
    done
done

echo
echo "── (2) A CONDICAO do while acima de 512 bytes ───────────────────────"
# `char cond[512]` tinha o mesmo clamp mudo: a condicao saia cortada e o laco
# testava outra coisa. A condicao abaixo tem ~700 B de comparacoes reais.
{
    printf 'x=alvo\n'
    printf 'n=0\n'
    printf 'while [ "$x" = alvo ]'
    k=0; while [ $k -lt 22 ]; do
        printf ' && [ "$x" != naoehisso%s ]' "$k"; k=$((k+1)); done
    printf ' && [ $n -lt 1 ]\ndo\n  n=1\n  echo DENTRO\ndone\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "condicao de while com ~700 B (arquivo de $sz B)"
{
    printf 'x=alvo\nn=0\n'
    printf 'while [ "$x" = alvo ]'
    k=0; while [ $k -lt 22 ]; do
        printf ' && [ "$x" != naoehisso%s ]' "$k"; k=$((k+1)); done
    printf ' && [ "$x" = NAO_BATE ]\ndo\n  n=1\n  echo NUNCA\ndone\necho "n=$n"\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "condicao longa cuja CAUDA e falsa — o laco nao pode rodar ($sz B)"

echo
echo "── (3) MUITO acima do teto antigo: ~100 KB de corpo ─────────────────"
for k in while until case; do
    gera 1500 "$k"
    sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
    comparado "$k com corpo de $sz B (~100x o teto antigo)"
done

echo
echo "── (4) o ultimo comando do corpo e o que mais some — ele existe? ────"
for k in while until case; do
    gera 40 "$k"
    n=$(jfout | grep -c '^MARCA$')
    b=$(bfout | grep -c '^MARCA$')
    if [ "$n" = "$b" ] && [ "$n" -gt 0 ]; then
        ok "$k: o comando final do corpo executou ($n vez(es), igual ao bash)"
    else
        bad "$k: cauda do corpo apagada" "jash=$n ocorrencias, bash=$b"
    fi
done

echo
echo "── (5) aninhado: corpo de fora grande, corpo de dentro grande ───────"
{
    printf 'i=0\nwhile [ $i -lt 2 ]\ndo\n  i=$((i+1))\n'
    k=0; while [ $k -lt 25 ]; do
        printf '  o%s=oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  j=0\n  until [ $j -ge 2 ]\n  do\n    j=$((j+1))\n'
    k=0; while [ $k -lt 25 ]; do
        printf '    n%s=nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn%s\n' "$k" "$k"; k=$((k+1)); done
    printf '    echo "par=$i$j"\n  done\n'
    printf '  echo "fora=$i"\ndone\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "while > until, os dois corpos acima de 1 KB ($sz B)"

# Varios ramos grandes em sequencia: o buffer e POR RAMO, entao varrer os
# nao-casados nao pode nem acumular nem sujar o ramo que casa. O ramo que casa
# e o ULTIMO — se a varredura dos anteriores errasse, ele nao rodaria.
{
    printf 'v=d\ncase $v in\n'
    for ramo in a b c d; do
        printf '  %s)\n' "$ramo"
        k=0; while [ $k -lt 25 ]; do
            printf '    x%s%s=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx%s\n' "$ramo" "$k" "$k"; k=$((k+1)); done
        printf '    echo RAMO-%s\n  ;;\n' "$ramo"
    done
    printf 'esac\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "case com QUATRO ramos acima de 1 KB — so o ultimo casa ($sz B)"

# Um laco inteiro dentro de um ramo de case (o corpo do ramo reentra no
# avaliador enquanto o heap do ramo ainda esta vivo).
{
    printf 'v=b\ncase $v in\n  a) echo NUNCA ;;\n  b)\n'
    k=0; while [ $k -lt 20 ]; do
        printf '    y%s=yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy%s\n' "$k" "$k"; k=$((k+1)); done
    printf '    i=0\n    while [ $i -lt 2 ]\n    do\n      i=$((i+1))\n'
    k=0; while [ $k -lt 20 ]; do
        printf '      z%s=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz%s\n' "$k" "$k"; k=$((k+1)); done
    printf '      echo "i=$i"\n    done\n'
    printf '    echo RAMO-B\n  ;;\nesac\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "while grande DENTRO de um ramo de case grande ($sz B)"

echo
echo "── (6) break / continue com corpo grande ────────────────────────────"
{
    printf 'i=0\nwhile [ $i -lt 4 ]\ndo\n  i=$((i+1))\n'
    k=0; while [ $k -lt 20 ]; do
        printf '  q%s=qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  if [ "$i" = 2 ]; then continue; fi\n'
    printf '  echo "i=$i"\ndone\necho fim\n'
} > "$SB/t.sh"
comparado "continue dentro de corpo de while grande"
{
    printf 'i=0\nwhile [ $i -lt 4 ]\ndo\n  i=$((i+1))\n'
    k=0; while [ $k -lt 20 ]; do
        printf '  q%s=qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  if [ "$i" = 3 ]; then break; fi\n'
    printf '  echo "i=$i"\ndone\necho fim\n'
} > "$SB/t.sh"
comparado "break dentro de corpo de while grande"

echo
echo "── (7) cauda depois do done, com corpo grande (o free vem ANTES) ────"
{
    printf 'i=0\nwhile [ $i -lt 1 ]\ndo\n  i=1\n'
    k=0; while [ $k -lt 25 ]; do
        printf '  g%s=gggggggggggggggggggggggggggggggggggggggggggggggggggggggg%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  echo "corpo=$i"\ndone; j=0; while [ $j -lt 1 ]; do j=1; echo "cauda=$j"; done; echo fim\n'
} > "$SB/t.sh"
comparado "corpo grande + outro while na cauda do done"

echo
echo '── (8) redirect / cano no done: o enunciado e REMONTADO em pipeline.c'
# `while …; do …; done < arq` (e `| cmd`, e `> arq`) nao passa por eval.c:
# eval.c delega a pipeline.c, que remontava o enunciado num `char
# full_line[1024]`. Penhasco medido: 1015 B ok, 1080 B "missing done".
printf 'l1\nl2\nl3\n' > "$SB/dados.txt"
corpo_grande() {   # $1 = prefixo da variavel, $2 = quantas linhas
    local k=0
    while [ "$k" -lt "$2" ]; do
        printf '  %s%s=dddddddddddddddddddddddddddddddddddddddddddddddddddddddd%s\n' "$1" "$k" "$k"
        k=$((k+1))
    done
}
for n in 14 15 16 20 40; do
    { printf 'while read L\ndo\n'; corpo_grande d "$n"
      printf '  echo "linha=$L"\ndone < dados.txt\necho fim\n'; } > "$SB/t.sh"
    sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
    comparado "while read + 'done < arquivo', $n linhas de corpo ($sz B)"
done
{ printf 'i=0\nwhile [ $i -lt 2 ]\ndo\n  i=$((i+1))\n'; corpo_grande e 25
  printf '  echo "saida=$i"\ndone > saida.txt\ncat saida.txt\necho fim\n'; } > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "while com corpo grande e 'done > arquivo' ($sz B)"
{ printf 'for x in a b c\ndo\n'; corpo_grande f 25
  printf '  echo "x=$x"\ndone | /usr/bin/wc -l\necho fim\n'; } > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "for com corpo grande e 'done | wc -l' ($sz B)"
{ printf 'v=b\ncase $v in\n  b)\n'; corpo_grande g 25
  printf '    echo RAMO\n  ;;\nesac > saida2.txt\ncat saida2.txt\necho fim\n'; } > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "case com ramo grande e 'esac > arquivo' ($sz B)"

echo
echo "── (9) A propriedade central: NADA se perde em silencio ─────────────"
for k in while until case; do
    gera 1500 "$k"
    jo=$(jfout); je=$(jferr); jr=$(jfrc); bo=$(bfout)
    if [ "$jo" = "$bo" ]; then
        ok "$k de 100 KB produz a saida do bash (nem truncado, nem mudo)"
    elif [ -n "$je" ] && [ "$jr" != "0" ]; then
        ok "$k de 100 KB recusado ALTO (rc=$jr + stderr) — nao e morte silenciosa"
    else
        bad "$k: morte silenciosa" "rc=$jr, stderr vazio, jash=[${jo:0:80}] != bash=[${bo:0:80}]"
    fi
done

echo
echo "── (10) o caminho curto (corpo < 1 KB) nao regrediu ─────────────────"
# Rc por FUNCAO (JINV-061 MERGED-EXACT: uma captura inline com os dois canais
# na mesma expressao deixa um aviso de UX no stderr decidir o veredito).
jcout() { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" 2>/dev/null); }
bcout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jcrc()  { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
bcrc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
oracle_c() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jcout "$code"); jr=$(jcrc "$code")
    bo=$(bcout "$code"); br=$(bcrc "$code")
    if [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then ok "$desc"
    else bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"; fi
}
oracle_c "while curto"          'n=0; while [ $n -lt 3 ]; do n=$((n+1)); echo "[$n]"; done'
oracle_c "until curto"          'n=0; until [ $n -ge 3 ]; do n=$((n+1)); echo "[$n]"; done'
oracle_c "while falso de cara"  'while false; do echo n; done; echo depois'
oracle_c "case curto"           'v=b; case $v in a) echo A ;; b) echo B ;; *) echo C ;; esac'
oracle_c "case sem match"       'v=z; case $v in a) echo A ;; esac; echo rc=$?'
oracle_c "case com ;; em aspas" 'case x in x) echo "a;;b" ;; esac'
oracle_c "rc do corpo vaza"     'n=0; while [ $n -lt 1 ]; do n=1; false; done; echo rc=$?'
oracle_c "cauda do done"        'n=0; while [ $n -lt 1 ]; do n=1; done && echo cauda'

printf '\n=== JINV-082: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
