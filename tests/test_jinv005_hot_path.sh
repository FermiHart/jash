#!/usr/bin/env bash
# JINV-005 / JINV-006 — o caminho quente não pode ser O(histórico).
#
# JINV-005: `metrics_read` somava o TSV INTEIRO a cada `jash -c`, e o
#           `jash_adopt_scorecard_maybe` chamava isso no epílogo de TODA
#           invocação — inclusive nos 90% dos casos em que o gate `t % 10`
#           descartava o resultado. Arquivo real medido em 2026-07-27: 684 KB /
#           36.233 linhas, crescendo ~65 KB/hora, custando 83-147 ms POR
#           COMANDO, sem rotação. Correção: sidecar de UMA linha com os totais.
# JINV-006: Release usava `-Oz` (mínimo tamanho) — 30-39% de CPU a mais no
#           interpretador, zero ganho de startup. Agora `-O2`, com `-Oz` sob
#           -DJASH_OPTIMIZE_SIZE=ON para o caso de embedding.
#
# O teste NÃO afirma um tempo absoluto (varia por máquina/carga). Afirma a
# PROPRIEDADE que estava quebrada: o custo por comando não pode crescer com o
# tamanho do histórico.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="./build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
export JASH_SIGIL=0

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

HOME_BAK="$HOME"
SANDBOX=$(mktemp -d) || exit 2
trap 'export HOME="$HOME_BAK"; rm -rf "$SANDBOX"' EXIT INT TERM
export HOME="$SANDBOX"
mkdir -p "$SANDBOX/.jash"

TSV="$SANDBOX/.jash/agent_metrics.tsv"
SUM="$SANDBOX/.jash/agent_metrics.sum"

# ── 1. O sidecar é criado na primeira leitura ──────────────────────────────
# Semeia um TSV pequeno e força uma leitura das métricas. O leitor lifetime
# real e `agent-guard status` (JINV-059 tirou a leitura lifetime do epilogo
# de toda invocacao -c — era a fonte de flakiness dos gates; `true` aqui
# nao aciona mais metrics_read e o sidecar legitimamente nao nasce).
printf '1700000000\t10\t5\t2\t0\t1\n' > "$TSV"
"$JASH" -c 'agent-guard status' >/dev/null 2>&1
if [ -f "$SUM" ]; then
    ok "sidecar de totais e criado (JINV-005)"
else
    bad "sidecar criado" "nenhum agent_metrics.sum apos leitura — caminho O(1) ausente"
fi

# ── 2. O sidecar bate com a varredura completa ─────────────────────────────
# Se o indice divergir do TSV, o numero mostrado ao usuario passa a ser mentira.
if [ -f "$SUM" ]; then
    sum_total=$(awk '{print $1}' "$SUM")
    tsv_total=$(awk -F'\t' '{s+=$2} END{print s+0}' "$TSV")
    if [ "$sum_total" = "$tsv_total" ]; then
        ok "sidecar bate com a soma do TSV ($sum_total)"
    else
        bad "sidecar consistente" "sidecar=$sum_total mas TSV soma $tsv_total"
    fi
fi

# ── 3. A PROPRIEDADE central: custo por comando nao cresce com o historico ──
# Mede N comandos com TSV pequeno, depois infla o TSV em ~40x e mede de novo.
# Antes do fix, o segundo tempo era varias vezes o primeiro (varredura total).
bench() {  # bench -> milissegundos totais de 15 invocacoes
    python3 - "$JASH" <<'PY'
import subprocess, sys, time
j = sys.argv[1]
t0 = time.time()
for _ in range(15):
    subprocess.run([j, "-c", "echo hi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(int((time.time() - t0) * 1000))
PY
}

t_small=$(bench)
# infla o historico: ~4000 linhas (~120 KB), ordem do que uma sessao real acumula
python3 - "$TSV" <<'PY'
import sys
with open(sys.argv[1], "a") as f:
    for i in range(4000):
        f.write("17000000%02d\t3\t1\t1\t0\t0\n" % (i % 100))
PY
rm -f "$SUM"          # forca reconstrucao do indice a partir do TSV grande
"$JASH" -c 'agent-guard status' >/dev/null 2>&1   # reconstroi uma vez
t_big=$(bench)

# Tolerancia generosa (2.5x) para absorver ruido de maquina: o que importa e
# que NAO seja proporcional ao tamanho do arquivo (que cresceu ~40x).
limit=$(( t_small * 25 / 10 + 150 ))
if [ "$t_big" -le "$limit" ]; then
    ok "custo/comando estavel com historico 40x maior (${t_small}ms -> ${t_big}ms)"
else
    bad "custo/comando estavel" \
        "historico 40x maior levou ${t_big}ms vs ${t_small}ms (limite ${limit}ms) — voltou a ser O(arquivo)?"
fi

# ── 4. JINV-006: o Release nao pode voltar para -Oz por padrao ─────────────
if grep -q 'JASH_OPTIMIZE_SIZE' CMakeLists.txt && \
   grep -qE 'set\(CMAKE_C_FLAGS_RELEASE "-O2' CMakeLists.txt; then
    ok "Release usa -O2 por padrao, -Oz sob JASH_OPTIMIZE_SIZE (JINV-006)"
else
    bad "Release em -O2" "CMakeLists voltou a fixar -Oz (30-39% de CPU no interpretador)"
fi

echo ""
echo "=== JINV-005/006: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
