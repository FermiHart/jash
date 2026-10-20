#!/usr/bin/env bash
# JINV-066 — o `!` de `[ … ]` tem que INVERTER. E isto NAO e cosmetica.
#
# ═══ O QUE ESTAVA ERRADO ═══
#
#   [ ! -f /etc/hosts ]      bash: rc 1 (falso)    jash: rc 0   ← ERRADO
#   [ ! -d /etc ]            bash: rc 1            jash: rc 0   ← ERRADO
#   [ ! "a" = "a" ]          bash: rc 1            jash: rc 0   ← ERRADO
#   [ ! -f /nao/existe ]     bash: rc 0            jash: rc 0   ← certo por acidente
#   ! [ -f /etc/hosts ]      bash: rc 1            jash: rc 127 ← segundo defeito
#   [[ ! -f /etc/hosts ]]    bash: rc 1            jash: rc 1   ← este funcionava
#
# O `test`/`[` reconhecia a expressao lendo o RESTO DA LINHA como texto:
# procurava um prefixo (`-f `, `-d `, `-z `…) e, se nada casasse, caia num
# apanha-tudo "string nao vazia = verdadeiro". O `!` nunca foi um caso desse
# reconhecedor — entao ele nao era ignorado: virava OPERANDO. A expressao
# inteira passava a perguntar "esta linha tem algum caractere?", que e sempre
# verdade. Por isso o unico caso que "funcionava" era o que ja deveria dar
# verdadeiro por outro motivo.
#
# ═══ POR QUE ISTO E DESTRUICAO DE DADO ═══
#
#     [ ! -f "$backup" ] && rm -rf "$dir"
#
# No bash isso so apaga se o backup SUMIU. No jash apagava SEMPRE — rc 0, sem
# mensagem, sem sinal de que algo deu errado. Toda clausula de guarda escrita
# no idioma mais comum do Unix (`if [ ! -f "$cfg" ]`, `if [ ! -d "$dest" ]`)
# falhava ABERTA, e sempre na direcao de EXECUTAR a acao destrutiva. Uma
# divergencia que erra para o lado seguro custa um retrabalho; esta custava o
# dado. E o `[[ ! … ]]` funcionava, o que tornava o defeito invisivel para
# quem testasse com colchete duplo.
#
# A assercao GUARDA-DEFENSIVA no fim deste arquivo e a que carrega o motivo:
# ela prova que `[ ! -f <arquivo-que-existe> ] && echo PERIGO` NAO imprime
# PERIGO. Se ela ficar vermelha, o shell voltou a apagar o que nao devia.
#
# ═══ COMO ESTE GATE JULGA ═══
#
# O /bin/bash e o ORACULO em toda assercao: o rc (e a saida) do jash sao
# comparados com os do bash na MESMA linha, para a MESMA entrada. Nenhum rc
# esperado esta escrito a mao — um numero digitado aqui seria mais uma
# afirmacao a conferir, e a semantica do `test` do POSIX e definida por
# CONTAGEM de argumentos, com regras que ninguem acerta de memoria:
#
#   - com 3 argumentos, `-a`/`-o` no meio nao sao os conectivos do analisador,
#     e sim um E/OU entre dois testes de UM argumento (`[ -f -a /nope ]` e
#     VERDADEIRO no bash: duas strings nao vazias);
#   - essa regra vem ANTES do `!` (`[ ! -a /etc ]` e verdadeiro);
#   - `[ ! ! x ]` e erro de sintaxe (rc 2), mas `[ ! -f x ]` nao e.
#
# Gate: bash tests/test_jinv066_test_negation.sh
# Conserto: c/builtins/builtin_test.c (porta argv), c/parser/eval.c (`! cmd`),
#           c/main.c (via rapida de -c nao pode tratar `!` como programa).

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "SKIP: $DL ausente"; exit 0; }
[ -x /bin/bash ] || { echo "SKIP: oraculo /bin/bash ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# Fixture: caminhos ABSOLUTOS, para que os dois shells vejam o mesmo mundo sem
# depender de qual diretorio cada um considera corrente.
: > "$T/vazio"
printf 'conteudo\n' > "$T/cheio"
mkdir -p "$T/dir"
ln -s "$T/cheio" "$T/link"
printf '#!/bin/sh\n' > "$T/exec"; chmod 755 "$T/exec"
AUSENTE="$T/nao-existe"

# ── O ORACULO ──────────────────────────────────────────────────────────────
# Roda a MESMA linha nos dois shells e exige rc E stdout iguais. O stderr fica
# de fora de proposito: o texto do diagnostico nunca foi prometido igual ao do
# bash, e fundi-lo na captura faria o veredito depender de mensagem de UX.
# Todo lado do jash passa pelo cao de guarda — inclusive os que negam laco
# (`until ! …`), que sao justamente os que travam quando a negacao quebra.
o() {
    local code="$1"
    local bo jo br jr
    bo=$(/bin/bash -c "$code" 2>/dev/null); br=$?
    jo=$("$DL" 15 "$JASH" -c "$code" 2>/dev/null); jr=$?
    if [ "$jr" = "124" ]; then
        bad "$code" "jash TRAVOU (prazo de 15s estourou); bash devolveu rc=$br"
    elif [ "$br" = "$jr" ] && [ "$bo" = "$jo" ]; then
        ok "bash=jash rc=$br  ::  $code"
    else
        bad "$code" "bash rc=$br out=[$bo]   |   jash rc=$jr out=[$jo]"
    fi
}

echo "── contagem de argumentos (0,1,2,3,4,n) ──────────────────────────────"
o '[ ]'
o 'test'
o '[ "" ]'
o '[ x ]'
o '[ ! ]'
o '[ - ]'
o '[ -n ]'
o '[ -z ]'
o '[ -a ]'
o '[ ! x ]'
o '[ ! "" ]'
o '[ -f -f ]'
o '[ a b ]'
o '[ -q x ]'
o '[ x y z ]'
o '[ ! = ! ]'
o '[ 1 -eq 1 ]'
o '[ ! -f '"$AUSENTE"' ]'
o '[ ! ! -f '"$T/cheio"' ]'
o '[ ! -f '"$T/cheio"' extra ]'
o '[ x -a y -a z ]'
o '[ a b c d e ]'
o '[ '"$T/cheio"' x y ]'

echo "── o defeito: ! com operadores UNARIOS ───────────────────────────────"
o '[ ! -f '"$T/cheio"' ]'
o '[ ! -f '"$AUSENTE"' ]'
o '[ ! -d '"$T/dir"' ]'
o '[ ! -d '"$T/cheio"' ]'
o '[ ! -e '"$T/cheio"' ]'
o '[ ! -e '"$AUSENTE"' ]'
o '[ ! -r '"$T/cheio"' ]'
o '[ ! -r '"$AUSENTE"' ]'
o '[ ! -w '"$T/cheio"' ]'
o '[ ! -x '"$T/exec"' ]'
o '[ ! -x '"$T/cheio"' ]'
o '[ ! -s '"$T/cheio"' ]'
o '[ ! -s '"$T/vazio"' ]'
o '[ ! -z "" ]'
o '[ ! -z x ]'
o '[ ! -n "" ]'
o '[ ! -n x ]'
o '[ ! -L '"$T/link"' ]'
o 'test ! -f '"$T/cheio"''
o 'test ! -d '"$T/dir"''

echo "── o defeito: ! com operadores BINARIOS ──────────────────────────────"
o '[ ! a = a ]'
o '[ ! a = b ]'
o '[ ! "a b" = "a b" ]'
o '[ ! "a b" = "a c" ]'
o '[ ! a != b ]'
o '[ ! a != a ]'
o '[ ! 1 -eq 1 ]'
o '[ ! 1 -eq 2 ]'
o '[ ! 1 -ne 2 ]'
o '[ ! 1 -ne 1 ]'
o '[ ! 1 -lt 2 ]'
o '[ ! 2 -lt 1 ]'
o '[ ! 2 -gt 1 ]'
o '[ ! 1 -gt 2 ]'
o '[ ! 1 -le 1 ]'
o '[ ! 2 -le 1 ]'
o '[ ! 1 -ge 1 ]'
o '[ ! 1 -ge 2 ]'

echo "── ! repetido (a paridade tem que contar) ────────────────────────────"
o '[ ! ! ]'
o '[ ! ! -f '"$T/cheio"' ]'
o '[ ! ! ! -f '"$T/cheio"' ]'
o '[ ! ! ! ! -f '"$T/cheio"' ]'
o '[ ! ! 1 -eq 1 ]'
o '[ ! ! x ]'
o '[ ! ! ! ]'

echo "── ! junto de -a / -o ────────────────────────────────────────────────"
o '[ ! 1 -eq 1 -a 2 -eq 2 ]'
o '[ 1 -eq 1 -a ! 1 -eq 2 ]'
o '[ ! x = x -a y = y ]'
o '[ ! x = x -o y = y ]'
o '[ x = x -a ! y = y ]'
o '[ ! -f '"$T/cheio"' -o ! -d '"$T/dir"' ]'
o '[ ! -f '"$AUSENTE"' -a -f '"$T/cheio"' ]'
o '[ ! -z "" -a -n x ]'
o '[ ! -n x -o ! -z "" ]'
# as tres regras de 3 argumentos que ninguem escreveria de memoria
o '[ -f -a /nope ]'
o '[ "" -a "" ]'
o '[ ! -a /etc ]'
o '[ -a -a -a ]'
o '[ -o -o -o ]'

echo "── parenteses de agrupamento ─────────────────────────────────────────"
o '[ \( x \) ]'
o '[ \( "" \) ]'
o '[ ! \( 1 -eq 1 \) ]'
o '[ \( ! 1 -eq 1 \) ]'
o '[ \( ! -f '"$T/cheio"' \) ]'
o '[ \( -f '"$T/cheio"' \) -a \( -d '"$T/dir"' \) ]'
o '[ \( \( 1 -eq 1 \) \) ]'
o '[ \( x \) y ]'

echo "── ! como PREFIXO de comando/pipeline (o segundo defeito) ────────────"
o '! true'
o '! false'
o '! true; echo rc=$?'
o '! false; echo rc=$?'
o '! [ -f '"$T/cheio"' ]'
o '! [ -f '"$AUSENTE"' ]'
o '! echo saida-preservada'
o '! echo x | /usr/bin/wc -l'
o '! true && echo AND || echo OR'
o '! false && echo AND || echo OR'
o 'if ! true; then echo T; else echo F; fi'
o 'if ! [ -f '"$T/cheio"' ]; then echo A; else echo B; fi'
o 'while ! true; do echo LACO; break; done'
# `until ! false` gira para sempre se a negacao devolver 127 em vez de 0 —
# por isso o cao de guarda esta em TODA chamada, nao so nesta.
o 'until ! false; do echo U; break; done'

echo "── quebras que NAO podem ser silenciosas (erro de sintaxe = rc 2) ────"
o '[ 1 -eq x ]'
o '[ x -eq 1 ]'
o '[ -f '"$T/cheio"''
o 'test -f '"$T/cheio"' ]'
o '[ x -a y z ]'

echo "── palavra citada continua UMA palavra ───────────────────────────────"
o 'v="a b"; [ "$v" = "a b" ]'
o 'v="a b"; [ ! "$v" = "a b" ]'
o 'v="a b"; [ ! "$v" = "a c" ]'
o 'v=""; [ ! -z "$v" ]'
o 'v="x"; [ ! -n "$v" ]'
o '[ ! " " = "" ]'

# ── A ASSERCAO QUE CARREGA O MOTIVO ───────────────────────────────────────
# Nao basta o rc bater com o do bash: o que estava em jogo era um comando
# DESTRUTIVO disparando. Aqui a guarda protege um arquivo que EXISTE, entao
# a acao nao pode rodar — e o julgamento e sobre a SAIDA, nao sobre o rc.
echo "── guarda defensiva: a acao destrutiva NAO pode disparar ─────────────"
guarda=$("$DL" 15 "$JASH" -c '[ ! -f '"$T/cheio"' ] && echo PERIGO' 2>/dev/null)
if [ -z "$guarda" ]; then
    ok "[ ! -f <existente> ] && echo PERIGO  ::  nao disparou (saida vazia)"
else
    bad "[ ! -f <existente> ] && echo PERIGO" \
        "DISPAROU: imprimiu [$guarda] — a guarda falhou ABERTA, no sentido destrutivo"
fi

guarda_dir=$("$DL" 15 "$JASH" -c '[ ! -d '"$T/dir"' ] && echo PERIGO' 2>/dev/null)
if [ -z "$guarda_dir" ]; then
    ok "[ ! -d <existente> ] && echo PERIGO  ::  nao disparou (saida vazia)"
else
    bad "[ ! -d <existente> ] && echo PERIGO" \
        "DISPAROU: imprimiu [$guarda_dir] — mkdir/rm de guarda voltaria a rodar sempre"
fi

guarda_if=$("$DL" 15 "$JASH" -c 'if [ ! -f '"$T/cheio"' ]; then echo PERIGO; fi' 2>/dev/null)
if [ -z "$guarda_if" ]; then
    ok "if [ ! -f <existente> ]  ::  ramo destrutivo nao foi tomado"
else
    bad "if [ ! -f <existente> ]" \
        "DISPAROU: imprimiu [$guarda_if] — o idioma mais comum de guarda do Unix"
fi

# E o outro lado: quando o arquivo REALMENTE falta, a guarda TEM que disparar.
# Sem isto, um `test` que sempre devolvesse falso passaria nas tres acima.
guarda_neg=$("$DL" 15 "$JASH" -c '[ ! -f '"$AUSENTE"' ] && echo ESPERADO' 2>/dev/null)
if [ "$guarda_neg" = "ESPERADO" ]; then
    ok "[ ! -f <ausente> ] && echo ESPERADO  ::  disparou, como deve"
else
    bad "[ ! -f <ausente> ] && echo ESPERADO" \
        "NAO disparou (saida [$guarda_neg]) — negacao invertida para o outro lado"
fi

echo
echo "=== JINV-066: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ] || exit 1
exit 0
