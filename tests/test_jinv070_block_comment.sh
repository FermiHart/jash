#!/usr/bin/env bash
# JINV-070 — um `#` no fim da linha acaba na NOVA-LINHA, nunca no fim do texto.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   Um comentario dentro de um bloco (`if`, `for`, `while`, `case`, funcao)
#   remove EXATAMENTE o resto daquela LINHA. Nunca o `fi`/`done`/`esac` que
#   vem depois.
#
# ═══ POR QUE ISTO IMPORTA MAIS QUE QUALQUER OUTRO CONSERTO DESTA CASA ═══
#
# Medido contra 2 176 scripts de shell REAIS desta maquina (NetBSD, Minix,
# bash-5.2, ncurses, libtool, Homebrew, Apple CommandLineTools, IOCCC —
# nenhum escrito para este teste), o jash percorria 44,5 % deles. O gatilho
# abaixo esta em 208 desses arquivos e UM basta para matar o arquivo inteiro:
#
#     if false; then
#     echo A # comentario
#     fi
#     echo fim
#
#     bash -> fim
#     jash -> jash: syntax error: missing fi     rc 2, ARQUIVO ABORTADO
#
# O efeito mudava com o bloco, e a variante mais perigosa era a silenciosa:
#
#     if … fi     -> aborta o arquivo, `missing fi`, rc 2
#     for … done  -> aborta o arquivo, `missing done`, rc 2
#     case … esac -> rc 0, SEM ERRO NENHUM, e o corpo do ramo nao roda
#     f() { … }   -> a definicao da funcao e destruida
#
# A causa era UMA e nao era "faltou tratar comentario": o tokenizador de topo
# SABIA o que e comentario — por isso `echo 'A#x'` sempre funcionou — mas
# cortava do `#` ate o FIM DA STRING em vez de ate o fim da LINHA. Num bloco
# de varias linhas isso engolia o terminador. Eram dois analisadores sobre o
# mesmo texto e so um estava certo: a mesma familia dos 4 casadores de glob
# divergentes e dos 7 rtrim cegos ja mortos aqui.
#
# O conserto nao escreveu um terceiro analisador: promoveu o do topo a
# publico (`jash_strip_comments_and_joins`, c/parser/quotes.c) e passou a
# usa-lo em UMA porta so — `jash_eval_line`.
#
# ═══ AS DUAS METADES QUE ESTE GATE COBRE ═══
#
#   (a) o comentario SOME — em todo tipo de bloco, em toda profundidade;
#   (b) o que NAO e comentario CONTINUA sendo dado: `#` entre aspas, `\#`
#       escapado, `a#b` no meio de palavra. Sem (b) o conserto seria um corte
#       cego novo, e a cura seria pior.
#
# Oraculo: /bin/bash, em TODA assercao.
# Prova negativa: rodar este arquivo com JASH_UNDER_TEST apontando para um binario
# anterior ao conserto — as assercoes (a) ficam VERMELHAS.
#
# Origem: audits/BENCHMARK-scripts-reais.md secao 4.1
# Codigo:  c/parser/quotes.c  (jash_strip_comments_and_joins)
#          c/parser/eval.c    (a unica porta que a chama)

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

# TODA invocacao passa pelo cao de guarda. NUNCA `timeout`: nao existe nesta
# maquina, devolve 127 sob bash, e faz o teste concluir "nao travou" quando
# travou. `command -v timeout` MENTE (no jash e builtin).
#
# Compara jash contra bash no MESMO arquivo: stdout e rc. O texto do stderr
# nao e comparado (jash nunca prometeu igualdade de diagnostico) — mas a
# AUSENCIA dele e verificada onde a propriedade e "nao inventar erro".
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

