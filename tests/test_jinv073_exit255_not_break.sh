#!/usr/bin/env bash
# JINV-073 — 254 e 255 sao STATUS, nao instrucoes de fluxo.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#     O codigo de saida de um comando NUNCA pode ser lido como um pedido de
#     break/continue. A faixa 0..255 pertence inteira ao processo.
#
# ═══ POR QUE ISTO IMPORTA MAIS DO QUE O CORPUS MEDE ═══
#
# O corpus de conformidade tem 2 casos que dependem disto. Dois. Pela metrica
# de alavancagem, seria a ultima coisa da fila — e a metrica estaria MENTINDO,
# porque o que estava em jogo nao era um caso de teste, era o shell do usuario:
#
#     jash -c '(exit 255); echo sobrevivi'      ->  nada. rc 255.
#     jash -c 'sh -c "exit 255"; echo sobrevivi'->  nada. rc 255.
#     jash -c 'f(){ return 255; }; f; echo x'   ->  nada. rc 255.
#
# O resto da linha era ABANDONADO em silencio. E 255 nao e um numero exotico:
# `ssh` devolve 255 quando a conexao falha, `xargs` devolve 255 para mandar
# parar, e qualquer `exit 255` de script e legitimo. Bastava um
# `ssh host cmd; echo depois` para o `echo depois` desaparecer sem aviso.
#
# ═══ A CAUSA E O CONSERTO ═══
#
# `break` devolvia 255 e `continue` devolvia 254 COMO codigo de saida — canal
# de controle e canal de status compartilhando o mesmo espaco. Quem consumia
# olhava so para o numero, e um numero nao sabe de onde veio.
#
# O conserto separa os canais: os builtins marcam `s->loop_ctl` (fora da banda
# de status, em `c/core/state.h`) e os consumidores exigem AS DUAS coisas — o
# rc sentinela E a marca. Um `exit 255` de verdade nunca passa pelo builtin,
# logo nunca marca, logo nunca e confundido.
#
# ═══ O QUE ESTE GATE NAO PODE DEIXAR PASSAR ═══
#
# As duas metades tem que ser afirmadas juntas, senao o conserto pode ser
# "consertado" ate matar o break:
#   (a) status 254/255 de verdade sobrevive e a linha continua;
#   (b) break/continue CONTINUAM funcionando, inclusive com N > 1.
# Um gate que so afirmasse (a) ficaria verde com `break` quebrado.
#
# ORACULO: /bin/bash. Nada aqui e comparado com constante escrita a mao.
# Gate: bash tests/test_jinv073_exit255_not_break.sh

set -u
cd "$(dirname "$0")/.." || exit 2

# `JASH_UNDER_TEST` existe para a PROVA NEGATIVA: apontar este mesmo gate para
# um binario anterior ao conserto e exigir que ele fique VERMELHO. Um gate que
# nunca foi visto vermelho e uma afirmacao sem testemunha (JINV-061).
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

# stdout SO. O stderr fica de fora de proposito: fundir os dois numa captura
# comparada por igualdade faz um aviso de UX decidir o veredito (MERGED-EXACT,
# a doenca que o JINV-061 catalogou).
jout() { (cd "$SB" && "$DL" 20 "$JASH" -c "$1" 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" 2>/dev/null); }
jrc()  { (cd "$SB" && "$DL" 20 "$JASH" -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 20 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

# Compara saida E status contra o oraculo. Os dois: um conserto que acerta o
# texto e erra o `$?` continua sendo um shell que mente para o `set -e` de quem
# o chama.
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

echo "── (a) status 254/255 e DADO: a linha continua ──────────────────────"
oracle "(exit 255) nao derruba o resto da linha"      '(exit 255); echo sobrevivi'
oracle "(exit 254) nao derruba o resto da linha"      '(exit 254); echo sobrevivi'
oracle "255 de processo externo nao e break"          'sh -c "exit 255"; echo sobrevivi'
oracle "254 de processo externo nao e continue"       'sh -c "exit 254"; echo sobrevivi'
oracle "return 255 de funcao nao derruba a linha"     'f() { return 255; }; f; echo sobrevivi rc=$?'
oracle "return 254 de funcao nao derruba a linha"     'f() { return 254; }; f; echo sobrevivi rc=$?'
oracle "\$? preserva o 255"                           '(exit 255); echo $?'
oracle "\$? preserva o 254"                           '(exit 254); echo $?'
oracle "255 no meio de && ||"                         'sh -c "exit 255" || echo caiu-no-ou; echo fim'
oracle "255 dentro de for nao sai do laco"            'for i in 1 2; do sh -c "exit 255"; echo iter$i; done; echo fim'
oracle "254 dentro de for nao pula a volta"           'for i in 1 2; do sh -c "exit 254"; echo iter$i; done; echo fim'
oracle "255 dentro de while nao sai do laco"          'i=0; while [ $i -lt 2 ]; do (exit 255); i=$((i+1)); echo w$i; done; echo fim'
oracle "255 dentro de subshell com cauda"             '(exit 255); (exit 254); echo dois-sobreviveram'

echo
echo "── (b) break/continue CONTINUAM funcionando ─────────────────────────"
oracle "break simples"                                'for i in 1 2 3; do break; echo nunca; done; echo fim'
oracle "continue simples"                             'for i in 1 2 3; do continue; echo nunca; done; echo fim'
oracle "break 2 atravessa dois lacos"                 'for i in 1 2; do for j in a b; do break 2; done; done; echo fim'
oracle "continue 2 atravessa dois lacos"              'for i in 1 2; do for j in a b; do continue 2; done; echo never; done; echo fim'
oracle "break dentro de while"                        'i=0; while [ $i -lt 5 ]; do i=$((i+1)); if [ $i = 2 ]; then break; fi; echo w$i; done; echo fim'
oracle "continue dentro de while"                     'i=0; while [ $i -lt 3 ]; do i=$((i+1)); if [ $i = 2 ]; then continue; fi; echo w$i; done; echo fim'
oracle "break dentro de case dentro de for"           'for i in 1 2 3; do case $i in 2) break;; esac; echo $i; done; echo fim'
oracle "continue dentro de case dentro de for"        'for i in 1 2 3; do case $i in 2) continue;; esac; echo $i; done; echo fim'

echo
echo "── (c) a marca nao vaza entre linhas ────────────────────────────────"
# `break` fora de laco e no-op no POSIX. Se a marca ficasse posta, a proxima
# linha veria um `(exit 255)` como pedido de break — o defeito voltaria pela
# porta dos fundos. Duas linhas de topo, uma alimentando a outra.
oracle "break fora de laco nao envenena a linha seguinte" 'break
(exit 255)
echo ainda-vivo'

printf '\n=== JINV-073: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
