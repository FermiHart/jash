#!/usr/bin/env bash
# JINV-017 — o pedido estruturado é honrado POR INTEIRO, ou recusado.
#
# `--exec-json` tinha um parser próprio, feito de `strstr`, que só entendia
# `argv`. `cwd`, `env` e `stdin` eram aceitos e IGNORADOS em silêncio —
# comprovado ao vivo antes da correção:
#
#   --exec-json '{"argv":["pwd"],"cwd":"/tmp"}'            → imprimia $PWD
#   --exec-json '{"argv":["sh","-c","echo $A"],"env":{…}}' → imprimia vazio
#   --exec-json '{"argv":["cat"],"stdin":"hello"}'         → lia /dev/null
#
# Aceitar um campo e ignorá-lo é a falha mais cara que um protocolo pode ter:
# o chamador acredita ter pedido isolamento (um `cwd` de scratch) e recebe
# execução no diretório errado, sem um único aviso. Pior ainda porque o
# protocolo COMPLETO já existia em core/serve.c — eram duas implementações do
# mesmo contrato, e a fraca era a que estava exposta na linha de comando.
#
# Correção: uma porta só (`jash_exec_json`), fail-closed.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/scratch"
REAL=$(cd "$TMP/scratch" && pwd -P)

# ── 1. cwd é aplicado de verdade ───────────────────────────────────────────
out=$("$JASH" --exec-json "{\"argv\":[\"pwd\"],\"cwd\":\"$TMP/scratch\"}" \
      2>/dev/null | tail -1)
outp=$(cd "$out" 2>/dev/null && pwd -P)
if [ "$outp" = "$REAL" ]; then
    ok "cwd aplicado (pwd -> $out)"
else
    bad "cwd aplicado" "esperado $REAL, veio '$out' — campo aceito e ignorado?"
fi

# ── 2. env é aplicado de verdade ───────────────────────────────────────────
out=$("$JASH" --exec-json \
      '{"argv":["sh","-c","echo A=$JINV017_A"],"env":{"JINV017_A":"vale"}}' \
      2>/dev/null | tail -1)
if [ "$out" = "A=vale" ]; then
    ok "env aplicado (A=vale)"
else
    bad "env aplicado" "esperado 'A=vale', veio '$out'"
fi

# ── 3. stdin é entregue de verdade ─────────────────────────────────────────
out=$("$JASH" --exec-json '{"argv":["cat"],"stdin":"linha-de-stdin"}' \
      </dev/null 2>/dev/null | tail -1)
if [ "$out" = "linha-de-stdin" ]; then
    ok "stdin entregue ao comando"
else
    bad "stdin entregue" "esperado 'linha-de-stdin', veio '$out'"
fi

# ── 4. stdin GRANDE (acima de um buffer de pipe) nao trava ─────────────────
# Um pipe travaria sozinho aqui: nao ha segundo processo drenando enquanto
# escrevemos. Por isso a porta usa arquivo temporario anonimo.
big=$(awk 'BEGIN{for(i=0;i<20000;i++) printf "x"}')
out=$("$JASH" --exec-json "{\"argv\":[\"wc\",\"-c\"],\"stdin\":\"$big\"}" \
      </dev/null 2>/dev/null | tail -1 | tr -d ' ')
if [ "$out" = "20000" ]; then
    ok "stdin de 20000 bytes entregue inteiro (sem deadlock)"
else
    bad "stdin grande" "wc -c devolveu '$out', esperado 20000"
fi

# ── 5. FAIL-CLOSED: cwd inacessivel RECUSA em vez de rodar em outro lugar ──
out=$("$JASH" --exec-json '{"argv":["pwd"],"cwd":"/nao/existe/mesmo"}' 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qi 'cwd'; then
    ok "cwd inacessivel RECUSA o pedido (rc=$rc)"
else
    bad "fail-closed no cwd" \
        "rc=$rc, saida='$out' — rodou no diretorio errado em vez de recusar"
fi

# ── 6. `line` (semantica de shell completa) tambem funciona pela mesma porta
out=$("$JASH" --exec-json '{"line":"echo um && echo dois"}' 2>/dev/null | tail -1)
if [ "$out" = "dois" ]; then
    ok "campo 'line' honrado (shell completo)"
else
    bad "campo 'line'" "esperado 'dois', veio '$out'"
fi

# ── 7. combinado: cwd + env + stdin no MESMO pedido ────────────────────────
echo conteudo > "$TMP/scratch/arquivo.txt"
out=$("$JASH" --exec-json \
   "{\"argv\":[\"sh\",\"-c\",\"cat arquivo.txt; echo \$JINV017_B; cat\"],\
     \"cwd\":\"$TMP/scratch\",\"env\":{\"JINV017_B\":\"bee\"},\
     \"stdin\":\"vindo-do-stdin\"}" </dev/null 2>/dev/null | tr '\n' ',')
case "$out" in
    *conteudo*bee*vindo-do-stdin*) ok "cwd+env+stdin juntos no mesmo pedido" ;;
    *) bad "pedido combinado" "saida='$out'" ;;
esac

# ── 8. JSON malformado FALHA ALTO (nao roda nada) ──────────────────────────
out=$("$JASH" --exec-json '{"argv":' 2>&1); rc=$?
if [ "$rc" -ne 0 ]; then
    ok "JSON malformado recusado (rc=$rc)"
else
    bad "JSON malformado" "rc=0 — aceitou lixo"
fi

echo ""
echo "=== JINV-017: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