# Igual ao anterior, e ainda exige stderr VAZIO quando o do bash e vazio.
# E aqui que a variante silenciosa do `case` (rc 0, corpo mudo) e apanhada
# pela saida, e as variantes ruidosas pelo canal de erro.
cmp_case_quiet() {   # cmp_case_quiet <nome> <corpo>
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

echo "── (a) o comentario acaba na LINHA, nao no arquivo ────────────────────"

cmp_case_quiet "if: comentario no fim da linha nao engole o 'fi'" \
'if false; then
echo A # comentario
fi
echo fim
'

cmp_case_quiet "for: nao engole o 'done'" \
'for i in 1 2; do
echo $i # nota
done
echo fim
'

cmp_case_quiet "while: nao engole o 'done'" \
'n=0
while [ $n -lt 2 ]; do
n=$((n+1)) # incrementa
done
echo $n
'

# A variante que NAO emitia erro nenhum: rc 0, sem mensagem, e o corpo do
# ramo simplesmente nao rodava. E a pior das tres porque um agente que confie
# no rc nao tem como perceber.
cmp_case_quiet "case: o corpo do ramo RODA (a falha era silenciosa, rc 0)" \
'case x in
x)
echo hit # nota
;;
esac
echo fim
'

cmp_case_quiet "funcao: a definicao sobrevive ao comentario" \
'f() {
echo inside # nota
}
f
echo fim
'

cmp_case_quiet "linha INTEIRA de comentario dentro do bloco" \
'if true; then
# so um comentario
echo A
fi
'

# `@(#)` e a string `what` do SCCS, onipresente em fonte BSD e autoconf. O
# `(` era fronteira de palavra, o `#` virava comentario, e o corte ia ate o
# fim do arquivo.
cmp_case_quiet "comentario com '@(#)' (string what do SCCS)" \
'if true; then
# @(#)Copyright (c) 1991 The Regents
echo A
fi
'

cmp_case_quiet "comentario que CONTEM a palavra terminadora" \
'if true; then
echo A # fi means end
fi
echo fim
'

cmp_case_quiet "3 niveis de aninhamento" \
'if true; then
if true; then
if true; then
echo deep # nota
fi
fi
fi
'

cmp_case_quiet "comentario depois de ';' e depois de atribuicao" \
'if true; then
V=1 # atribuicao comentada
echo A; # ponto-e-virgula antes
echo $V #
fi
'

cmp_case_quiet "tabulacao antes do '#'" \
"$(printf 'if true; then\necho A \t# nota\nfi\n')"

echo "── (b) o que NAO e comentario continua sendo DADO ─────────────────────"

cmp_case "aspas simples: 'A#x' e dado" \
"if true; then
echo 'A#x'
fi
"

cmp_case 'aspas duplas: "x # y" e dado' \
'if true; then
echo "x  # y"
fi
'

cmp_case 'escapado: \# e dado' \
'if true; then
echo A\#x
fi
'

cmp_case "no meio da palavra: a#b e dado" \
'if true; then
echo a#b
fi
'

cmp_case 'dentro de $( ): a passada e do cmdsub, nao desta' \
'echo $(echo hi) # z
echo "$(echo hi)" # z
'

cmp_case 'dentro de ${ }: o # e operador, nao comentario' \
'v=abcdef
echo ${v#abc}
echo ${v##a} # nota
'

echo "── (c) comentario e continuacao se cruzam (a razao de serem UMA passada)"

# `# nota \` NAO continua: o comentario acaba na nova-linha, ponto. Se as
# duas regras morassem em passadas diferentes, esta linha juntaria a
# seguinte e `echo b` sumiria dentro do comentario.
cmp_case 'comentario com "\" no fim NAO continua a linha' \
'echo a # note \
echo b
'

echo "── (d) o topo continua funcionando (sem regressao) ────────────────────"

cmp_case "topo: comentario no fim da linha" \
'echo a #note
echo b
'

cmp_case "topo: linha que e so comentario" \
'# nada aqui
echo b
'

cmp_case "topo: espaco escapado antes do # continua sendo dado (JINV-063)" \
'echo a\  #nota
'

printf '\n=== JINV-070: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
