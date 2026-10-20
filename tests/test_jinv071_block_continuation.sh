#!/usr/bin/env bash
# JINV-071 — `\` no fim da linha junta a linha seguinte, TAMBEM dentro de um bloco.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   Uma barra invertida imediatamente antes da nova-linha, fora de aspas
#   simples, remove AS DUAS: as duas linhas fisicas viram uma linha logica.
#   Isto vale no topo E dentro de `if`/`for`/`while`/`case`/funcao.
#
# ═══ POR QUE ISTO IMPORTA ═══
#
# E o gatilho MAIS NUMEROSO dos 2 176 scripts de shell reais medidos nesta
# maquina: 759 arquivos (34,9 %) contem uma continuacao dentro de um bloco.
# Dentro de bloco ela nao era honrada de jeito nenhum:
#
#     if true; then
#     echo A \
#     B
#     fi
#
#     bash -> A B
#     jash -> A \          e depois:  B: command not found     rc 127
#
# Repare no efeito: nao e "saida errada". A segunda metade de um argumento
# vira um COMANDO. E a mesma familia do JINV-016/017/018/019 e do JINV-072 —
# DADO virando COMANDO por uma fronteira lida errado — so que aqui a
# fronteira e a nova-linha.
#
# A causa e a mesma do JINV-070 e o conserto e o mesmo: o topo (c/main.c)
# juntava continuacoes, o rastreador de bloco nao juntava nada. Duas leituras
# do mesmo texto. Agora ha uma so — `jash_strip_comments_and_joins` em
# c/parser/quotes.c, chamada de uma unica porta (`jash_eval_line`).
#
# Comentario e continuacao andam na MESMA passada de proposito, porque as duas
# regras se cruzam: `# nota \` NAO continua (o comentario acaba na nova-linha)
# e `echo a \` + `# x` continua ANTES de existir comentario. Dois analisadores
# separados nao tem como acertar os dois — e a razao de o JINV-070 e este
# gate cobrirem os dois lados desse cruzamento.
#
# ═══ O QUE ESTE GATE NAO MEDE ═══
#
# `echo A \` + `   B` (linha seguinte INDENTADA) ainda diverge quando o `echo`
# usado e o BUILTIN: o bash imprime `A B` (a indentacao vira separador de
# campo e some), o jash imprime `A   B`. Isso NAO e continuacao — e o
# `echo` builtin nao colapsar corridas de branco entre argumentos
# (`echo A     B` diverge igual, sem continuacao nenhuma). E outra raiz, em
# outro arquivo, e nao pertence a este gate. As assercoes indentadas aqui
# usam /bin/echo e contagem de argumentos, que medem a JUNCAO e nada mais.
#
# Oraculo: /bin/bash, em TODA assercao.
# Prova negativa: JASH_UNDER_TEST=<binario anterior ao conserto> -> secao (a) VERMELHA.
#
# Origem: audits/BENCHMARK-scripts-reais.md secao 4.2
# Codigo:  c/parser/quotes.c (jash_strip_comments_and_joins), c/parser/eval.c

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
H=$(mktemp -d) || exit 2
trap 'rm -rf "$T" "$H"' EXIT INT TERM

# Cao de guarda em TODA porta. NUNCA `timeout`: nao existe nesta maquina,
# devolve 127 sob bash, e o teste conclui "nao travou" exatamente quando
# travou. `command -v timeout` mente (no jash e builtin).
cmp_case() {   # cmp_case <nome> <corpo-do-script>
    local name="$1" body="$2"
    printf '%s' "$body" > "$T/c.sh"
    local bo bc jo jc
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash ./c.sh 2>/dev/null </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   ./c.sh 2>/dev/null </dev/null); jc=$?
    if [ "$bo" = "$jo" ] && [ "$bc" = "$jc" ]; then
        ok "$name"
    else
        bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"
    fi
}

# Mesma comparacao, e ainda exige que o jash nao invente diagnostico onde o
# bash nao inventou. E aqui que o `B: command not found` da continuacao
# perdida e apanhado mesmo se a saida por acaso coincidir.
cmp_case_quiet() {
    local name="$1" body="$2"
    printf '%s' "$body" > "$T/c.sh"
    local bo bc jo jc be je
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash ./c.sh 2>"$T/be" </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   ./c.sh 2>"$T/je" </dev/null); jc=$?
    be=$(wc -c < "$T/be" | tr -d ' '); je=$(wc -c < "$T/je" | tr -d ' ')
    if [ "$bo" != "$jo" ] || [ "$bc" != "$jc" ]; then
        bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"
    elif [ "$be" = "0" ] && [ "$je" != "0" ]; then
        bad "$name (stderr)" "bash calou, jash reclamou: $(head -c 120 "$T/je")"
    else
        ok "$name"
    fi
}

echo "── (a) a continuacao e honrada DENTRO do bloco ────────────────────────"

cmp_case_quiet "if: duas linhas viram um comando so" \
'if true; then
echo A \
B
fi
'

cmp_case_quiet "if: linha seguinte INDENTADA (o caso do corpus), via /bin/echo" \
'if true; then
/bin/echo A \
   B
fi
'

cmp_case_quiet "for: continuacao no corpo" \
'for i in 1 2; do
echo $i \
x
done
'

cmp_case_quiet "while: continuacao no corpo" \
'n=0
while [ $n -lt 1 ]; do
n=1 \
; echo $n
done
'

cmp_case_quiet "case: continuacao no corpo do ramo" \
'case x in
x)
echo hit \
again
;;
esac
'

cmp_case_quiet "funcao: continuacao no corpo" \
'f() {
echo in \
side
}
f
'

# A prova que nao depende de como o `echo` trata branco: quantos ARGUMENTOS a
# linha logica tem. Se a continuacao nao for honrada, `$#` muda.
cmp_case_quiet "a juncao produz UMA linha logica (contagem de argumentos)" \
'if true; then
set -- A \
   B \
   C
echo $#
fi
'

cmp_case_quiet "a juncao pode partir ate o NOME do comando" \
'if true; then
ec\
ho junto
fi
'

echo "── (b) o que NAO e continuacao continua intacto ───────────────────────"

# `\\` no fim e uma barra invertida ESCAPADA: a nova-linha NAO some.
cmp_case 'barra escapada (\\) no fim NAO continua' \
'if true; then
echo a\\
echo b
fi
'

cmp_case 'dentro de aspas duplas a continuacao TAMBEM junta (POSIX)' \
'if true; then
echo "A \
B"
fi
'

cmp_case 'barra no MEIO da linha nao e continuacao' \
'if true; then
echo a\b c
fi
'

# `# nota \` nao continua: o comentario acaba na nova-linha. E o cruzamento
# entre esta regra e a do JINV-070.
cmp_case 'dentro do bloco, "\" no fim de um COMENTARIO nao continua' \
'if true; then
echo a # nota \
echo b
fi
'

echo "── (c) o topo continua funcionando (sem regressao) ────────────────────"

cmp_case "topo: continuacao simples" \
'echo A \
B
'

cmp_case "topo: continuacao dentro de atribuicao" \
'V=a\
b
echo $V
'

cmp_case "topo: contagem de argumentos com continuacao indentada" \
'set -- A \
   B
echo $#
'

printf '\n=== JINV-071: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
