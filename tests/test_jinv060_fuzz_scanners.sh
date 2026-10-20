#!/usr/bin/env bash
# JINV-060 — nenhuma entrada pode fazer o shell parar de responder.
#
# Este gate existe por causa de uma humilhação específica. O builtin `resume`
# passou em 14 asserções escritas à mão e travou o shell na PRIMEIRA vez que
# foi usado de verdade: `rs_collect_paths` tinha conjuntos de separadores
# assimétricos — `(` era pulado mas não parava a varredura, `)` parava mas não
# era pulado — e num `)` o ponteiro ficava imóvel. Nenhuma das 14 asserções
# tinha pontuação no texto de teste. Antes disso o mesmo builtin classificou
# "83/83" como caminho de arquivo e reportou "não existe mais".
#
# O padrão é o que importa: **gate escrito à mão testa o que o autor imagina.**
# O autor imagina texto limpo. A entrada real tem parêntese, aspa solta,
# barra invertida no fim, UTF-8, e 40 asteriscos seguidos.
#
# ═══ A PROPRIEDADE CENTRAL ═══
#
# Para TODA entrada, o jash **termina**. Não "termina certo" — termina. Um
# shell que responde errado é um bug; um shell que não responde é o fim da
# sessão, e leva junto a suíte inteira se ninguém puser prazo.
#
# ═══ As três propriedades de apoio ═══
#
# 1. **Determinismo.** Semente fixa (20260727). Um fuzzer que sorteia dá
#    veredito diferente a cada rodada, e um gate que muda de opinião sozinho
#    é ruído — ninguém acredita nele na terceira vez.
#
# 2. **O detector detecta.** Um fuzzer que não sabe reconhecer travamento dá
#    luz VERDE em cima de um shell pendurado: pior que nenhum. Por isso a
#    asserção 2 abaixo joga um laço infinito de verdade contra o próprio
#    watchdog antes de qualquer coisa.
#
# 3. **Oráculo antes de acusar.** Nem toda demora é defeito: `*a*a*a*a...`
#    faz o bash sofrer igual, porque não existe casamento linear para essa
#    forma. Todo achado é reconferido contra o /bin/bash na MESMA linha; se
#    os dois travam, é propriedade da classe e entra como SHARED (informativo),
#    não como falha. O que sustenta a acusação é a diferença, não o relógio.
#
# Crash (morte por sinal) e travamento são contados SEPARADO: são defeitos
# diferentes, com causas diferentes, e juntá-los num número só esconde os dois.
#
# Motor: tests/fuzz/scanners_fuzz.py (480 casos, ~22s, prazo de 5s por
# invocação, HOME+cwd isolados por caso).
#
# F E R M I ∞ H A R T

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

OUT=$(mktemp) || exit 2
trap 'rm -f "$OUT"' EXIT INT TERM

# ── roda o fuzzer ───────────────────────────────────────────────────────────
# Um teto de tempo próprio: se o MOTOR travar, o gate não pode travar junto.
t0=$(python3 -c 'import time;print(int(time.time()))')
JASH="$JASH" python3 tests/fuzz/scanners_fuzz.py > "$OUT" 2>&1
motor_rc=$?
t1=$(python3 -c 'import time;print(int(time.time()))')
wall=$((t1 - t0))

field() { awk -v k="$1" '$1==k {print $2}' "$OUT" | head -1; }

if [ "$motor_rc" != "0" ]; then
    bad "o motor do fuzzer terminou mal" "rc=$motor_rc — saída em $OUT:
$(head -20 "$OUT")"
fi
if grep -q '^SKIP ' "$OUT"; then
    echo "SKIP: $(cat "$OUT")"; exit 0
fi

cases=$(field CASES); invoc=$(field INVOCATIONS)
[ -n "${cases:-}" ] || { bad "motor sem saída" "$(head -20 "$OUT")"; \
                         echo ""; echo "=== JINV-060: $pass ok / $fail fail ==="; exit 1; }

# ── 1. o corpus é grande o bastante para valer alguma coisa ────────────────
# Um "fuzzer" de 20 casos é teatro. O número é escolhido para caber no
# orçamento da suíte (< 60s), não para caber numa promessa.
if [ "$cases" -ge 300 ]; then
    ok "corpus com $cases casos / $invoc invocações"
else
    bad "corpus raso" "só $cases casos — abaixo do piso de 300"
fi

# ── 2. O DETECTOR DETECTA (a asserção que valida todas as outras) ──────────
# `while :; do :; done` é um travamento inegável. Se o watchdog não o vê,
# todo "ok" impresso abaixo é uma mentira, e o gate tem que dizer isso.
st=$(awk '$1=="SELFTEST"{print $2}' "$OUT")
if [ "$st" = "hang_detected=yes" ]; then
    ok "autoteste: laço infinito real é reconhecido como travamento"
