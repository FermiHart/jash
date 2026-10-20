#!/usr/bin/env bash
# JINV-055 — `assert`: uma afirmação amarrada ao seu verificador.
#
# Numa única sessão neste repositório, o agente afirmou quatro coisas falsas —
# e nenhuma foi mentira. O padrão foi sempre o mesmo: **verificou uma vez,
# continuou afirmando, o mundo mudou embaixo.**
#
# Este gate protege a propriedade que ataca isso: o estado que importa não é
# "passou", é a **transição**. Uma afirmação que era verdade e deixou de ser
# tem que gritar — porque é exatamente essa que o agente vai repetir de memória.
#
# E uma afirmação sem verificador é RECUSADA. Não é rigor decorativo: é a mesma
# régua do JINV-017 (campo aceito e ignorado), do JINV-045 (flag que devolve
# zero sem fazer nada) e do JINV-024 (veredito que afirma mais do que sustenta),
# apontada para o próprio agente.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM
J() { HOME="$H" "$JASH" -c "$1" 2>&1; }
touch "$H/alvo.txt"

# ── 1. afirmação verdadeira é aceita e verificada NA HORA ───────────────
out=$(J "assert \"alvo existe\" --check 'test -f $H/alvo.txt'")
printf '%s' "$out" | grep -q "✓" \
    && ok "afirmação verdadeira → verificada na hora" \
    || bad "registro de afirmação verdadeira" "veio: $(printf '%s' "$out" | tail -1)"

# ── 2. afirmação SEM verificador é RECUSADA ─────────────────────────────
# Este é o coração: uma afirmação que não pode ser refutada não entra.
HOME="$H" "$JASH" -c 'assert "confie em mim"' >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    ok "afirmação sem verificador é recusada (rc=$rc)"
else
    bad "recusa sem verificador" \
        "rc=0 — uma afirmação sem meio de refutação é um campo aceito e ignorado"
fi
J 'assert' | grep -q "confie em mim" \
    && bad "não registra a recusada" "a afirmação sem verificador foi gravada" \
    || ok "a afirmação recusada NÃO foi gravada"

# ── 3. a TRANSIÇÃO: era verdade, deixou de ser ─────────────────────────
# Sem isto o builtin é só uma lista de tarefas.
rm -f "$H/alvo.txt"
out=$(J 'assert')
if printf '%s' "$out" | grep -q "CAIU"; then
    ok "afirmação que deixou de valer é marcada como CAIU"
else
    bad "detecção de queda" \
        "a afirmação era verdade e deixou de ser, e o builtin não gritou"
fi
# E o rc tem que refletir — um agente checa rc, não lê texto.
HOME="$H" "$JASH" -c 'assert' >/dev/null 2>&1
[ $? -ne 0 ] && ok "rc≠0 quando há afirmação caída" \
             || bad "rc reflete queda" "rc=0 com uma afirmação caída"

# ── 4. e volta a valer quando o mundo volta ────────────────────────────
touch "$H/alvo.txt"
out=$(J 'assert')
printf '%s' "$out" | grep -q "CAIU" \
    && bad "recuperação" "continua CAIU depois que a condição voltou a valer" \
    || ok "volta a valer quando a condição volta"

# ── 5. 'nunca passou' é distinto de 'caiu' ─────────────────────────────
# Confundir os dois é perder a informação que interessa.
J "assert \"nunca foi verdade\" --check 'test -f $H/inexistente'" >/dev/null
out=$(J 'assert')
if printf '%s' "$out" | grep -q "nunca passou"; then
    ok "'nunca passou' é distinto de 'CAIU'"
else
    bad "distinção de estados" "uma que nunca valeu está sendo mostrada como caída"
fi

# ── 6. JSON com os mesmos estados ──────────────────────────────────────
if command -v python3 >/dev/null; then
    out=$(HOME="$H" "$JASH" -c 'assert --json' 2>/dev/null | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
e=[x['state'] for x in d['assertions']]
print(all(k in x for x in d['assertions'] for k in ('claim','check','state','created','last_true')),
      'nunca' in e)" 2>/dev/null)
    case "$r" in
        "True True") ok "JSON traz claim/check/state/created/last_true" ;;
        *) bad "JSON completo" "veio '$r'" ;;
    esac
else
    ok "sem python3 — JSON não verificado"
fi

# ── 7. esquecer ────────────────────────────────────────────────────────
J 'assert --forget nunca foi verdade' >/dev/null
J 'assert' | grep -q "nunca foi verdade" \
    && bad "--forget" "a afirmação continua registrada" \
    || ok "--forget remove a afirmação"

# ── 8. flag desconhecida não vira texto de afirmação (JINV-046) ───────
HOME="$H" "$JASH" -c 'assert --nao-existe' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag desconhecida recusada" \
             || bad "guarda de flags" "'--nao-existe' virou afirmação"

echo ""
echo "=== JINV-055: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
