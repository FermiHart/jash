#!/usr/bin/env bash
# JINV-020 — um separador dentro de aspas ANINHADAS em `$( )` é DADO, nunca
# fronteira de comando.
#
#   jash -c 'echo "[$(echo "a;b")]"'
#
# O `;` está dentro de aspas duplas que abrem DENTRO da substituição de
# comando — em POSIX a citação recomeça do zero dentro de `$( )`, então esse
# `;` é literal. bash imprime `[a;b]`. O jash quebrava a linha ali e executava
# `b)]` como um segundo comando:
#
#   $ jash -c 'echo "[$(echo "a;b")]"'
#   "[
#   b)]: command not found
#
# É a mesma classe do JINV-016: texto que deveria ser dado vira comando. Aqui
# é pior de perceber, porque o primeiro comando ainda imprime algo — a saída
# parece "quase certa" e o segundo comando roda escondido no meio do ruído.
# Foi assim que este bug se revelou: um relatório em markdown, passado por
# substituição de comando, teve o próprio corpo executado linha a linha.
#
# O que já funcionava e NÃO pode regredir: aspas simples internas, ausência de
# aspas externas, e `|`, `>`, `#` internos — o splitter de pipeline e o de
# redirect já eram quote-aware. Só o de lista (`;`, `&&`, `||`) não era.

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

# Compara jash com o bash de referencia: o oraculo e o shell que jash imita.
cmp_bash() { # cmp_bash <rotulo> <linha de comando>
    local label="$1" cmd="$2" gb gj
    gb=$(bash    -c "$cmd" 2>/dev/null | head -1)
    gj=$("$JASH" -c "$cmd" 2>/dev/null | head -1)
    if [ "$gb" = "$gj" ]; then
        ok "$label  -> '$gj'"
    else
        bad "$label" "bash='$gb'  jash='$gj'"
    fi
}

# ── 1. o caso central: ';' em aspas aninhadas ──────────────────────────────
cmp_bash "';' em aspas aninhadas"    'echo "[$(echo "a;b")]"'
cmp_bash "'&&' em aspas aninhadas"   'echo "[$(echo "a&&b")]"'
cmp_bash "'||' em aspas aninhadas"   'echo "[$(echo "a||b")]"'
cmp_bash "sem colchetes ao redor"    'echo "$(echo "x;y")"'
cmp_bash "atribuicao + uso"          'V="$(echo "a;b")"; echo "$V"'
cmp_bash "crase em vez de \$( )"     'echo "[`echo "a;b"`]"'
cmp_bash "printf com ';' no formato" 'echo "$(printf "%s;%s" a b)"'

# ── 2. o segundo comando NAO pode rodar (a parte perigosa) ────────────────
TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
cd "$TMP" || exit 2
"$JASH" -c 'echo "[$(echo "a; touch CANARIO")]"' >/dev/null 2>&1
criados=$(ls -A "$TMP" 2>/dev/null | wc -l | tr -d ' ')
cd "$OLDPWD" 2>/dev/null || cd "$(dirname "$0")/.." || exit 2
if [ "$criados" != "0" ]; then
    bad "nada e executado depois do ';' interno" \
        "apareceu(ram) $criados arquivo(s): o texto virou um segundo comando (o nome sai deformado, ex.: 'CANARIO)]')"
else
    ok "nada e executado depois do ';' interno (diretorio vazio)"
fi

# ── 3. o que ja funcionava continua funcionando ────────────────────────────
cmp_bash "aspas SIMPLES internas"    'echo "[$(echo '"'"'a;b'"'"')]"'
cmp_bash "sem aspas externas"        'echo [$(echo "a;b")]'
cmp_bash "'|' interno"               'echo "[$(echo "a|b")]"'
cmp_bash "'>' interno"               'echo "[$(echo "a>b")]"'
cmp_bash "'#' interno"               'echo "[$(echo "a#b")]"'

# ── 4. separadores REAIS continuam separando ───────────────────────────────
cmp_bash "';' de verdade separa"     'echo um; echo dois'
cmp_bash "'&&' de verdade separa"    'true && echo depois'
cmp_bash "'||' de verdade"           'false || echo alternativa'
cmp_bash "pipeline de verdade"       'echo x | tr x y'

# ── 5. aninhamento mais fundo ──────────────────────────────────────────────
cmp_bash "duas substituicoes"        'echo "[$(echo "a;b")][$(echo "c;d")]"'
cmp_bash "substituicao aninhada"     'echo "[$(echo "$(echo "p;q")")]"'

echo ""
echo "=== JINV-020: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
