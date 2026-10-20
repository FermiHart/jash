#!/usr/bin/env bash
# JINV-016 — argv[0] tem que ser citado como QUALQUER outro token.
#
# A tese declarada da entrada estruturada (serve.h; FEATURES.md §5.2) é: "argv
# é pré-tokenizado — a classe BUG-059 não pode existir por construção". Mas
# `srv_argv_to_line` emitia o token 0 BARE, então ele voltava a ser TEXTO DE
# SHELL. Comprovado ao vivo, nas DUAS portas, antes da correção:
#
#   --exec-json '{"argv":["echo hi; touch /tmp/pwned"]}'   → executou 2 comandos
#   echo '{"id":1,"argv":["echo hi; touch /tmp/pwned"]}' | jash --serve   → idem
#
# Um agente que monta argv a partir de dados não-confiáveis — que é o caso de
# uso INTEIRO deste protocolo — entregava execução arbitrária justamente pelo
# campo que a documentação chamava de mais seguro.
#
# Este teste tenta a injeção pelas duas portas e exige que o arquivo-canário
# NÃO apareça. Sem isto, a regressão volta calada: o comando "funciona", só
# executa demais.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
CANARY="$TMP/pwned"

# ── 1. injeção por ';' via --exec-json ─────────────────────────────────────
rm -f "$CANARY"
"$JASH" --exec-json "{\"argv\":[\"echo hi; touch $CANARY\"]}" >/dev/null 2>&1
if [ -f "$CANARY" ]; then
    bad "--exec-json: ';' em argv[0] nao executa" \
        "o canario $CANARY foi criado — argv[0] voltou a ser emitido bare"
else
    ok "--exec-json: ';' em argv[0] nao vira segundo comando"
fi

# ── 2. mesma injeção via --serve (a porta que os agentes usam) ─────────────
rm -f "$CANARY"
echo "{\"id\":1,\"argv\":[\"echo hi; touch $CANARY\"]}" \
    | "$JASH" --serve >/dev/null 2>&1
if [ -f "$CANARY" ]; then
    bad "--serve: ';' em argv[0] nao executa" \
        "o canario $CANARY foi criado pela porta do daemon"
else
    ok "--serve: ';' em argv[0] nao vira segundo comando"
fi

# ── 3. outros metacaracteres no token 0 ────────────────────────────────────
for meta in '&&' '||' '|' '$(' '`'; do
    rm -f "$CANARY"
    "$JASH" --exec-json "{\"argv\":[\"echo hi $meta touch $CANARY\"]}" \
        >/dev/null 2>&1
    if [ -f "$CANARY" ]; then
        bad "argv[0] com '$meta' nao executa" "canario criado via '$meta'"
        break
    fi
done
[ ! -f "$CANARY" ] && ok "argv[0] com && || | \$( \` — nenhum executa"

# ── 4. e o caminho LEGÍTIMO continua funcionando ───────────────────────────
# Citar argv[0] nao pode quebrar despacho de builtin nem de binario.
out=$("$JASH" --exec-json '{"argv":["echo","funciona"]}' 2>/dev/null | tail -1)
if [ "$out" = "funciona" ]; then
    ok "comando comum ainda despacha ('echo funciona')"
else
    bad "comando comum despacha" "esperado 'funciona', veio '$out'"
fi

out=$("$JASH" --exec-json '{"argv":["pwd"]}' 2>/dev/null | tail -1)
if [ -n "$out" ] && [ -d "$out" ]; then
    ok "builtin citado ainda despacha (pwd -> $out)"
else
    bad "builtin citado despacha" "pwd devolveu '$out'"
fi

# ── 5. o argumento continua LITERAL (a garantia original) ──────────────────
out=$("$JASH" --exec-json '{"argv":["echo","a b","$HOME","`id`"]}' 2>/dev/null | tail -1)
if [ "$out" = 'a b $HOME `id`' ]; then
    ok "argumentos permanecem literais (sem split, sem expansao)"
else
    bad "argumentos literais" "esperado 'a b \$HOME \`id\`', veio '$out'"
fi

echo ""
echo "=== JINV-016: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
