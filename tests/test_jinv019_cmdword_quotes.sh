#!/usr/bin/env bash
# JINV-019 — a palavra de comando é extraída respeitando aspas.
#
# Havia TRÊS cópias da extração da primeira palavra — eval.c:145,
# pipeline.c:112, main.c:433 — e nenhuma rastreava estado de aspas: todas
# cortavam no primeiro espaço. Um nome de comando legítimo com espaço:
#
#   jash -c "'/opt/my tool/run' arg"    →  '/opt/my: command not found
#
# O truncamento produzia `'/opt/my`, o `jash_unquote` seguinte batia no
# "aspa não terminada" e devolvia -1, e a guarda `if (len >= 0)` engolia a
# falha em silêncio. O usuário recebia um "command not found" apontando para
# um arquivo que ele nunca escreveu — o erro mais caro de depurar, porque a
# mensagem mente sobre o que foi procurado.
#
# Este teste é irmão do JINV-016: lá, argv[0] passou a ser citado; se a
# extração não honrasse aspas, a correção de segurança teria quebrado todo
# nome de comando com espaço. Os dois gates se seguram mutuamente.

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
mkdir -p "$TMP/um dir"
TOOL="$TMP/um dir/uma ferramenta"
printf '#!/bin/sh\necho tool-ok "$@"\n' > "$TOOL"
chmod +x "$TOOL"

# ── 1. nome citado com espaço, direto no shell ─────────────────────────────
out=$("$JASH" -c "'$TOOL' arg1" 2>/dev/null | tail -1)
if [ "$out" = "tool-ok arg1" ]; then
    ok "nome de comando citado com espaco executa"
else
    bad "nome citado com espaco" "veio '$out' (truncado no espaco?)"
fi

# ── 2. o mesmo por escape de barra ─────────────────────────────────────────
esc=$(printf '%s' "$TOOL" | sed 's/ /\\ /g')
out=$("$JASH" -c "$esc arg1" 2>/dev/null | tail -1)
if [ "$out" = "tool-ok arg1" ]; then
    ok "nome com espaco escapado por barra executa"
else
    bad "escape por barra" "veio '$out'"
fi

# ── 3. aspas duplas tambem ─────────────────────────────────────────────────
out=$("$JASH" -c "\"$TOOL\" arg1" 2>/dev/null | tail -1)
if [ "$out" = "tool-ok arg1" ]; then
    ok "nome entre aspas duplas executa"
else
    bad "aspas duplas" "veio '$out'"
fi

# ── 4. dentro de pipeline (a segunda copia da extracao) ────────────────────
out=$("$JASH" -c "'$TOOL' x | cat" 2>/dev/null | tail -1)
if [ "$out" = "tool-ok x" ]; then
    ok "funciona em pipeline (copia do pipeline.c unificada)"
else
    bad "pipeline" "veio '$out'"
fi

# ── 5. via --exec-json (o motivo pelo qual isto virou bloqueante) ──────────
out=$("$JASH" --exec-json "{\"argv\":[\"$TOOL\",\"arg1\"]}" 2>/dev/null | tail -1)
if [ "$out" = "tool-ok arg1" ]; then
    ok "--exec-json com argv[0] contendo espaco"
else
    bad "--exec-json argv[0] com espaco" "veio '$out'"
fi

# ── 6. o caminho comum NAO regrediu ────────────────────────────────────────
out=$("$JASH" -c 'echo comum' 2>/dev/null | tail -1)
[ "$out" = "comum" ] && ok "comando comum sem aspas intacto" \
                     || bad "comando comum" "veio '$out'"

out=$("$JASH" -c "'echo' citado" 2>/dev/null | tail -1)
[ "$out" = "citado" ] && ok "builtin citado sem espaco intacto" \
                      || bad "builtin citado" "veio '$out'"

# ── 7. subshell e brace group continuam sendo palavras-chave ──────────────
out=$("$JASH" -c '(echo sub)' 2>/dev/null | tail -1)
[ "$out" = "sub" ] && ok "subshell '(' ainda e keyword" \
                   || bad "subshell" "veio '$out'"

out=$("$JASH" -c '{ echo brace; }' 2>/dev/null | tail -1)
[ "$out" = "brace" ] && ok "brace group '{' ainda e keyword" \
                     || bad "brace group" "veio '$out'"

# ── 8. aspa NAO terminada continua sendo erro (nao vira comando estranho) ──
# JINV-019: o rc TEM que ser o do jash. `x=$(cmd | tail -1); rc=$?` captura o
# rc do `tail` (sempre 0) — esta assercao nao podia falhar por rc antes disto.
# Aqui o stderr E desejado: a assercao procura a mensagem de diagnostico.
raw=$("$JASH" -c "'sem-fechar arg" 2>&1); rc=$?
out=$(printf '%s\n' "$raw" | tail -1)
if [ "$rc" -ne 0 ] || printf '%s' "$out" | grep -qi 'quote\|not found\|aspas'; then
    ok "aspa nao terminada continua diagnosticada (rc=$rc)"
else
    bad "aspa nao terminada" "rc=$rc saida='$out' — aceitou linha malformada"
fi

echo ""
echo "=== JINV-019: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
