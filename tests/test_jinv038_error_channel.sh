#!/usr/bin/env bash
# JINV-038 — mensagem de erro vai para o STDERR, nunca para o canal de saída
# do comando.
#
# `jash_exec_external` anunciava a falha de montagem de argv com
# `jash_writef(w, ctx, ...)` — o writer da SAÍDA do comando. Resultado: um
# pipeline recebia a mensagem de erro como **dado**:
#
#   $ jash -c '/bin/echo "$(cat arquivo_acima_do_teto)"' | wc -c
#   38          ← o tamanho da MENSAGEM DE ERRO, não do conteúdo
#
# Falhar alto está certo — não há truncamento silencioso neste caminho, e isso
# é bom. O defeito é o canal: anunciar a falha pelo stdout transforma
# diagnóstico em dado e contamina tudo o que vier depois no pipeline. É a mesma
# família dos JINV-021 e JINV-026 vista de outro ângulo — o estágio seguinte
# recebe algo plausível e errado.
#
# A mensagem também passou a dizer o TAMANHO da linha: "argv build failed (-3)"
# não diz a quem lê o que fazer a respeito.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
python3 -c "open('$T/big','w').write('z'*300000)"

# ── 1. a linha inteira é recusada, sem prefixo nem sufixo executado ───────
"$JASH" -c "/bin/echo \"\$(cat $T/big)\"; touch $T/must-not-exist" \
    >"$T/stdout" 2>"$T/stderr"
rc=$?
n=$(wc -c < "$T/stdout" | tr -d ' ')
if [ "$n" -le 1 ]; then
    ok "stdout limpo na falha ($n bytes)"
else
    bad "stdout limpo" \
        "$n bytes sairam no stdout — a mensagem de erro virou dado do pipeline"
fi
if [ "$rc" -ne 0 ] && [ ! -e "$T/must-not-exist" ]; then
    ok "overflow recusa a linha inteira com status não-zero"
else
    bad "recusa atômica" "rc=$rc; comando posterior executado=$([ -e "$T/must-not-exist" ] && echo sim || echo não)"
fi

# ── 2. e a mensagem SAI, pelo stderr ─────────────────────────────────────
err=$(head -1 "$T/stderr")
if printf '%s' "$err" | grep -q 'jash: \$(): output excede'; then
    ok "a falha é anunciada no stderr"
else
    bad "falha anunciada" \
        "stderr vazio — trocar o canal não pode virar silêncio: '$err'"
fi

# ── 3. a mensagem diz o TAMANHO (acionável, não só um código) ───────────
if printf '%s' "$err" | grep -qE "[0-9]{4,} bytes"; then
    ok "a mensagem informa o tamanho da linha"
else
    bad "mensagem acionável" \
        "não diz quantos bytes — '(-3)' sozinho não orienta ninguém: '$err'"
fi

# ── 4. num pipeline, o estágio seguinte não recebe a mensagem ───────────
saida=$("$JASH" -c "/bin/echo \"\$(cat $T/big)\" | tr -d 'z'" 2>/dev/null)
if printf '%s' "$saida" | grep -qi "jash:"; then
    bad "pipeline não recebe o erro" \
        "o estágio seguinte leu a mensagem de erro como entrada"
else
    ok "o estágio seguinte do pipeline não vê a mensagem"
fi

