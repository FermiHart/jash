#!/usr/bin/env bash
# JINV-011 / JINV-012 — nem printf nem ls podem perder dado em silêncio.
#
# JINV-011 (printf): `char argv_buf[64][256]` + strlcpy — TODO argumento acima
#   de 255 bytes saía cortado, e do 64º em diante era descartado. Os dois em
#   silêncio, com rc=0. Um agente que monta JSON/payload com printf recebia
#   corrupção indistinguível de sucesso. Os 20 usos eram só-leitura, então a
#   cópia nunca foi necessária: aponta-se direto para in_argv.
#
# JINV-012 (ls): QUATRO tetos de 32 encadeados (args_copy, argv, targets,
#   file_args/dir_args). Com 40 arquivos, `ls f*.txt` listava 32 — e o corte
#   no meio de um nome fazia o ls reportar "No such file" para arquivo que
#   EXISTE. Mentir sobre o filesystem é pior que truncar.
#
# A propriedade trancada é PARIDADE COM O BASH, não um número — se o bash
# lista N, o jash lista N.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
cd "$TMP" || exit 2

# ── JINV-011: printf ───────────────────────────────────────────────────────
BIG=$(python3 -c "print('A'*300)")
j=$("$JASH"   -c "printf '%s\n' '$BIG'" 2>/dev/null | wc -c | tr -d ' ')
b=$(/bin/bash -c "printf '%s\n' '$BIG'" 2>/dev/null | wc -c | tr -d ' ')
if [ "$j" = "$b" ]; then
    ok "printf: argumento de 300 bytes nao e truncado ($j bytes, = bash)"
else
    bad "printf 300 bytes" "jash=$j bash=$b — teto de 255 de volta"
fi

ARGS=$(python3 -c "print(' '.join(str(i) for i in range(1,100)))")
j=$("$JASH"   -c "printf '%s\n' $ARGS" 2>/dev/null | wc -l | tr -d ' ')
b=$(/bin/bash -c "printf '%s\n' $ARGS" 2>/dev/null | wc -l | tr -d ' ')
if [ "$j" = "$b" ]; then
    ok "printf: 99 argumentos, nenhum descartado ($j linhas, = bash)"
else
    bad "printf 99 args" "jash=$j bash=$b — teto de 64 argumentos de volta"
fi

# Regressao: as formas que ja funcionavam.
v=$("$JASH" -c 'printf -v out "%s-%s" a b; echo $out' 2>/dev/null)
f=$("$JASH" -c 'printf "%d %x %o %5s|\n" 255 255 255 ab' 2>/dev/null)
if [ "$v" = "a-b" ] && [ "$f" = "255 ff 377    ab|" ]; then
    ok "printf: -v e conversoes (%d/%x/%o/%5s) nao regrediram"
else
    bad "printf regressao" "-v=[$v] fmt=[$f]"
fi

# ── JINV-012: ls ───────────────────────────────────────────────────────────
mkdir -p many && cd many
for i in $(seq 1 100); do : > "f$i.txt"; done

j=$("$JASH"   -c 'ls f*.txt' 2>/dev/null | tr ' ' '\n' | grep -c 'f[0-9]')
b=$(/bin/bash -c 'ls f*.txt' 2>/dev/null | tr ' ' '\n' | grep -c 'f[0-9]')
if [ "$j" = "$b" ]; then
    ok "ls: glob com 100 arquivos lista todos ($j, = bash)"
else
    bad "ls 100 arquivos" "jash=$j bash=$b — teto de 32 de volta"
fi

# O pior sintoma: mentir sobre o filesystem.
if "$JASH" -c 'ls f*.txt' 2>&1 | grep -q 'No such file'; then
    bad "ls nao fabrica erro" "reportou 'No such file' para arquivo que EXISTE"
else
    ok "ls: nao fabrica 'No such file' para arquivo existente"
fi

# Regressao: caminhos que ja funcionavam.
n=$("$JASH" -c 'ls' 2>/dev/null | tr ' ' '\n' | grep -c 'f[0-9]')
l=$("$JASH" -c 'ls -l f1.txt' 2>/dev/null | grep -c 'f1.txt')
if [ "$n" = "100" ] && [ "$l" = "1" ]; then
    ok "ls: sem-args e -l nao regrediram"
else
    bad "ls regressao" "sem-args=$n -l=$l"
fi

# Erro REAL continua sendo reportado (nao viramos tudo silencioso).
if "$JASH" -c 'ls nao_existe_mesmo_xyz' 2>&1 | grep -q 'No such file'; then
    ok "ls: arquivo realmente ausente ainda reporta erro"
else
    bad "ls erro real" "parou de reportar arquivo inexistente"
fi

echo ""
echo "=== JINV-011/012: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
