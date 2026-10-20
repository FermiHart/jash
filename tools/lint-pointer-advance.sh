#!/usr/bin/env bash
# lint — um laco de varredura TEM que avancar o ponteiro. JINV-062.
#
# A propriedade:
#
#     Nenhuma volta de um `while` sobre `*p` pode terminar onde comecou.
#
# Quando termina, a volta seguinte parte do estado exato da anterior e o shell
# trava — sem mensagem, sem log, sem core, sem nada para depurar. Esta classe
# derrubou o jash DUAS VEZES em um unico dia, em lugares diferentes, escrita
# pela mesma cabeca com oito horas de intervalo:
#
#   CASO 1 · c/parser/if_block.c, tokenize_if_block (JINV-043) — laco externo
#   cujo corpo podia terminar imovel. Conserto: `const char* iter_start = p;
#   ... if (p == iter_start && *p) p++;`
#
#   CASO 2 · c/builtins/builtin_resume.c, rs_collect_paths (JINV-057) — dois
#   lacos vizinhos com conjuntos de separadores ASSIMETRICOS: `(` pulava mas
#   nao parava, `)` parava mas nao pulava. Num `)` o ponteiro ficava imovel.
#   Travou o shell instalado na maquina do dono, ao gravar um bilhete que
#   tinha parenteses.
#
# Duas vezes no mesmo dia significa que a cabeca nao aprende esta forma; so o
# gate aprende. Um bug so morre quando vira catraca.
#
# A analise mora em `tools/lint-pointer-advance.py` — Python porque comparar
# CONJUNTOS DE CARACTERES entre dois lacos exige casar parenteses, decodificar
# `'\t'`/`'\''` e expandir `#define` e funcoes de pular do mesmo arquivo, e um
# lint que erra o conjunto e pior que lint nenhum. Este arquivo e a CATRACA, no
# molde do `lint-no-silent-truncation.sh`: lista de excecoes justificada, teto
# que so desce.
#
# Uso:
#   bash tools/lint-pointer-advance.sh              # varredura + catraca
#   bash tools/lint-pointer-advance.sh --check A.c  # veredito por laco (rc=1 se suspeito)
#   bash tools/lint-pointer-advance.sh --list       # todos os vereditos de c/

set -u
cd "$(dirname "$0")/.." || exit 2

# A lista de excecoes e sobreponivel SO para que o gate consiga provar que a
# catraca recusa uma excecao sem justificativa (JINV062_ALLOW aponta para uma
# copia adulterada). Nenhum caminho de producao passa essa variavel — se ela
# aparecer num Makefile ou num CI, e contorno, nao configuracao.
ALLOW="${JINV062_ALLOW:-tools/lint-pointer-advance.allow}"
PY="tools/lint-pointer-advance.py"
SCOPE="c"

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 ausente"; exit 0; }