# Todos os três executores de substituição recusam, em vez de truncar.
for spec in "builtin:cat $T/big" "external:/bin/cat $T/big" "file:<$T/big"; do
    label=${spec%%:*}; inner=${spec#*:}
    "$JASH" -c "/bin/echo \"\$($inner)\"" >"$T/$label.out" 2>"$T/$label.err"
    sub_rc=$?
    sub_n=$(wc -c < "$T/$label.out" | tr -d ' ')
    if [ "$sub_rc" -ne 0 ] && [ "$sub_n" -le 1 ] &&
       grep -q 'jash: \$(): output excede' "$T/$label.err"; then
        ok "$label acima do teto é recusado sem saída parcial"
    else
        bad "$label recusa overflow" "rc=$sub_rc stdout=$sub_n bytes stderr=$(head -1 "$T/$label.err")"
    fi
done

# Each capture fits by itself, but their aggregate expansion exceeds the line
# ceiling. The outer builder must refuse instead of returning a valid-looking
# prefix assembled from complete individual substitutions.
python3 -c "open('$T/half-a','w').write('a'*140000); open('$T/half-b','w').write('b'*140000)"
"$JASH" -c "/bin/echo \"\$(cat $T/half-a)\$(cat $T/half-b)\"; touch $T/aggregate-ran" \
    >"$T/aggregate.out" 2>"$T/aggregate.err"
aggregate_rc=$?
if [ "$aggregate_rc" -ne 0 ] && [ ! -s "$T/aggregate.out" ] &&
   [ ! -e "$T/aggregate-ran" ] &&
   grep -q 'jash: \$(): output excede' "$T/aggregate.err"; then
    ok "expansão agregada acima do teto é recusada atomicamente"
else
    bad "recusa do agregado" "rc=$aggregate_rc stdout=$(wc -c < "$T/aggregate.out") marker=$([ -e "$T/aggregate-ran" ] && echo sim || echo não)"
fi

# A failure to create safe anonymous capture storage must fail closed before
# the external command runs; the old same-process pipe fallback could deadlock.
printf '#!/bin/sh\n/usr/bin/touch %s\nprintf resource-output\n' "$T/resource-ran" > "$T/external-probe"
chmod +x "$T/external-probe"
: > "$T/not-a-directory"
TMPDIR="$T/not-a-directory" "$JASH" -c "/bin/echo \"\$($T/external-probe)\"" \
    >"$T/resource.out" 2>"$T/resource.err"
resource_rc=$?
if [ "$resource_rc" -ne 0 ] && [ ! -e "$T/resource-ran" ] &&
   grep -q 'captura segura indisponivel' "$T/resource.err"; then
    ok "falha de recurso recusa antes de executar o comando interno"
else
    bad "captura sem recurso falha fechada" "rc=$resource_rc marker=$([ -e "$T/resource-ran" ] && echo sim || echo não)"
fi

# A refusal aborts its submitted line, not the shell process forever. The next
# top-level script line starts with a clean error channel and runs normally.
printf '/bin/echo "$(cat %s)"\necho recovered-a; echo recovered-b\n' "$T/big" \
    > "$T/recover.jsh"
recovered=$("$JASH" "$T/recover.jsh" 2>/dev/null)
if [ "$recovered" = "$(printf 'recovered-a\nrecovered-b')" ]; then
    ok "a linha seguinte não herda a recusa anterior"
else
    bad "erro não contamina linha seguinte" "saída: '$recovered'"
fi

# ── 5. o caminho normal não regrediu ─────────────────────────────────────
out=$("$JASH" -c '/bin/echo ok-externo' 2>/dev/null | tail -1)
[ "$out" = "ok-externo" ] && ok "comando externo normal intacto" \
                          || bad "comando externo" "veio '$out'"
out=$("$JASH" -c '/bin/echo a b c' 2>/dev/null | tail -1)
[ "$out" = "a b c" ] && ok "argumentos múltiplos intactos" \
                     || bad "argumentos múltiplos" "veio '$out'"
# Linhas grandes mas dentro do teto continuam passando inteiras.
python3 -c "open('$T/med','w').write('y'*5000)"
n=$("$JASH" -c "/bin/echo \"\$(cat $T/med)\"" 2>/dev/null | wc -c | tr -d ' ')
[ "$n" = "5001" ] && ok "5000 bytes ainda passam inteiros" \
                  || bad "linha média" "esperava 5001, veio $n"

echo ""
echo "=== JINV-038: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
