#!/usr/bin/env bash
# JINV-076 — `printf` nao tem teto de argumentos, e diagnostico nao vai no dado.
#
# ═══ AS DUAS PROPRIEDADES PROTEGIDAS ═══
#
#   1. Nenhum teto arbitrario de CONTAGEM de argumentos. `printf` processa o
#      que o shell entregou — todos.
#   2. Se algum dia um diagnostico precisar existir, ele sai pelo STDERR e com
#      rc != 0. NUNCA pelo stdout do comando.
#
# ═══ O DEFEITO ═══
#
#     jash -c "printf 'a%.0s' \$(seq 1 1023) | wc -c"   ->  1023   (certo)
#     jash -c "printf 'a%.0s' \$(seq 1 1024) | wc -c"   ->    43   (?!)
#
# O `43` nao e uma saida truncada. E o COMPRIMENTO DA MENSAGEM DE ERRO
# "jash: printf: argumentos demais (max 1024)\n", que tinha 43 bytes e era
# escrita pelo writer do COMANDO — ou seja, no stdout, dentro do cano, com o
# formato de dado do usuario. Quem estava do outro lado recebia texto de erro
# como se fosse resultado. Cliff bissetado em 1024 exato.
#
# ═══ POR QUE ISTO E PIOR DO QUE PARECE ═══
#
# O conserto anterior (JINV-011) diagnosticou certo — "printf nao pode truncar
# nem descartar em silencio" — e parou no meio: trocou o descarte mudo por uma
# recusa, e mandou a recusa pelo canal errado. Trocou corrupcao silenciosa por
# corrupcao silenciosa com outro texto. O erro no stdout e da mesma familia do
# JINV-038: diagnostico misturado com dado e indistinguivel de dado.
#
# O teto tambem nunca teve razao de ser: `argv_buf` so era LIDO. O conserto e o
# que o proprio comentario do JINV-011 ja prescrevia e nao foi feito — apontar
# direto para `in_argv`, sem copia, sem array na pilha, sem teto, e portanto
# sem mensagem para colocar no lugar errado.
#
# ORACULO: /bin/bash. Gate: bash tests/test_jinv076_printf_no_arg_ceiling.sh

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

jout() { (cd "$SB" && "$DL" 30 "$JASH" -c "$1" 2>/dev/null); }
bout() { (cd "$SB" && "$DL" 30 /bin/bash -c "$1" 2>/dev/null); }
jerr() { (cd "$SB" && "$DL" 30 "$JASH" -c "$1" 2>&1 >/dev/null); }
jrc()  { (cd "$SB" && "$DL" 30 "$JASH" -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
brc()  { (cd "$SB" && "$DL" 30 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

oracle() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jout "$code"); jr=$(jrc "$code")
    bo=$(bout "$code"); br=$(brc "$code")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 30s): $code"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"
    fi
}

echo "── (a) o cliff de 1024: os dois lados dele ──────────────────────────"
oracle "1023 argumentos"  "printf 'a%.0s' \$(seq 1 1023) | /usr/bin/wc -c"
oracle "1024 argumentos"  "printf 'a%.0s' \$(seq 1 1024) | /usr/bin/wc -c"
oracle "1025 argumentos"  "printf 'a%.0s' \$(seq 1 1025) | /usr/bin/wc -c"

echo
echo "── (b) bem acima do teto antigo ─────────────────────────────────────"
oracle "2000 argumentos"  "printf 'a%.0s' \$(seq 1 2000) | /usr/bin/wc -c"
oracle "5000 argumentos via \$( )" "v=\$(printf 'a%.0s' \$(seq 1 5000)); echo \${#v}"
oracle "2000 argumentos para arquivo" \
       "printf 'a%.0s' \$(seq 1 2000) > big.txt; /usr/bin/wc -c < big.txt"
oracle "3000 linhas com %s" "printf '%s\n' \$(seq 1 3000) | /usr/bin/wc -l"

echo
echo "── (c) o dado nao carrega diagnostico ───────────────────────────────"
# A prova direta do defeito: 43 bytes de mensagem de erro saindo pelo stdout.
# Aqui a saida tem que ser SO 'a' repetido — nenhum byte de texto de erro.
out=$(jout "printf 'a%.0s' \$(seq 1 1500)")
if printf '%s' "$out" | grep -q 'printf'; then
    bad "stdout limpo de diagnostico" "a saida contem texto de erro: [$(printf '%s' "$out" | head -c 80)]"
else
    ok "stdout limpo de diagnostico (nenhum 'printf:' no dado)"
fi

n=$(jout "printf 'a%.0s' \$(seq 1 1500) | /usr/bin/wc -c" | tr -d ' ')
if [ "$n" = "1500" ]; then
    ok "1500 argumentos produzem 1500 bytes (nao 43)"
else
    bad "contagem de bytes" "esperado 1500, veio [$n] — 43 e o tamanho da mensagem de recusa"
fi

echo
echo "── (d) o comportamento normal nao regrediu ──────────────────────────"
oracle "printf basico"        "printf '%s\n' a b c | /usr/bin/wc -l"
oracle "printf com %d"        "printf '%d-%s\n' 42 world"
oracle "printf -v em variavel" "printf -v V '%s' hello; echo \$V"
oracle "printf -- termina opcoes" "printf -- '-n\n'"
oracle "printf sem argumento extra" "printf '%s|%s\n' um"
oracle "argumento longo nao e cortado" \
       "v=\$(printf 'b%.0s' \$(seq 1 400)); printf '%s' \"\$v\" | /usr/bin/wc -c"

printf '\n=== JINV-076: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
