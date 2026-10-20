#!/usr/bin/env bash
# JINV-039 — os buffers grandes do ciclo de recursão saem da PILHA.
#
# Uma recursão de função custa ~300 KB de pilha por nível (medido pelo dono,
# documentado em `eval.c`), e o teto de 32 chamadas é o piso de segurança
# calibrado nesse custo — não um bug. Cada buffer grande que sai da pilha é
# teto que **poderia** subir depois.
#
# Dois candidatos de 16 KB estavam no ciclo:
#   eval.c  `char seg[16384]`       — executor de lista, por segmento
#   eval.c  `char unq_args[16384]`  — despacho de builtin, por chamada
#
# Ambos migrados para `eval_depth_buf` — o mecanismo de slot-por-profundidade
# que JÁ existia no arquivo (criado para o `jash_expand_ctx_t` de ~104 KB).
# Mesma capacidade, mesmo comportamento de estouro; muda só ONDE moram.
#
# ═══ O QUE A MEDIÇÃO MOSTROU, E QUE NÃO ERA O ESPERADO ═══
#
# Com o guarda temporariamente elevado a 190 para achar o limite físico:
#
#   profundidade 25, 31, 40, 60  → completa
#   profundidade 80, 120, 200    → morre
#
# O piso de crash continua entre **60 e 80** — o mesmo intervalo que o código
# já documentava ANTES desta migração. Os 32 KB removidos eram ~10% dos
# ~300 KB por nível: **não eram o custo dominante.**
#
# Por isso o teto de 32 **permanece 32**. Subi-lo com base numa melhora que a
# medição não confirma seria trocar uma margem de segurança real por uma
# esperança — exatamente o hábito que esta auditoria inteira combate.
#
# O que sobra do custo está espalhado em buffers menores e nos próprios quadros
# de chamada do ciclo `jash_eval_line → …  → jash_eval_line`. Encontrá-lo pede
# um profiler de pilha, não mais palpite — registrado na lista de tarefas.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# ── 1. os dois buffers saíram da pilha ───────────────────────────────────
if grep -q "char seg\[16384\]" c/parser/eval.c; then
    bad "seg fora da pilha" "voltou a ser array de pilha no ciclo de recursão"
else
    ok "seg usa slot por profundidade (fora da pilha)"
fi
if grep -q "char unq_args\[16384\]" c/parser/eval.c; then
    bad "unq_args fora da pilha" "voltou a ser array de pilha"
else
    ok "unq_args usa slot por profundidade (fora da pilha)"
fi
n=$(grep -c "eval_depth_buf" c/parser/eval.c)
if [ "$n" -ge 5 ]; then
    ok "o mecanismo existente é reusado ($n usos de eval_depth_buf)"
else
    bad "reuso do mecanismo" "só $n usos — alguém inventou outro caminho?"
fi

# ── 2. o teto continua sendo o piso de segurança calibrado ──────────────
teto=$(grep -oE '#define JASH_FUNC_MAX_DEPTH +[0-9]+' c/parser/eval.c | grep -oE '[0-9]+$')
if [ "$teto" = "32" ]; then
    ok "JASH_FUNC_MAX_DEPTH continua 32 (a medição não autorizou subir)"
else
    bad "teto inalterado" \
        "virou $teto — o piso de crash medido segue entre 60 e 80; subir sem margem de ~35% troca segurança por esperança"
fi

# ── 3. recursão legítima funciona até o teto ────────────────────────────
python3 - "$T" <<'PY'
import sys
t = sys.argv[1]
corpo = ('conta() {\n'
         '  if [ "$1" -le 0 ]; then echo FIM; return 0; fi\n'
         '  conta $(( $1 - 1 ))\n'
         '}\n')
for d in (5, 20, 31):
    open("%s/r%d.sh" % (t, d), "w").write(corpo + "conta %d\n" % d)
open("%s/r40.sh" % t, "w").write(corpo + "conta 40\n")
PY
for d in 5 20 31; do
    out=$(JASH_LOOPGUARD=off JASH_NUDGE=0 "$JASH" "$T/r$d.sh" 2>/dev/null | tail -1)
    if [ "$out" = "FIM" ]; then
        ok "recursão de profundidade $d completa"
    else
        bad "recursão profundidade $d" "esperava FIM, veio '$out'"
    fi
done

# ── 4. e além do teto RECUSA alto, sem derrubar o processo ──────────────
# Um shell que é o cérebro de comandos do SO não pode morrer por SIGSEGV.
out=$(JASH_LOOPGUARD=off JASH_NUDGE=0 "$JASH" "$T/r40.sh" 2>&1 | tail -1)
if printf '%s' "$out" | grep -qi "recursion depth"; then
    ok "além do teto: recusa com mensagem, processo vivo"
else
    bad "recusa alta" "esperava aviso de profundidade, veio '$out'"
fi

# ── 5. o comportamento observável não mudou com a migração ─────────────
out=$("$JASH" -c 'echo a; echo b && echo c' 2>/dev/null | tr '\n' ' ')
[ "$out" = "a b c " ] && ok "executor de lista intacto" \
                      || bad "executor de lista" "veio '$out'"
out=$("$JASH" -c "echo 'x  y'" 2>/dev/null)
[ "$out" = "x  y" ] && ok "despacho de builtin com aspas intacto" \
                    || bad "despacho de builtin" "veio '$out'"

echo ""
echo "=== JINV-039: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
