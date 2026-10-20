#!/usr/bin/env bash
# JINV-010 — `$( )` não pode TRAVAR quando o comando produz muito output.
#
# A captura usava um PIPE, mas escritor e leitor eram o MESMO processo:
# dup2(pipe[1], 1) -> roda o comando -> só DEPOIS lê pipe[0]. O buffer do pipe
# no kernel é fixo (64 KiB no macOS); passando disso o `write` do comando
# bloqueia esperando um leitor que só existirá quando ele terminar.
# Deadlock clássico de pipe em processo único.
#
# Reproduzido em 2026-07-27: `v=$(cat arquivo_de_200KB)` travava PARA SEMPRE.
# Um agente que faça `x=$(cat log)` num log grande pendura a sessão inteira —
# e um shell travado é pior que um shell que erra, porque não há sinal nenhum.
#
# Correção: capturar em arquivo temporário (unlink imediato). Sem buffer
# limitado, sem deadlock possível.
#
# NB: existe um teto SEPARADO de captura (JASH_CMDSUB_CAP, 128 KiB) — o
# resultado sai truncado nesse tamanho. Isso é um limite, não um travamento:
# o comando RETORNA. Este teste tranca a ausência de deadlock, não o teto.

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

# Roda com limite de tempo; ecoa TRAVOU se estourar. `perl -e alarm` porque
# `timeout` não existe no macOS base.
run_limited() { # run_limited <segundos> <codigo>
    local out rc
    out=$(perl -e 'alarm shift; exec @ARGV' "$1" "$JASH" -c "$2" 2>/dev/null)
    rc=$?
    if [ "$rc" -eq 142 ]; then printf 'TIMEOUT:'
    else printf '%s:%s' "$rc" "$out"
    fi
}

# ── 1. O bug original: 200 KB nao pode travar ──────────────────────────────
python3 -c "open('big.txt','w').write('x'*200000)"
r=$(run_limited 10 'v=$(cat big.txt); echo ${#v}')
if [ "$r" = "0:200000" ]; then
    ok "cmdsub de 200 KB retorna inteiro e rc=0"
else
    bad "cmdsub de 200 KB retorna inteiro" "esperava 0:200000, veio '$r'"
fi

# ── 2. Bem acima do buffer do kernel, varios tamanhos ──────────────────────
viol=""
for kb in 64 128 256 512; do
    python3 -c "import sys; open('s.txt','w').write('y'*(int(sys.argv[1])*1024))" "$kb"
    r=$(run_limited 10 'v=$(cat s.txt); echo ${#v}')
    if [ "$kb" -lt 256 ]; then expected="0:$((kb * 1024))"
    else expected="1:"
    fi
    [ "$r" = "$expected" ] || viol="$viol ${kb}KB(got=$r,want=$expected)"
done
if [ -z "$viol" ]; then
    ok "sub-cap passa inteiro; over-cap recusa com rc=1"
else
    bad "status/tamanho de captura" "divergencias:$viol"
fi

# ── 3. Regressao: os casos pequenos seguem exatos ──────────────────────────
a=$("$JASH" -c 'echo $(echo oi)' 2>/dev/null)
b=$("$JASH" -c 'echo $(echo $(echo profundo))' 2>/dev/null)
c=$("$JASH" -c 'x=$(printf "a\nb"); echo "$x"' 2>/dev/null | tr '\n' '/')
if [ "$a" = "oi" ] && [ "$b" = "profundo" ] && [ "$c" = "a/b/" ]; then
    ok "cmdsub pequeno/aninhado/multilinha nao regrediu"
else
    bad "cmdsub pequeno" "simples=[$a] aninhado=[$b] multilinha=[$c]"
fi

# ── 4. Status de saida do comando interno preservado ───────────────────────
rc=$("$JASH" -c 'x=$(false); echo $?' 2>/dev/null)
if [ "$rc" = "1" ]; then
    ok "\$? do comando interno preservado (BUG-074d nao regrediu)"
else
    bad "status interno" "esperado 1, obtido [$rc]"
fi

# ── 5. Nao deixa arquivo temporario para tras ──────────────────────────────
before=$(ls "${TMPDIR:-/tmp}" 2>/dev/null | grep -c jash_cmdsub || true)
"$JASH" -c 'v=$(cat big.txt); echo ${#v}' >/dev/null 2>&1
after=$(ls "${TMPDIR:-/tmp}" 2>/dev/null | grep -c jash_cmdsub || true)
if [ "$after" -le "$before" ]; then
    ok "nao vaza arquivo temporario (unlink imediato)"
else
    bad "sem vazamento de tmp" "antes=$before depois=$after"
fi

echo ""
echo "=== JINV-010: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
