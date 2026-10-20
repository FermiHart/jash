#!/usr/bin/env bash
# JINV-007 — um script grande demais nunca é executado PELA METADE com rc 0.
#
# O buffer de leitura de script era fixo em 64 KiB (c/main.c). Passando disso,
# `65536 - script_len - 1` chegava a 0, o `sys_read` devolvia 0, o laço
# terminava, e o script TRUNCADO era executado com exit 0.
#
# Reproduzido em 2026-07-27 com 70.021 bytes: o bash chegava ao fim, o jash
# executava só o começo e reportava sucesso. Para um shell de agente essa é a
# pior classe de defeito — o LLM gera um script longo, metade roda, e o rc=0
# afirma que deu tudo certo.
#
# A propriedade que este teste tranca é EXECUTE-TUDO-OU-FALHE-ALTO, não um
# tamanho específico. Existe um segundo teto conhecido em ~128 KiB
# (JASH_PARAM_EXPAND_CAP no expansor) — mas ele falha com rc != 0, que é o
# comportamento aceitável. O inaceitável é rc 0 com execução parcial.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="./build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

# Gera um script de N KiB cujo ÚLTIMO comando imprime um marcador único.
gen() { # gen <kb> <arquivo>
    python3 - "$1" "$2" <<'PY'
import sys
kb, path = int(sys.argv[1]), sys.argv[2]
lines = ['echo INICIO']
n = 0
while sum(len(l) + 1 for l in lines) < kb * 1024:
    n += 1
    lines.append('v%d=%d' % (n, n))
lines.append('echo FIM_ALCANCADO')
open(path, 'w').write('\n'.join(lines) + '\n')
PY
}

# ── 1. O bug original: 70 KiB tem que rodar ATE O FIM ──────────────────────
gen 70 "$TMP/s70.sh"
out=$("$JASH" "$TMP/s70.sh" 2>/dev/null)
if printf '%s' "$out" | grep -qx FIM_ALCANCADO; then
    ok "script de 70 KiB executa ate o fim (era truncado em 64 KiB)"
else
    bad "script de 70 KiB completo" \
        "o marcador final nao apareceu — voltou a truncar em 64 KiB"
fi

# ── 2. Sanidade: um script pequeno continua funcionando ────────────────────
gen 4 "$TMP/s4.sh"
if "$JASH" "$TMP/s4.sh" 2>/dev/null | grep -qx FIM_ALCANCADO; then
    ok "script pequeno continua funcionando"
else
    bad "script pequeno" "regressao no caminho comum"
fi

# ── 3. A PROPRIEDADE central: nunca parcial-com-sucesso ────────────────────
# Para varios tamanhos, o par (chegou ao fim, exit code) so pode ser
# (sim, 0) ou (nao, !=0). O proibido e (nao, 0) — executou metade e mentiu.
viol=""
for kb in 70 100 150 300; do
    gen "$kb" "$TMP/x.sh"
    out=$("$JASH" "$TMP/x.sh" 2>/dev/null); rc=$?
    if printf '%s' "$out" | grep -qx FIM_ALCANCADO; then
        [ "$rc" -ne 0 ] && viol="$viol ${kb}KB(completo-mas-rc=$rc)"
    else
        [ "$rc" -eq 0 ] && viol="$viol ${kb}KB(PARCIAL-com-rc=0)"
    fi
done
if [ -z "$viol" ]; then
    ok "nenhum tamanho executa parcialmente reportando sucesso"
else
    bad "execute-tudo-ou-falhe-alto" "violacoes:$viol"
fi

# ── 4. Paridade com o bash no tamanho que o bug quebrava ───────────────────
gen 70 "$TMP/p.sh"
j=$("$JASH" "$TMP/p.sh" 2>/dev/null | tail -1)
b=$(/bin/bash "$TMP/p.sh" 2>/dev/null | tail -1)
if [ "$j" = "$b" ]; then
    ok "paridade com bash em 70 KiB (ambos: $b)"
else
    bad "paridade com bash" "jash=[$j] bash=[$b]"
fi

echo ""
echo "=== JINV-007: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
