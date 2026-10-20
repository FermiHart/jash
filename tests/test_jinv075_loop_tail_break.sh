#!/usr/bin/env bash
# JINV-075 — a cauda depois de `done`/`esac` respeita break/continue.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#     Enquanto um break/continue esta subindo, NADA depois do fechador do
#     bloco pode executar — e nada pode substituir o pedido pelo proprio
#     status.
#
# ═══ O DEFEITO, E O QUE ELE TEM DE INSTRUTIVO ═══
#
#     jash -c 'for i in 1 2; do for j in a b; do break 2; done; echo never; done; echo done'
#     never
#     never
#     done          (bash imprime so `done`)
#
# Duas coisas quebram de uma vez. O `echo never`, que vem DEPOIS do `done` do
# laco interno, executa apesar do `break 2`. E — pior — o rc 0 dele substitui a
# sentinela no `return rest_rc`: o pedido de sair do laco externo e APAGADO, e
# o externo segue rodando como se nada tivesse sido pedido. Sumir com a
# instrucao e mais grave do que imprimir a linha errada.
#
# O que este defeito ensina nao e sobre lacos. `c/parser/if_block.c:501` JA
# TINHA a guarda certa. Faltava nos dois irmaos (`c/parser/for_loop.c`, e o
# `case` em `c/parser/eval.c`) — a mesma disciplina aplicada num lugar e
# esquecida nos outros dois, sem nada que cobrasse a diferenca. E o padrao
# desta casa: nao "ninguem sabia", e sim "um sabia e nao virou regra".
# Um bug so morre quando vira catraca.
#
# ═══ INTERACAO COM O JINV-073 ═══
#
# A guarda pergunta "ha um pedido de fluxo em voo?" — e desde o JINV-073 essa
# pergunta e feita ao CANAL DE CONTROLE (`jash_loopctl_pending`), nunca ao
# codigo de saida. Se fosse feita ao rc, um comando que sai com 255 no fim do
# laco faria a cauda desaparecer sem que ninguem tivesse pedido nada. Por isso
# a secao (c) existe: ela prova que a guarda nao virou um filtro de numeros.
#
# ORACULO: /bin/bash. Gate: bash tests/test_jinv075_loop_tail_break.sh

set -u
cd "$(dirname "$0")/.." || exit 2

# PROVA NEGATIVA: aponte para um binario anterior ao conserto e este gate tem
# que ficar VERMELHO (JINV-061).
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "FAIL: $DL ausente (cao de guarda obrigatorio)"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

jout() { (cd "$SB" && "$DL" 20 "$JASH" -c "$1" 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jrc()  { (cd "$SB" && "$DL" 20 "$JASH" -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

oracle() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jout "$code"); jr=$(jrc "$code")
    bo=$(bout "$code"); br=$(brc "$code")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 20s): $code"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"
    fi
}

echo "── (a) cauda depois de \`done\` (for) ────────────────────────────────"
oracle "break 2: a cauda do laco interno nao roda" \
       'for i in 1 2; do for j in a b; do break 2; done; echo never; done; echo done'
oracle "continue 2: a cauda do laco interno nao roda" \
       'for i in 1 2; do for j in a b; do continue 2; done; echo never; done; echo done'
oracle "break 3 atravessa tres lacos" \
       'for a in 1 2; do for b in 1 2; do for c in 1 2; do break 3; done; echo n1; done; echo n2; done; echo fim'
oracle "break 1 NAO derruba o laco de fora" \
       'for i in 1 2; do for j in a b; do break; done; echo depois$i; done; echo fim'

echo
echo "── (b) cauda depois de \`esac\` (case) ───────────────────────────────"
oracle "continue dentro de case pula a cauda" \
       'for i in 1 2 3; do case $i in 2) continue;; esac; echo $i; done'
oracle "break dentro de case pula a cauda" \
       'for i in 1 2 3; do case $i in 2) break;; esac; echo $i; done; echo fim'
oracle "case sem break: a cauda roda normalmente" \
       'for i in 1 2; do case $i in 1) echo um;; esac; echo pos$i; done'

echo
echo "── (c) a guarda pergunta ao CANAL, nao ao numero (JINV-073) ─────────"
# Se a guarda fosse `rc == 255`, um comando que sai com 255 no fim do corpo
# faria a cauda depois do `done` sumir sem ninguem ter pedido nada.
oracle "255 no corpo nao apaga a cauda depois do done" \
       'for i in 1; do for j in a; do sh -c "exit 255"; done; echo cauda-viva; done; echo fim'
oracle "254 no corpo nao apaga a cauda depois do esac" \
       'for i in 1; do case $i in 1) sh -c "exit 254";; esac; echo cauda-viva; done; echo fim'

echo
echo "── (d) o pedido chega inteiro: contagem de voltas do laco EXTERNO ───"
# A metade que o texto do `echo never` nao mostra: o rc da cauda APAGAVA a
# sentinela e o laco externo continuava. Contar as voltas mede isso direto.
oracle "break 2 encerra o laco externo na PRIMEIRA volta" \
       'n=0; for i in 1 2 3; do n=$((n+1)); for j in a; do break 2; done; echo never; done; echo voltas=$n'
oracle "continue 2 nao transforma o externo em laco infinito" \
       'n=0; for i in 1 2 3; do n=$((n+1)); for j in a; do continue 2; done; echo never; done; echo voltas=$n'

echo
echo "── (e) while tambem (o irmao que ninguem citou) ─────────────────────"
oracle "break 2 de dentro de while + cauda depois do done" \
       'i=0; while [ $i -lt 3 ]; do i=$((i+1)); for j in a; do break 2; done; echo never; done; echo fim'
oracle "while sem break: cauda depois do done roda" \
       'i=0; while [ $i -lt 2 ]; do i=$((i+1)); for j in a; do break; done; echo depois$i; done; echo fim'

printf '\n=== JINV-075: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
