#!/usr/bin/env bash
# JINV-080 — o corpo de um `for` nao pode ser truncado em SILENCIO.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   O corpo de um laco `for` nao tem teto arbitrario de tamanho. Se algum dia
#   faltar memoria, a recusa e ALTA (stderr + rc != 0 + o laco NAO roda).
#   O que nao pode existir e o meio-termo: rodar um programa DIFERENTE do que
#   estava escrito, com rc 0, sem uma palavra.
#
# ═══ O DEFEITO ═══
#
#     char body[1024];
#     if (body_len >= sizeof(body)) body_len = sizeof(body) - 1;   /* clamp mudo */
#
#   Um corpo com mais de 1023 bytes tinha a CAUDA APAGADA e o laco rodava
#   assim mesmo. Reproducao minima (corpo de ~15 comandos):
#
#     $ jash ./b.sh          # nada
#     $ bash ./b.sh          # MARCA-FIM=a
#     $ echo $?              # 0 nos dois
#
#   O `echo` final do corpo simplesmente deixava de existir. rc 0, stderr
#   vazio. Quem chamou nao tinha COMO saber — nem o shell sabia.
#
#   1024 bytes de corpo sao ~15 linhas de comando reais. Todo laco de verdade
#   passa disso; o `for ac_option / do … done` de qualquer `configure` de
#   autoconf tem ~7 KB.
#
# ═══ COMO FOI ACHADO ═══
#
#   Medindo os 19 `configure` DEPOIS de consertar JINV-077 (`export NOME`) e
#   JINV-078 (`for VAR;`). Enquanto o `export` abortava o arquivo, este bug
#   era inalcancavel. Um bloqueador dominante esconde o proximo — e o proximo
#   era o pior dos dois, porque nao dizia nada.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real.
#
# ORACULO: /bin/bash em toda asercao.
# Gate: bash tests/test_jinv080_loop_body_size.sh

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

# Gera um laco cujo CORPO tem $1 comandos de ~68 bytes cada.
# $2 = "in" (for x in a) ou "pos" (for x, JINV-078)
gera() {
    local n="$1" forma="$2" i=0
    {
        if [ "$forma" = pos ]; then
            printf 'set -- a\nfor x\ndo\n'
        else
            printf 'for x in a\ndo\n'
        fi
        while [ "$i" -lt "$n" ]; do
            printf '  v%s=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa%s\n' "$i" "$i"
            i=$((i + 1))
        done
        printf '  echo "MARCA-FIM=$x"\n'
        printf 'done\n'
        printf 'echo cauda-ok\n'
    } > "$SB/b.sh"
}

