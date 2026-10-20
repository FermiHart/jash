#!/usr/bin/env bash
# JINV-030 — um teto de linha declarado, e um só.
#
# Antes de `c/core/limits.h`, cada estágio escolhia o seu número, e a
# incoerência entre eles é o que produzia os cortes silenciosos de
# `audits/ACHADO-buffers-fixos.md`:
#
#   - a substituição de comando declarava 131072 bytes de capacidade;
#   - a expansão de parâmetro declarava 131072;
#   - mas o SEGMENTO de pipeline que recebia os dois aceitava 8192.
#
# A capacidade dos dois primeiros era **ficção**. E o mesmo valor aparecia
# escrito de quatro maneiras (`131072`, `128 * 1024`, `131073`, `8192`), o que
# tornava a incoerência invisível numa leitura casual.
#
# A regra que este invariante torna verificável:
#
#     um estágio nunca pode aceitar MENOS do que quem o alimenta produz.
#
# Este gate não checa números — checa **derivação**. Comparar valores só pega
# a divergência depois que alguém a escreveu; exigir uma fonte única impede
# que ela seja escrita.

set -u
cd "$(dirname "$0")/.." || exit 2

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

LIM="c/core/limits.h"
[ -f "$LIM" ] || { echo "FAIL: $LIM ausente — a fonte única sumiu"; exit 1; }

# ── 1. a fonte única existe e define os três tetos ───────────────────────
for m in JASH_LINE_CAP JASH_LINE_CAP_STACK JASH_LINE_CAP_ENVVAL; do
    if grep -qE "^#define $m " "$LIM"; then
        ok "$m definido em core/limits.h"
    else
        bad "$m definido" "sumiu da fonte única"
    fi
done

# ── 2. quem consome DERIVA, não repete o número ──────────────────────────
check_deriva() { # check_deriva <arquivo> <macro> <esperado>
    # Aceita tanto "X Y" quanto expressoes como "(2 * Y)": o macro DERIVA de Y,
    # mas nao e um numero solto.
    if grep -qE "#define $2[[:space:]]+.*$3" "$1"; then
        ok "$2 deriva de $3"
    else
        atual=$(grep -E "#define $2 " "$1" | head -1 | sed 's/  */ /g')
        bad "$2 deriva de $3" "virou número solto: $atual"
    fi
}
check_deriva c/parser/cmdsub.c      JASH_CMDSUB_CAP        JASH_LINE_CAP
check_deriva c/parser/param_expand.c JASH_PARAM_EXPAND_CAP JASH_LINE_CAP
check_deriva c/parser/pipeline.c    PIPE_SEG_MAX           JASH_LINE_CAP
check_deriva c/main.c               JASH_SCRIPT_PROC_CAP   JASH_LINE_CAP
check_deriva c/core/serve.h         JASH_EXEC_JSON_LINE_MAX JASH_LINE_CAP_STACK
check_deriva c/core/state.h         JASH_ENV_VAL_LEN       JASH_LINE_CAP_ENVVAL

# ── 3. os buffers de pilha da expansão derivam do teto de PILHA ──────────
n=$(grep -c "JASH_LINE_CAP_STACK" c/parser/expand.h 2>/dev/null || echo 0)
if [ "$n" -ge 3 ]; then
    ok "os 3 buffers de jash_expand_ctx_t derivam de JASH_LINE_CAP_STACK"
else
    bad "buffers de expansão derivam" \
        "só $n de 3 derivam — um número solto de 32768 voltou a expand.h"
fi

# ── 4. a relação de ordem é verificada em COMPILAÇÃO ─────────────────────
# Um teto de pilha maior que o canônico significa derivação trocada por número.
if grep -Eq '^[[:space:]]*_Static_assert[[:space:]]*\([[:space:]]*JASH_LINE_CAP_STACK[[:space:]]*<=[[:space:]]*JASH_LINE_CAP[[:space:]]*,' "$LIM"; then
    ok "limits.h falha a compilação se a ordem dos tetos for invertida"
else
    bad "guarda de ordem" "o _Static_assert que protege a relação sumiu"
fi

# ── 5. e a guarda REALMENTE dispara (prova, não promessa) ────────────────
if command -v cc >/dev/null; then
    T=$(mktemp -d) || exit 2
    sed 's/#define JASH_LINE_CAP  *(256 \* 1024)/#define JASH_LINE_CAP (16 * 1024)/' \
        "$LIM" > "$T/limits.h"
    mkdir -p "$T/core" && mv "$T/limits.h" "$T/core/limits.h"
    printf '#include <core/limits.h>\nint main(void){return 0;}\n' > "$T/p.c"
    if cc -I"$T" -o "$T/p" "$T/p.c" 2>/dev/null; then
        bad "a guarda dispara" \
            "reduzi JASH_LINE_CAP abaixo do teto de pilha e compilou — o _Static_assert não protege nada"
    else
        ok "a guarda DISPARA quando o canônico fica menor que o de pilha"
    fi
    rm -rf "$T"
else
    ok "sem compilador — prova da guarda pulada"
fi

# ── 6. a razão de cada teto menor está escrita ───────────────────────────
# Um teto menor sem razão ao lado é como a incoerência nasceu.
if grep -q "PILHA" "$LIM" && grep -q "VALOR DE VARIÁVEL" "$LIM"; then
    ok "cada teto menor tem a razão escrita ao lado"
else
    bad "razões escritas" \
        "as justificativas sumiram — um teto menor sem razão é um número solto com outro nome"
fi

echo ""
echo "=== JINV-030: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
