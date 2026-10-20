#!/usr/bin/env bash
# JINV-074 — o texto depois do `fi` roda UMA vez, e quem o roda e o chamador.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#     Cada comando escrito uma vez executa uma vez. Um bloco `if` nao pode ter
#     DOIS donos para a mesma cauda.
#
# ═══ O DEFEITO ═══
#
#     jash -c 'if false; then echo yes; fi; echo after'   ->  after
#                                                             after
#
# Havia dois donos rodando o mesmo texto: o tratador de bloco executava por
# conta propria o que vinha depois do `fi` (`c/parser/script.c`, no fim de
# `jash_eval_if_statement`; `c/parser/if_block.c`, no fim de
# `jash_eval_if_block`), e o chamador (`c/parser/eval.c`) executava DE NOVO via
# `jash_block_tail` (JINV-036).
#
# ═══ POR QUE O SINTOMA ENGANOU ═══
#
# "Imprime duas vezes" e so a metade visivel. O incremento de um laco TAMBEM e
# cauda:
#
#     i=0; while [ $i -lt 2 ]; do if [ $i = 0 ]; then echo z; else echo n; fi;
#                                 i=$((i+1)); done
#
# O `i=$((i+1))` rodava 2x por volta, `i` pulava 0->2, e o laco saia na
# PRIMEIRA volta imprimindo so `z`. Foi diagnosticado por muito tempo como
# LACO INFINITO. E o oposto: laco que termina cedo demais. Um gate que so
# olhasse "imprimiu duas vezes?" nao pegaria esta metade — por isso as duas
# formas estao aqui.
#
# ═══ O CONSERTO (e por que ele nao depende de acordo entre arquivos) ═══
#
# O precedente correto ja existia ao lado: `case`. O `jash_eval_case_stmt` NAO
# roda a cauda; so o chamador roda. Um dono.
#
# Em vez de pedir aos tratadores que parem de rodar a cauda — o que exigiria
# que dois arquivos concordassem para sempre —, o chamador passou a entregar a
# eles uma string que TERMINA no `fi` (`jash_block_split`, eval.c). Nao ha
# cauda para eles rodarem. A propriedade vale por CONSTRUCAO, e nao por acordo:
# mesmo que o codigo interno deles mude, eles continuam recebendo um bloco sem
# cauda.
#
# ORACULO: /bin/bash. Gate: bash tests/test_jinv074_if_tail_once.sh

set -u
cd "$(dirname "$0")/.." || exit 2

# PROVA NEGATIVA: aponte para um binario anterior ao conserto e este gate tem
# que ficar VERMELHO (JINV-061 — gate que nao pode falhar e gate ausente).
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

echo "── (a) a cauda depois do fi executa UMA vez ─────────────────────────"
oracle "ramo nao tomado + cauda"            'if false; then echo yes; fi; echo after'
oracle "ramo tomado + cauda"                'if true; then echo yes; fi; echo after'
oracle "else + cauda"                       'if false; then echo a; else echo b; fi; echo after'
oracle "cauda com DOIS comandos"            'if true; then echo a; fi; echo b; echo c'
oracle "if aninhado + cauda"                'if true; then if true; then echo in; fi; fi; echo after'
oracle "if dentro de funcao + cauda"        'f() { if true; then echo in; fi; echo tailfn; }; f'
oracle "recursao depende da cauda"          'f() { if [ $1 -le 0 ]; then echo base; return 0; fi; echo n$1; f $(($1-1)); }; f 2'
oracle "cauda separada por &&"              'if true; then echo Y; fi && echo z'
oracle "elif + cauda"                       'if false; then echo a; elif true; then echo b; fi; echo after'

echo
echo "── (b) o \$? depois do fi e o do BLOCO, nao o da cauda interna ───────"
# Estes quatro sao o rastro fino do mesmo defeito: a cauda interna rodava ANTES
# de o status do `if` ser escrito, entao `echo $?` via o status da CONDICAO.
oracle "if true; then :; fi -> 0"           'if true; then :; fi; echo $?'
oracle "if false; then :; fi -> 0"          'if false; then :; fi; echo $?'
oracle "if true; then false; fi -> 1"       'if true; then false; fi; echo $?'
oracle "if false; then :; else false; fi"   'if false; then :; else false; fi; echo $?'

echo
echo "── (c) a METADE INVISIVEL: o laco que terminava cedo demais ─────────"
# O incremento tambem e cauda. Rodando 2x por volta, `i` pula 0->2 e o `while`
# sai na PRIMEIRA volta. O sintoma parecia laco infinito; era o contrario.
oracle "while com if dentro roda as DUAS voltas" \
       'i=0; while [ $i -lt 2 ]; do if [ $i = 0 ]; then echo z; else echo n; fi; i=$((i+1)); done'
oracle "while com if dentro conta ate 3" \
       'i=0; while [ $i -lt 3 ]; do if true; then echo v$i; fi; i=$((i+1)); done; echo fim'
oracle "for com if dentro nao pula elemento" \
       'for x in a b c; do if true; then echo -n ""; fi; echo $x; done'

echo
echo "── (d) efeito colateral contado, nao so texto ───────────────────────"
# Texto duplicado da para ver; efeito colateral duplicado, nao. Um contador em
# arquivo mede o numero de EXECUCOES, que e a propriedade de verdade.
#
# A linha TEM que comecar pelo `if`: e so nesse caso que o bloco e a cauda
# chegam juntos ao mesmo `jash_eval_line`. Com qualquer coisa antes, o divisor
# de listas separa os segmentos e o defeito nao aparece — a primeira versao
# deste teste comecava com `rm -f n.txt;` e por isso ficou VERDE contra o
# binario bugado. Um teste que passa sem o bug morrer e a doenca, nao a cura.
jout 'rm -f n.txt' >/dev/null
bout 'rm -f m.txt' >/dev/null
jout 'if false; then echo yes; fi; echo x >> n.txt' >/dev/null
bout 'if false; then echo yes; fi; echo x >> m.txt' >/dev/null
c1=$(jout '/usr/bin/wc -l < n.txt')
c2=$(bout '/usr/bin/wc -l < m.txt')
if [ "$c1" = "$c2" ]; then
    ok "a cauda gravou o MESMO numero de linhas que no bash"
else
    bad "contagem de execucoes da cauda" "jash=[$c1] bash=[$c2]"
fi

printf '\n=== JINV-074: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