# Rc por FUNCAO, nunca `var=$( … >/dev/null 2>&1 )` inline: a captura inline
# com os dois canais na mesma expressao e o achado MERGED-EXACT do JINV-061
# (um aviso de UX no stderr passaria a decidir o veredito). Aqui os dois canais
# morrem no /dev/null e so o `$?` atravessa.
jfout() { (cd "$SB" && "$DL" 20 "$JASH"   ./b.sh 2>/dev/null); }
bfout() { (cd "$SB" && "$DL" 20 /bin/bash ./b.sh 2>/dev/null); }
jferr() { (cd "$SB" && "$DL" 20 "$JASH"   ./b.sh 2>&1 >/dev/null); }
jfrc()  { (cd "$SB" && "$DL" 20 "$JASH"   ./b.sh >/dev/null 2>&1); printf '%s' "$?"; }
bfrc()  { (cd "$SB" && "$DL" 20 /bin/bash ./b.sh >/dev/null 2>&1); printf '%s' "$?"; }
jcout() { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" 2>/dev/null); }
bcout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jcrc()  { (cd "$SB" && "$DL" 20 "$JASH"   -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
bcrc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

# Roda o MESMO arquivo nos dois shells e exige saida + rc iguais.
comparado() {
    local desc="$1" jo bo jr br
    jo=$(jfout); jr=$(jfrc)
    bo=$(bfout); br=$(bfrc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo]"
    fi
}

echo "── (1) os dois lados do corte de 1024 bytes ─────────────────────────"
# 14 comandos ficam ABAIXO de 1024 bytes de corpo (passava antes tambem);
# 15 e o primeiro que ultrapassa — era exatamente ali que a cauda sumia.
for n in 10 14 15 16 20; do
    gera "$n" in
    sz=$(wc -c < "$SB/b.sh" | tr -d ' ')
    comparado "corpo com $n comandos (arquivo de $sz B) — for..in"
done

echo
echo "── (2) o mesmo para a forma sem 'in' (JINV-078) ─────────────────────"
for n in 15 25 40; do
    gera "$n" pos
    sz=$(wc -c < "$SB/b.sh" | tr -d ' ')
    comparado "corpo com $n comandos (arquivo de $sz B) — for x; do"
done

echo
echo "── (3) A propriedade central: NADA se perde em silencio ─────────────"
# Se a cauda do corpo for cortada, o shell tem que DIZER. O que nao pode
# acontecer e o par (rc 0, stderr vazio, saida diferente do bash) — que era
# exatamente o estado anterior.
gera 40 in
jo=$(jfout); je=$(jferr); jr=$(jfrc); bo=$(bfout)
if [ "$jo" = "$bo" ]; then
    ok "corpo grande produz a saida do bash (nem truncado, nem mudo)"
elif [ -n "$je" ] && [ "$jr" != "0" ]; then
    ok "corpo grande recusado ALTO (rc=$jr + stderr) — nao e morte silenciosa"
else
    bad "morte silenciosa" "rc=$jr, stderr vazio, saida jash=[$jo] != bash=[$bo]"
fi

echo
echo "── (4) o ultimo comando do corpo e o que mais some — ele existe? ────"
gera 40 in
n=$(jfout | grep -c 'MARCA-FIM=a')
b=$(bfout | grep -c 'MARCA-FIM=a')
if [ "$n" = "$b" ] && [ "$n" -gt 0 ]; then
    ok "o comando final do corpo executou ($n vez(es), igual ao bash)"
else
    bad "cauda do corpo apagada" "jash=$n ocorrencias, bash=$b"
fi

echo
echo "── (5) aninhado: o corpo de fora cresce, o de dentro tambem ─────────"
{
    printf 'for i in 1 2\ndo\n'
    k=0; while [ $k -lt 12 ]; do
        printf '  o%s=oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  for j in x y\n  do\n'
    k=0; while [ $k -lt 12 ]; do
        printf '    n%s=nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn%s\n' "$k" "$k"; k=$((k+1)); done
    printf '    echo "par=$i$j"\n  done\n'
    printf '  echo "fora=$i"\ndone\necho cauda-ok\n'
} > "$SB/b.sh"
comparado "laco aninhado, os dois corpos acima de 1 KB"

echo
echo "── (6) cauda depois do done, com corpo grande (o free vem antes) ────"
{
    printf 'for i in 1\ndo\n'
    k=0; while [ $k -lt 25 ]; do
        printf '  g%s=gggggggggggggggggggggggggggggggggggggggggggggggggggggggg%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  echo "corpo=$i"\ndone; for j in 2; do echo "cauda=$j"; done; echo fim\n'
} > "$SB/b.sh"
comparado "corpo grande + outro laco na cauda do done"

echo
echo "── (7) break/continue continuam funcionando com corpo grande ────────"
{
    printf 'for i in 1 2 3\ndo\n'
    k=0; while [ $k -lt 20 ]; do
        printf '  q%s=qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  if [ "$i" = 2 ]; then continue; fi\n'
    printf '  echo "i=$i"\ndone\necho fim\n'
} > "$SB/b.sh"
comparado "continue dentro de corpo grande"
{
    printf 'for i in 1 2 3\ndo\n'
    k=0; while [ $k -lt 20 ]; do
        printf '  q%s=qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  if [ "$i" = 2 ]; then break; fi\n'
    printf '  echo "i=$i"\ndone\necho fim\n'
} > "$SB/b.sh"
comparado "break dentro de corpo grande"

echo
echo "── (8) o caminho curto (corpo pequeno) nao regrediu ─────────────────"
oracle_small() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jcout "$code"); jr=$(jcrc "$code")
    bo=$(bcout "$code"); br=$(bcrc "$code")
    if [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then ok "$desc"
    else bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"; fi
}
oracle_small "for..in curto"     'for i in 1 2 3; do echo "[$i]"; done'
oracle_small "for..in vazio"     'for i in; do echo n; done; echo depois'
oracle_small "for nu curto"      'set -- a b; for i; do echo "[$i]"; done'
oracle_small "rc do corpo vaza"  'for i in 1; do false; done; echo rc=$?'
oracle_small "cauda do done"     'for i in 1; do :; done && echo cauda'

printf '\n=== JINV-080: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
