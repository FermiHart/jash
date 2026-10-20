#!/usr/bin/env bash
# JINV-009 — o resultado de um glob não pode sofrer word splitting.
#
# A expansão do jash é reescrita de STRING e a separação de campos fica a cargo
# do tokenizador de espaços a jusante. Sem um sentinela, um espaço que veio de
# DENTRO de um nome de arquivo é indistinguível de um separador — e
# `for f in *` parte "meu arquivo.txt" em duas palavras.
#
# Reproduzido em 2026-07-27: diretório com 2 arquivos (um com espaço no nome)
# era contado como 4 pelo `for`. Correção: blindar o espaço interno com
# JASH_PROT_SPACE (0x06), restaurado por jash_quote_unshield — o mesmo
# mecanismo já usado para " (0x03), ' (0x04) e \ (0x05).
#
# Este teste também vigia o RISCO da correção: um sentinela que vaze para a
# saída seria pior que o bug original.

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

: > "arquivo com espaco.txt"
: > "outro.txt"
: > "tres  espacos  aqui.txt"

# ── 1. O bug original: contagem correta ────────────────────────────────────
n=$("$JASH" -c 'n=0; for f in *; do n=$((n+1)); done; echo $n' 2>/dev/null)
b=$(/bin/bash -c 'n=0; for f in *; do n=$((n+1)); done; echo $n' 2>/dev/null)
if [ "$n" = "$b" ]; then
    ok "for f in * conta $n (igual ao bash)"
else
    bad "contagem do for" "jash=$n bash=$b — glob ainda sofre word splitting"
fi

# ── 2. Os nomes chegam INTEGROS ────────────────────────────────────────────
if "$JASH" -c 'for f in *; do echo "$f"; done' 2>/dev/null \
     | grep -qx 'arquivo com espaco.txt'; then
    ok "nome com espaco chega integro ao corpo do laco"
else
    bad "nome integro" "'arquivo com espaco.txt' saiu partido ou alterado"
fi

# ── 3. Espacos MULTIPLOS tambem ────────────────────────────────────────────
if "$JASH" -c 'for f in *; do echo "$f"; done' 2>/dev/null \
     | grep -qx 'tres  espacos  aqui.txt'; then
    ok "espacos multiplos preservados"
else
    bad "espacos multiplos" "'tres  espacos  aqui.txt' foi alterado"
fi

# ── 4. RISCO DA CORRECAO: o sentinela nao pode vazar ───────────────────────
# Um 0x06 na saida seria pior que o bug — dado corrompido em vez de partido.
if "$JASH" -c 'for f in *; do echo "$f"; done' 2>/dev/null | od -An -c | grep -q '006'; then
    bad "sentinela nao vaza" "byte 0x06 apareceu na SAIDA — unshield faltando em algum caminho"
else
    ok "sentinela 0x06 nao vaza para a saida"
fi

# ── 5. O arquivo e realmente alcancavel pelo nome expandido ────────────────
# Prova que o nome nao so *parece* certo — ele funciona como caminho real.
if "$JASH" -c 'for f in *; do [ -f "$f" ] || exit 1; done; echo TODOS_EXISTEM' 2>/dev/null \
     | grep -qx TODOS_EXISTEM; then
    ok "todo nome expandido e um caminho valido (test -f passa)"
else
    bad "nome utilizavel" "algum nome expandido nao resolve como arquivo real"
fi

# ── 6. Regressao: glob normal (sem espacos) segue funcionando ──────────────
mkdir -p sub && (cd sub && : > a.txt && : > b.txt && : > c.txt)
n=$("$JASH" -c 'n=0; for f in sub/*; do n=$((n+1)); done; echo $n' 2>/dev/null)
if [ "$n" = "3" ]; then
    ok "glob sem espacos nao regrediu (3 arquivos)"
else
    bad "glob comum" "esperado 3, obtido $n"
fi

echo ""
echo "=== JINV-009: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
