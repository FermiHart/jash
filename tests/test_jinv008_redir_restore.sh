#!/usr/bin/env bash
# JINV-008 — um redirect aplicado NO SHELL PAI tem que ser desfeito.
#
# No caminho `lastpipe` (o último segmento de um pipeline roda no próprio
# processo do shell, sem fork), `apply_redirs` era chamado no PAI e só o stdin
# era salvo/restaurado. Um `cmd | outro > arquivo` deixava o fd 1 do SHELL
# apontando para o arquivo — para sempre.
#
# Reproduzido em 2026-07-27: `echo A | cat > saida.txt; echo DEPOIS` não
# imprimia DEPOIS. Todo comando seguinte da sessão ficava mudo. Para um agente
# é catastrófico: ele continua rodando e nada mais aparece na saída.

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

# ── 1. O bug original: stdout sobrevive a pipe+redirect ────────────────────
out=$("$JASH" -c 'echo A | cat > f1.txt; echo DEPOIS' 2>/dev/null)
if [ "$out" = "DEPOIS" ]; then
    ok "stdout do shell sobrevive a 'cmd | cmd > arquivo'"
else
    bad "stdout sobrevive ao redirect" \
        "esperado [DEPOIS], obtido [$out] — fd 1 do shell continua sequestrado"
fi

# ── 2. O redirect PRECISA ter funcionado (nao basta restaurar cedo demais) ──
if [ "$(cat f1.txt 2>/dev/null)" = "A" ]; then
    ok "o redirect continua fazendo seu trabalho (arquivo tem o conteudo)"
else
    bad "redirect funcional" "f1.txt=[$(cat f1.txt 2>/dev/null)] — restore cedo demais?"
fi

# ── 3. stderr tambem ────────────────────────────────────────────────────────
# NB: com `2> arquivo` o stdout NAO e redirecionado, entao o "A" do cat sai no
# terminal — esperado. O que se verifica e que o comando SEGUINTE ainda fala.
out=$("$JASH" -c 'echo A | cat 2> e1.txt; echo DEPOIS_ERR' 2>/dev/null)
if printf '%s' "$out" | grep -qx DEPOIS_ERR; then
    ok "stderr redirecionado no ultimo estagio nao vaza"
else
    bad "stderr nao vaza" "DEPOIS_ERR nao apareceu; saida=[$out]"
fi

# ── 4. Varios redirects seguidos nao acumulam dano ─────────────────────────
out=$("$JASH" -c 'echo 1 | cat > a.txt; echo 2 | cat > b.txt; echo 3 | cat > c.txt; echo FIM' 2>/dev/null)
if [ "$out" = "FIM" ]; then
    ok "tres pipes com redirect em sequencia: shell segue vivo"
else
    bad "sequencia de redirects" "esperado [FIM], obtido [$out]"
fi

# ── 5. Paridade com bash ────────────────────────────────────────────────────
j=$("$JASH"    -c 'echo A | cat > p.txt; echo PARIDADE' 2>/dev/null)
b=$(/bin/bash  -c 'echo A | cat > q.txt; echo PARIDADE' 2>/dev/null)
if [ "$j" = "$b" ]; then
    ok "paridade com bash ($b)"
else
    bad "paridade com bash" "jash=[$j] bash=[$b]"
fi

# ── 6. Append (>>) tambem restaura ─────────────────────────────────────────
out=$("$JASH" -c 'echo A | cat >> ap.txt; echo DEPOIS_APPEND' 2>/dev/null)
if [ "$out" = "DEPOIS_APPEND" ]; then
    ok "append (>>) no ultimo estagio nao vaza"
else
    bad "append nao vaza" "esperado [DEPOIS_APPEND], obtido [$out]"
fi

echo ""
echo "=== JINV-008: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