# ──── modo --check: um veredito por laco, sem catraca ────
# E o que o gate usa nas fixtures: rc=1 quando o arquivo tem SUSPEITO, rc=0
# quando nao tem. Sem isto o gate so provaria que o lint RODA; com isto ele
# prova que o lint DISCRIMINA.
if [ "${1:-}" = "--check" ]; then
    shift
    [ $# -gt 0 ] || { echo "uso: bash $0 --check ARQUIVO..." >&2; exit 2; }
    out=$(python3 "$PY" --all "$@" 2>&1) || { printf '%s\n' "$out"; exit 2; }
    printf '%s\n' "$out"
    printf '%s\n' "$out" | grep -q '^SUSPEITO' && exit 1
    exit 0
fi
if [ "${1:-}" = "--list" ]; then
    exec python3 "$PY" --all $SCOPE
fi

PARCIAL=0
[ $# -gt 0 ] && { SCOPE="$*"; PARCIAL=1; }   # escopo menor: uso interativo e gate

err=$(mktemp) || exit 2
trap 'rm -f "$err"' EXIT INT TERM

# UMA varredura so: a arvore inteira leva alguns segundos, e o gate chama este
# script mais de uma vez.
detalhe=$(python3 "$PY" --records $SCOPE 2>"$err" | sort -u)
atual=$(printf '%s\n' "$detalhe" | sed -E 's/  ::  .*$//' | sort -u)
n_seguro=$(awk '/^SEGURO /{print $2}' "$err")
n_suspeito=$(awk '/^SUSPEITO /{print $2}' "$err")
[ -n "${n_seguro:-}" ] || { echo "✗ o analisador nao rodou:"; cat "$err"; exit 2; }

[ -f "$ALLOW" ] || : > "$ALLOW"

# A chave guardada e `arquivo: TIPO funcao ponteiro` — SEM numero de linha, que
# muda a cada edicao e transformaria a lista de excecoes num arquivo de conflito.
permitido=$(grep -vE '^\s*#|^\s*$' "$ALLOW" 2>/dev/null | sort -u)

novos=$(comm -23 <(printf '%s\n' "$atual" | sed '/^$/d') <(printf '%s\n' "$permitido"))
mortos=$(comm -13 <(printf '%s\n' "$atual" | sed '/^$/d') <(printf '%s\n' "$permitido"))

rc=0
if [ -n "$novos" ]; then
    echo "✗ laco de varredura SUSPEITO novo (a volta pode terminar imovel):"
    printf '%s\n' "$novos" | while IFS= read -r k; do
        printf '%s\n' "$detalhe" | grep -F "$k  ::  " | sed 's/^/    /'
    done
    echo "  Conserte de um dos dois jeitos — sao independentes, qualquer um basta:"
    echo "    1. simetria: o conjunto que PARA a varredura tem que estar contido"
    echo "       no que ela PULA (ai todo caractere e consumido por um dos dois);"
    echo "    2. guarda:   const char* ini = p; ... if (p == ini && *p) p++;"
    echo "  Se o avanco existir e o lint e que nao o enxerga, acrescente a linha"
    echo "  em $ALLOW precedida de um comentario '# porque: <razao>'."
    rc=1
fi
# Numa varredura PARCIAL toda entrada de fora do escopo pareceria obsoleta —
# a conferencia so faz sentido sobre a arvore inteira.
if [ -n "$mortos" ] && [ "$PARCIAL" = "0" ]; then
    echo "✗ entradas obsoletas em $ALLOW (o laco sumiu ou foi consertado — remova-as):"
    printf '%s\n' "$mortos" | sed 's/^/    /'
    rc=1
fi

# ──── catraca: a secao DÍVIDA so encolhe ────
# REVISADO e permanente: o avanco existe e esta provado a mao, o lint e que nao
# alcanca (ponteiro que avanca por `ep = ee`, funcao de pular de outro arquivo).
# DÍVIDA e o que ainda pode travar. Se este numero sobe, alguem escreveu mais um
# laco desta familia e o DECLAROU em vez de consertar.
DEBT_CEILING=1   # 2 → 1: `split_statements` quitada pelo JINV-064 (2026-07-27)
n_debt=$(awk '/pode travar/{f=1;next} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
if [ "$n_debt" -gt "$DEBT_CEILING" ]; then
    echo "✗ a divida de avanco SUBIU: $n_debt (teto $DEBT_CEILING)"
    echo "  Esta secao so encolhe. Conserte em vez de declarar."
    rc=1
elif [ "$n_debt" -lt "$DEBT_CEILING" ]; then
    echo "✗ a divida caiu para $n_debt — abaixe DEBT_CEILING para $n_debt neste mesmo commit"
    rc=1
fi

# ──── toda excecao precisa de justificativa escrita ────
sem_porque=$(awk '/^[[:space:]]*#/ { if ($0 ~ /porque:/) seen=1; next } /^[[:space:]]*$/ { seen=0; next } { if (!seen) print "    " $0; seen=0 }' "$ALLOW")
if [ -n "$sem_porque" ]; then
    echo "✗ excecao sem '# porque:' na linha anterior (divida escondida):"
    printf '%s\n' "$sem_porque"
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "✓ nenhum laco de varredura suspeito novo (seguros: $n_seguro · declarados: $n_suspeito · divida: $n_debt)"
fi
exit $rc
