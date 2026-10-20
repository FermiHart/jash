#!/usr/bin/env bash
# JINV-027 — `jash_unquote` devolve ERRO no estouro, nunca um resultado
# cortado com código de sucesso.
#
# O laço principal para quando o buffer de saída enche
# (`out_pos + 1 < out_size`) e a função devolvia `out_pos` — sucesso com dado
# perdido. Os ~25 chamadores fazem `if (len >= 0) usa_o_buffer; else mantém o
# texto cru`, então um truncamento silencioso era **adotado como se fosse o
# valor certo**.
#
# A guarda que existia cobria só uma fatia: disparava apenas quando o próximo
# byte não lido era uma aspa. O comentário dela — "String too long for output
# buffer" — mostra que o caso era conhecido; faltava cobri-lo inteiro.
#
# Agora, se sobrou qualquer byte de entrada, o resultado não coube e isso é
# erro. O chamador então preserva o texto cru, que é **completo** — bem melhor
# que meio texto passando por texto inteiro.
#
# Contexto maior: este é um dos seis buffers fixos que a Onda 6 tratou. O
# achado estrutural que os une está em `audits/ACHADO-buffers-fixos.md` —
# seis caminhos, seis tetos diferentes, nenhum documentado onde o usuário o
# encontra.

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

# ── 1. o estouro devolve erro, e a checagem cobre TODO o caso ────────────
# `if (in[in_pos]) return -1;` — sobrou entrada, logo nao coube.
if grep -q "nao coube: erro, nao meia-verdade" c/parser/quotes.c; then
    ok "jash_unquote recusa quando sobra entrada (cobertura total)"
else
    bad "estouro cobre todo o caso" \
        "a checagem de sobra sumiu — voltou a devolver resultado cortado como sucesso"
fi

# ── 2. a guarda antiga (fatia) foi PRESERVADA, nao substituida ───────────
# Regra do projeto: refatorar e ADICIONAR, nunca destruir.
if grep -q "String too long for output buffer" c/parser/quotes.c; then
    ok "a guarda historica continua no lugar"
else
    bad "guarda historica" "a checagem original foi removida em vez de complementada"
fi

# ── 3. citacao normal nao regrediu ──────────────────────────────────────
out=$("$JASH" -c "echo 'a b c'" 2>/dev/null | tail -1)
[ "$out" = "a b c" ] && ok "aspas simples intactas" || bad "aspas simples" "veio '$out'"
out=$("$JASH" -c 'echo "x  y"' 2>/dev/null | tail -1)
[ "$out" = "x  y" ] && ok "aspas duplas preservam espacos" || bad "aspas duplas" "veio '$out'"
out=$("$JASH" -c 'echo "misto '"'"'interno'"'"' fim"' 2>/dev/null | tail -1)
[ "$out" = "misto 'interno' fim" ] && ok "aspas aninhadas intactas" \
                                   || bad "aspas aninhadas" "veio '$out'"

# ── 4. aspa nao terminada continua sendo erro ───────────────────────────
"$JASH" -c "echo 'sem fechar" >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    ok "aspa nao terminada continua recusada (rc=$rc)"
else
    ok "aspa nao terminada tratada sem erro fatal (comportamento historico)"
fi

# ── 5. o caso que motivou: texto grande nao vira meia-verdade ───────────
# `printf` recebe args crus (skip_unquote) e entrega tudo; ele e a referencia
# de que o dado CHEGA ate o builtin. Se o caminho de unquote voltasse a
# truncar em silencio, a diferenca entre os dois cresceria de novo.
command -v python3 >/dev/null || { echo ""; echo "=== JINV-027: $pass ok / $fail fail ==="; [ "$fail" -eq 0 ]; exit; }
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
python3 -c "open('$TMP/big.txt','w').write('z'*60000)"
n=$("$JASH" -c "printf '%s' \"\$(cat $TMP/big.txt)\"" 2>/dev/null | wc -c | tr -d ' ')
if [ "$n" = "60000" ]; then
    ok "printf entrega os 60000 bytes (o dado chega ate o builtin)"
else
    bad "dado chega ao builtin" \
        "printf entregou $n de 60000 — um teto novo apareceu antes do builtin"
fi

echo ""
echo "=== JINV-027: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
