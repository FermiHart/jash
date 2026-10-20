#!/usr/bin/env bash
# JINV-021 — um segmento de pipeline não é cortado em silêncio.
#
# `split_pipeline` copiava cada segmento para um `char seg_buf[...][8192]` e,
# no estouro, fazia `if (slen >= 8192) slen = 8192 - 1;` — cortava e seguia.
# (O comentário acima da declaração dizia "bumped from 512 to 16384"; o código
# dizia 8192. A documentação já não batia com o teto real.)
#
# O teto era incoerente com quem o alimenta: uma substituição de comando
# produz até JASH_CMDSUB_CAP (128 KB) e a expansão de parâmetro até
# JASH_PARAM_EXPAND_CAP (128 KB). Com 8 KB no segmento, essas capacidades eram
# ficção — `V="$(cat arquivo_de_30KB)"` perdia dados sem um único aviso.
#
# Medido antes: `echo "$(cat arquivo_de_30000_bytes)" | wc -c` devolvia 8187
# no jash e 30001 no bash.
#
# Agora o teto do SEGMENTO e PIPE_SEG_MAX (128 KB), casado com quem o
# alimenta, e o estouro RECUSA com mensagem em vez de truncar — porque um
# corte deixa texto solto que o parser pode reinterpretar como comando (foi
# exatamente o que aconteceu no JINV-022).
#
# LIMITE RESIDUAL, MEDIDO E DECLARADO: o teto do segmento deixou de ser o
# gargalo, mas a montagem da linha JA EXPANDIDA ainda tem um teto de 16 KB
# mais acima na cadeia (ha buffers de 16384 em eval.c). Ou seja: uma
# substituicao de 30 KB atravessa o pipeline ate 16 KB, nao ate 128 KB.
#
# Este gate NAO finge que esta resolvido. Ele trava uma CATRACA: o teto
# efetivo era 8187 antes, e nao pode voltar a cair. Quem for atras do proximo
# elo tem aqui o numero exato para bater.
#
# O corte no estagio de lista (eval.c) tambem deixou de ser mudo: ele agora
# RECUSA o comando em vez de executar uma versao truncada — um comando cortado
# no meio nao e o comando que o usuario escreveu.
#
# ACHADO ABERTO (medido, nao corrigido): acima de ~32 KB de linha expandida,
# o corte cai ANTES do '|' e o estagio do pipeline simplesmente SOME — o
# comando muda de significado em silencio (`... | wc -c` vira `...`). E a
# forma mais perigosa desta familia e merece o proximo invariante.

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
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

mk() { python3 -c "open('$TMP/big.txt','w').write('z'*$1)"; }

# ── 1. paridade com o bash ate o teto efetivo ─────────────────────────────
for n in 4000 10000 16000; do
    mk "$n"
    gj=$("$JASH" -c "echo \"\$(cat $TMP/big.txt)\" | wc -c" 2>/dev/null | tr -d ' ')
    gb=$(bash    -c "echo \"\$(cat $TMP/big.txt)\" | wc -c" 2>/dev/null | tr -d ' ')
    if [ "$gj" = "$gb" ]; then
        ok "substituicao de $n bytes atravessa o pipeline inteira ($gj)"
    else
        bad "substituicao de $n bytes" \
            "jash=$gj bash=$gb — o segmento cortou (antes do JINV-021: parava em 8187)"
    fi
done

# ── 1b. CATRACA: o teto efetivo nao pode regredir ─────────────────────────
# Era 8187 antes do JINV-021. Se alguem reintroduzir um buffer menor no
# caminho, este numero cai e o teste denuncia.
# (Sonda em 30000: acima de ~32 KB a linha e cortada ANTES do proprio '|',
#  entao nao ha numero para medir — ver o achado aberto no cabecalho.)
mk 30000
efetivo=$("$JASH" -c "echo \"\$(cat $TMP/big.txt)\" | wc -c" 2>/dev/null | tr -d ' ')
case "$efetivo" in
    ''|*[!0-9]*) bad "teto efetivo mensuravel" "wc devolveu '$efetivo'" ;;
    *) if [ "$efetivo" -ge 16384 ]; then
           ok "teto efetivo = $efetivo bytes (catraca: >= 16384; era 8187)"
       else
           bad "catraca do teto efetivo" \
               "caiu para $efetivo (minimo 16384) — um buffer menor voltou ao caminho"
       fi ;;
esac

# ── 2. o teto do segmento deriva da MESMA fonte que o produtor ──────
# Depois do JINV-030 os dois derivam de JASH_LINE_CAP (core/limits.h). A
# verificacao deixou de ser 'os numeros batem' e passou a ser 'ha uma fonte
# so' — que e a propriedade que impede a incoerencia de voltar. Comparar
# numeros literais so pegava a divergencia DEPOIS de alguem a escrever.
if grep -Eq '^#define PIPE_SEG_MAX[[:space:]]+\(2 \* JASH_LINE_CAP\)([[:space:]]|$)' c/parser/pipeline.c && \
   grep -Eq '^#define JASH_CMDSUB_CAP[[:space:]]+JASH_LINE_CAP([[:space:]]|$)' c/parser/cmdsub.c; then
    ok "segmento e substituicao derivam ambos de JASH_LINE_CAP"
else
    bad "fonte unica de teto" \
        "PIPE_SEG_MAX ou JASH_CMDSUB_CAP voltou a ser numero solto — a incoerencia entre tetos e o que produzia os cortes silenciosos"
fi

# O teto de PILHA nunca pode exceder o canonico. limits.h exige a relacao
# com _Static_assert, inclusive no bear; o gate cobra a declaracao real.
if grep -Eq '^[[:space:]]*_Static_assert[[:space:]]*\([[:space:]]*JASH_LINE_CAP_STACK[[:space:]]*<=[[:space:]]*JASH_LINE_CAP[[:space:]]*,' c/core/limits.h; then
    ok "limits.h falha em compilacao se a ordem dos tetos for invertida"
else
    bad "guarda de ordem" "o _Static_assert que protege a relacao sumiu de core/limits.h"
fi

# ── 3. nao existe mais corte MUDO ─────────────────────────────────────────
if grep -qE 'slen = 8192 - 1' c/parser/pipeline.c; then
    bad "sem truncamento mudo no pipeline" \
        "voltou o 'slen = 8192 - 1' em pipeline.c — corte silencioso de novo"
else
    ok "pipeline.c: estouro de segmento recusa, nao corta"
fi

# O estagio de lista tambem precisa RECUSAR, nao truncar-e-executar.
if grep -q 'if (slen >= sizeof(seg)) slen = sizeof(seg) - 1;' c/parser/eval.c; then
    bad "sem truncamento mudo na lista" \
        "eval.c voltou a truncar o segmento e executar a versao cortada"
else
    ok "eval.c: comando longo demais e recusado, nao executado truncado"
fi

# ── 4. o caminho comum nao regrediu ───────────────────────────────────────
out=$("$JASH" -c 'echo alpha | tr a-z A-Z' 2>/dev/null | tail -1)
[ "$out" = "ALPHA" ] && ok "pipeline simples intacto" \
                     || bad "pipeline simples" "veio '$out'"
out=$("$JASH" -c 'printf "a\nb\nc\n" | grep b | wc -l' 2>/dev/null | tr -d ' ')
[ "$out" = "1" ] && ok "pipeline de 3 estagios intacto" \
                 || bad "pipeline de 3 estagios" "veio '$out'"

echo ""
echo "=== JINV-021: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
