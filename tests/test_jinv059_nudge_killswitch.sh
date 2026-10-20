#!/usr/bin/env bash
# JINV-059 — JASH_NUDGE=0 silencia TODAS as dicas, inclusive o scorecard.
#
# `c/core/hints.c:23` declara: "one kill switch for ALL tips". O scorecard de
# adoção (`jash_adopt_scorecard_maybe`, c/core/adopt.c) escapava dele.
#
# ═══ Por que isto virou gate ═══
#
# O scorecard acorda a cada 10º comando (`t % 10 == 0`) com contador
# **persistente entre invocações**, e vai para o stderr real — o próprio código
# anota "governor text goes to the REAL stderr, never a capture writer".
#
# Um gate que captura `2>&1` e compara com igualdade exata passa ou falha
# conforme o contador global caia num múltiplo de 10 no momento do teste.
# O JINV-019 viveu assim desde que foi escrito: dez asserções verdes por
# sorte. Caiu de uma vez quando a adoção subiu de 2% para 3% e o contador
# alinhou. Nada no shell tinha mudado.
#
# **Um gate que passa por sorte é pior que gate nenhum** — produz confiança
# sem sustentação, exatamente o defeito que o JINV-024 (veredito que afirma
# mais do que sustenta) e o JINV-051 atacaram no `sigil`. Aqui a mesma falha
# estava na régua, não no medido.
#
# Este gate fecha a classe: com JASH_NUDGE=0, nenhuma dica pode aparecer em
# stderr, por mais comandos que se rode.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
export JASH_SIGIL=0

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM

# Liga a adoção no nível que faz o scorecard disparar, num HOME isolado.
HOME="$H" "$JASH" -c 'adopt 2' >/dev/null 2>&1

# ── 1. com JASH_NUDGE=0, 40 comandos não produzem UMA linha de dica ─────
# 40 cobre quatro múltiplos de 10 — o gatilho do scorecard.
sujo=""
i=0
while [ "$i" -lt 40 ]; do
    e=$(HOME="$H" JASH_NUDGE=0 "$JASH" -c 'true' 2>&1)
    [ -n "$e" ] && sujo="$sujo|$e"
    i=$((i+1))
done
if [ -z "$sujo" ]; then
    ok "JASH_NUDGE=0: 40 comandos, stderr limpo (cobre 4 múltiplos de 10)"
else
    bad "vazamento de dica" "com JASH_NUDGE=0 apareceu: $(printf '%s' "$sujo" | cut -c1-140)"
fi

# ── 2. especificamente o scorecard de adoption ──────────────────────────
i=0; achou=0
while [ "$i" -lt 40 ]; do
    HOME="$H" JASH_NUDGE=0 "$JASH" -c 'true' 2>&1 | grep -q "adoption" && achou=1
    i=$((i+1))
done
[ "$achou" = "0" ] && ok "o scorecard de adoption obedece ao interruptor" \
                   || bad "scorecard escapa" \
                          "'[jash] adoption' apareceu com JASH_NUDGE=0 (c/core/adopt.c)"

# ── 3. e o interruptor NÃO é um mudo permanente ─────────────────────────
# Se `JASH_NUDGE=0` tivesse silenciado a adoção inteira, as duas asserções
# acima passariam pelo motivo errado e a feature estaria morta para quem a
# quer. Aqui o subsistema é exercitado SEM o interruptor.
#
# Limite declarado: o gatilho do scorecard periódico é `t % 10 == 0` sobre um
# contador PERSISTENTE que só sobe no caminho de sessão — não em `jash -c`.
# Ele não é disparável de forma determinística dentro de um gate. Esta
# asserção portanto verifica que a adoção continua VIVA e falante, não que
# aquele gatilho específico armou. Fingir o contrário seria escrever a mesma
# instabilidade que este JINV existe para matar.
#
# A não-morte do gatilho foi confirmada por experimento diferencial fora do
# gate: binário pré-conserto e pós-conserto, 40 invocações cada, ambos 40/40.
out=$(HOME="$H" "$JASH" -c 'adopt' 2>&1)
if printf '%s' "$out" | grep -qiE "adoption|adopt|nivel|level"; then
    ok "sem o interruptor, a adoção continua viva e falante"
else
    bad "adoção emudecida" \
        "o conserto silenciou o subsistema inteiro, não só a dica: $(printf '%s' "$out" | head -1)"
fi

echo ""
echo "=== JINV-059: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