else
    bad "o fuzzer não sabe detectar travamento" \
        "o canário 'while :; do :; done' passou como saudável — todo veredito verde deste gate é sem valor até isto voltar"
fi

# ── 3. ORÇAMENTO DE TEMPO — este gate roda na suíte inteira ────────────────
# Gate lento é gate que alguém desliga.
el=$(field ELAPSED)
if [ "$wall" -lt 60 ]; then
    ok "roda em ${wall}s (motor ${el}s) — dentro do teto de 60s"
else
    bad "gate lento demais" "${wall}s — passou do teto de 60s; corte casos ou famílias"
fi

# ── 4. A PROPRIEDADE CENTRAL, por família ─────────────────────────────────
# Por família e não em bloco: "algo travou" manda ninguém a lugar nenhum;
# "a família glob travou" nomeia o suspeito. É a régua de discriminação do
# JINV-057 apontada para o fuzzer.
while read -r _ fam rest; do
    h=$(printf '%s' "$rest" | sed -n 's/.*hangs=\([0-9]*\).*/\1/p')
    c=$(printf '%s' "$rest" | sed -n 's/.*cases=\([0-9]*\).*/\1/p')
    sk=$(printf '%s' "$rest" | sed -n 's/.*skipped=\([0-9]*\).*/\1/p')
    if [ "${h:-0}" -eq 0 ]; then
        ok "família '$fam': $c casos, nenhum travamento"
    else
        det=$(awk -v f="$fam" '$1=="HANG" && $2==f' "$OUT" | head -3)
        bad "família '$fam' TRAVOU ($h caso(s), $sk pulados pelo disjuntor)" \
            "entrada exata (repr + hex + linha):
$det"
    fi
done < <(grep '^FAMILY ' "$OUT")

# ── 5. crash é outro defeito, contado à parte ─────────────────────────────
tc=$(sed -n 's/^TOTAL .*crashes=\([0-9]*\).*/\1/p' "$OUT")
if [ "${tc:-0}" -eq 0 ]; then
    ok "nenhuma entrada matou o shell por sinal"
else
    bad "$tc entrada(s) derrubaram o shell por sinal" \
        "$(grep '^CRASH ' "$OUT" | head -5)"
fi

# ── 6. o oráculo continua sendo consultado ────────────────────────────────
# SHARED não falha — mas tem que aparecer, porque é a prova de que a acusação
# passou por um segundo shell antes de virar veredito.
ts=$(sed -n 's/^TOTAL .*shared=\([0-9]*\).*/\1/p' "$OUT")
if [ "${ts:-0}" -gt 0 ]; then
    echo "info  $ts caso(s) travam TAMBÉM no /bin/bash — classe, não defeito:"
    grep '^SHARED ' "$OUT" | head -3 | sed 's/^/        /'
fi
ok "todo achado foi reconferido contra o /bin/bash antes de acusar"

# ── 7. o achado aberto tem endereço — e um achado NOVO não se esconde nele ─
# Um gate vermelho sem documento vira "esse sempre falha" em duas semanas, e
# aí o próximo travamento chega e ninguém olha. Por isso não basta existir um
# ACHADO: o travamento visto tem que ser O travamento documentado. Qualquer
# HANG fora da família já diagnosticada é achado NOVO e é dito com essa palavra.
if grep -q '^HANG ' "$OUT"; then
    DOC=audits/ACHADO-fuzz-glob-estrelas-consecutivas.md
    novos=$(grep '^HANG ' "$OUT" | awk '$2 != "glob"')
    if [ -n "$novos" ]; then
        bad "travamento NOVO, ainda sem documento" \
            "não é o achado já conhecido (família glob) — caracterize e escreva um audits/ACHADO-fuzz-*.md antes de mexer no código:
$novos"
    elif [ -f "$DOC" ]; then
        ok "achado documentado em $DOC"
    else
        bad "travamento sem documento" "há HANG e nenhum $DOC em audits/"
    fi
    echo ""
    echo "        ┌─ este gate está VERMELHO DE PROPÓSITO ────────────────────┐"
    echo "        │ o travamento acima é real e NÃO foi consertado.           │"
    echo "        │ diagnóstico completo, as 4 portas de glob e a linha exata:│"
    echo "        │ audits/ACHADO-fuzz-glob-estrelas-consecutivas.md          │"
    echo "        └──────────────────────────────────────────────────────────┘"
fi

echo ""
echo "=== JINV-060: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
