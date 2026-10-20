#!/usr/bin/env bash
# JINV-065 — uma corrida de `*` custa o mesmo que um `*` só.
#
# ═══ A propriedade ═══
#
# `*****` **É** `*`. Não "quase", não "na prática": é o mesmo padrão, e um
# casador correto responde nos dois em tempo idêntico. Quem avança **um**
# asterisco por vez dá a cada `*` da corrida uma varredura própria de todo o
# resto do texto, e as varreduras se multiplicam: C(n,k) chamadas. Doze
# asteriscos contra quarenta caracteres são 5,6e9 chamadas — o shell não
# responde nunca. Cada asterisco a mais multiplicava o tempo por ~4,4.
#
# ═══ Por que isto importa (o gatilho aparece em código normal) ═══
#
# `************b` parece patológico e ninguém escreve. **A forma globstar
# parece inofensiva e todo agente escreve**: dois asteriscos, barra, repetido,
# `.c` no fim. É a MESMA corrida — dois asteriscos já bastam — e um caminho de
# 118 caracteres, nada que se olhe duas vezes, pendurava o shell.
#
# E o sujeito do casamento quase sempre vem de FORA (nome de arquivo, linha de
# saída de outro comando, argumento de agente) enquanto o padrão vem do script:
# **quem controla o dado controla o tempo de resposta**. É negação de serviço
# escrita por acidente.
#
# ═══ Por que o gate nasce agora ═══
#
# O jash tem QUATRO casadores de glob para uma semântica só, cada um reescrito
# no arquivo que precisava dele. O idioma que conserta — `while (*pattern ==
# '*') pattern++;` — já estava escrito no repositório, pela mão certa, no ramo
# quente de `c/parser/conditional.c` (por isso `[[ ]]` era imune). Nas outras
# duas portas ele estava escrito só no RABO da função, onde só roda com o texto
# já esgotado: presente, legível, e protegendo nada. Duas portas de quatro
# ficaram de fora — `case` (`c/parser/case_stmt.c`) e `${var#pat}` e irmãos
# (`c/parser/param_expand.c`) — e travavam.
#
# A lição que este gate fixa não é "o conserto foi aplicado". É: **um conserto
# que não vira gate não chega às outras cópias**. Diagnóstico completo, as
# quatro portas e a linha exata: audits/ACHADO-fuzz-glob-estrelas-consecutivas.md
#
# ═══ O que este gate NÃO acusa (refutado contra o oráculo) ═══
#
# `*a*a*a*a…b` contra um texto longo trava no jash **e no /bin/bash**. É
# backtracking inerente à classe, não defeito do jash: sem autômato não existe
# implementação linear. Não refile. A acusação se sustenta só sobre a corrida
# de asteriscos CONSECUTIVOS, onde `*****` é semanticamente idêntico a `*` e
# colapsar é uma linha.
#
# Para essa classe vizinha existe a segunda linha de defesa: um ORÇAMENTO de
# passos (mesmo teto de `c/core/glob.c`, a única porta que já tinha um). A
# asserção 7 cobra que o estouro **grite**: `c/core/glob.c` devolve "não casou"
# em silêncio, e responder rápido e ERRADO sem avisar é a família de defeito que
# este projeto inteiro persegue — travar pelo menos avisa.
#
# ═══ O cão de guarda ═══
#
# `tools/deadline.sh`, NUNCA `timeout`. `timeout(1)` não existe nesta máquina:
# sob bash devolve 127, o teste vê rc≠124 e conclui "não travou" — um watchdog
# que nunca acordou. A asserção 0 prova que o cão está vivo ANTES de confiar
# nele; sem ela, este arquivo inteiro poderia passar verde com o bug de pé.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="$ROOT/build/jash"
DEADLINE="$ROOT/tools/deadline.sh"
BASH_ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

now_ms() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time;print(int(time.time()*1000))'
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}

# ── emissores de sonda ─────────────────────────────────────────────────
# O padrão vai INLINE no texto do comando, nunca via variável: um padrão vindo
# de variável tem regras de expansão próprias e diluiria o que se mede.
emit_case()  { # tag pattern text
    printf "case '%s' in %s) printf '%s M\\\\n';; *) printf '%s N\\\\n';; esac\n" \
           "$3" "$2" "$1" "$1"
}
emit_strip() { # tag op pattern text
    printf "v='%s'; printf '%s [%%%%s]\\\\n' \"\${v%s%s}\"\n" "$4" "$1" "$2" "$3"
}

# Roda um ARQUIVO de sondas, em pedaços — um processo por pedaço em vez de um
# por sonda (a diferença entre 15 s e 40 min de gate). O teto de 400 linhas é
# folgado: acima de ~1960 linhas o leitor de script do jash corrompe a entrada
# (defeito pré-existente, alheio a este invariante).
run_probes() { # shell probefile outfile -> rc 124 se algum pedaço travou
    local sh="$1" src="$2" out="$3" c rc
    : > "$out"
    rm -f "$W"/chunk.*
    ( cd "$W" && split -l 400 "$src" "$W/chunk." )
    for c in "$W"/chunk.*; do
        (cd "$W" && "$DEADLINE" 60 "$sh" "$c" 2>/dev/null) >> "$out"
        rc=$?
        [ "$rc" = "124" ] && return 124
    done
    return 0
}

TEXTS="a b ab abc abcd aab xay xaby a/b a/b/c / // dir/file.c file.c x.c aaaa xxxxxxxxxxQ9 xxxxxxxxxxxxxxxxxxxx a1b 9"
OPS='# ## % %%'

# ── 0. ANTI-INÉRCIA: o cão de guarda está vivo? ────────────────────────
# Sem isto, todas as asserções de travamento abaixo podem passar por 127.
"$DEADLINE" 2 sleep 30 >/dev/null 2>&1
rc=$?
if [ "$rc" = "124" ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "watchdog inerte" \
        "deadline.sh 2 sleep 30 devolveu rc=$rc; esperado 124 — TODAS as asserções de travamento abaixo estão sem valor"
fi

# ── 1. A corrida de `*` no `case` não trava ────────────────────────────
# Repro exato que o fuzzer JINV-060 imprime. Antes do conserto: >25 s medidos.
T41='xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
hung=""
for stars in '**' '*****' '**********' '********************' \
             '****************************************' ; do
    (cd "$W" && "$DEADLINE" 10 "$JASH" -c "case '$T41' in ${stars}Q9) :;; esac" >/dev/null 2>&1)
    [ $? = 124 ] && { hung="$stars"; break; }
done
if [ -z "$hung" ]; then
    ok "case: corrida de até 40 asteriscos não trava"
else
    bad "case trava na corrida de asteriscos" \
        "travou com ${#hung} asteriscos: case '$T41' in ${hung}Q9) :;; esac"
fi

# ── 2. As QUATRO formas de expansão de parâmetro também não travam ─────
# O strip chama o casador UMA VEZ POR COMPRIMENTO de prefixo/sufixo, então
# aqui o custo entra multiplicado por N: era O(N) × exponencial.
hung=""
for op in $OPS; do
    (cd "$W" && "$DEADLINE" 10 "$JASH" -c "v='$T41'; printf '%s' \"\${v${op}**********Q9}\"" >/dev/null 2>&1)
    [ $? = 124 ] && { hung="$op"; break; }
done
if [ -z "$hung" ]; then
    ok "\${v#p} \${v##p} \${v%p} \${v%%p}: corrida não trava em nenhuma"
else
    bad "expansão de parâmetro trava" \
        "\${v${hung}**********Q9} não terminou em 10s"
fi

# ── 3. A FORMA REAL: globstar com caminho fundo ────────────────────────
# Isto é o que quebra na vida real: o padrão é a forma que agente escreve todo
# dia e o caminho tem 118 caracteres, que é um caminho comum.
deep=""; i=0
while [ $i -lt 20 ]; do deep="${deep}diretorio$i/"; i=$((i+1)); done
deep="${deep}arquivo.txt"
(cd "$W" && "$DEADLINE" 15 "$JASH" -c "case '$deep' in **/**/**/*.c) :;; esac" >/dev/null 2>&1)
if [ $? != 124 ]; then
    ok "globstar (${#deep} chars de caminho) não trava — a forma que aparece em código normal"
else
    bad "globstar trava" \
        "case com padrão globstar contra caminho de ${#deep} caracteres não terminou em 15s"
fi

# ── 4. A CURVA É PLANA — não só "coube no prazo" ───────────────────────
# O defeito multiplicava o tempo por ~4,4 A CADA asterisco. Se o colapso
# estiver certo, 4 e 28 asteriscos custam o mesmo (só subir o processo).
# Passar por pouco no prazo não é passar.
a=$(now_ms)
(cd "$W" && "$DEADLINE" 20 "$JASH" -c "case '$T41' in ****Q9) :;; esac" >/dev/null 2>&1)
rc4=$?; b=$(now_ms); t4=$((b-a))
a=$(now_ms)
(cd "$W" && "$DEADLINE" 20 "$JASH" -c "case '$T41' in ****************************Q9) :;; esac" >/dev/null 2>&1)
rc28=$?; b=$(now_ms); t28=$((b-a))
if [ "$rc4" = "124" ] || [ "$rc28" = "124" ]; then
    bad "curva de custo" "uma das medições travou (rc4=$rc4 rc28=$rc28)"
elif [ $((t28 - t4)) -lt 2000 ]; then
    ok "curva plana: 4 asteriscos ${t4}ms vs 28 asteriscos ${t28}ms"
else
    bad "custo cresce com o número de asteriscos" \
        "4 asteriscos: ${t4}ms; 28 asteriscos: ${t28}ms — a corrida não está sendo colapsada"
fi

# ── 5. COLAPSO NEUTRO: a corrida responde igual ao asterisco único ─────
# Propriedade pura, sem oráculo externo: para todo padrão P com corrida e o seu
# colapso P', o jash tem que dar a MESMA resposta — em `case` e nas quatro
# expansões, com o asterisco no começo, no meio e no fim, combinado com `?` e
# `[...]`, colado em texto, e com padrão só de estrelas. Um colapso rápido e
# ERRADO seria pior que travar.
PARES='**|*
*****|*
**********|*
**a|*a
*****a|*a
a**|a*
a*****|a*
a**b|a*b
a*****b|a*b
**a**|*a*
**ab|*ab
ab**|ab*
**?|*?
?**|?*
?**?|?*?
**?a|*?a
a?**|a?*
**[abc]|*[abc]
[abc]**|[abc]*
**[a-c]**|*[a-c]*
[!a]**|[!a]*
**.c|*.c
**x**|*x*
x**y|x*y
**a**b**|*a*b*
**********Q9|*Q9'
n5=0
: > "$W/p5.probe"
{
    id=0
    printf '%s\n' "$PARES" | while IFS='|' read -r praw pcol; do
        [ -n "$praw" ] || continue
        for t in "" $TEXTS; do
            emit_case "A$id" "$praw" "$t"; emit_case "B$id" "$pcol" "$t"
            id=$((id+1))
            for op in $OPS; do
                emit_strip "A$id" "$op" "$praw" "$t"
                emit_strip "B$id" "$op" "$pcol" "$t"
                id=$((id+1))
            done
        done
    done
} > "$W/p5.probe"
n5=$(( $(wc -l < "$W/p5.probe") / 2 ))
if run_probes "$JASH" "$W/p5.probe" "$W/p5.out"; then
    mis=$(awk '{k=$1; sub(/^[AB]/,"",k); v=substr($0,index($0," ")+1);
                if (substr($1,1,1)=="A") a[k]=v;
                else if (k in a && a[k]!=v) { print k" A="a[k]" B="v; exit }}' "$W/p5.out")
    got=$(wc -l < "$W/p5.out" | tr -d ' ')
    if [ -n "$mis" ]; then
        bad "colapso MUDOU o resultado" \
            "corrida e asterisco único divergem — sonda $mis (A=corrida, B=colapsada)"
    elif [ "$got" != "$(wc -l < "$W/p5.probe" | tr -d ' ')" ]; then
        bad "sondas perdidas" "emitidas $(wc -l < "$W/p5.probe") linhas, respondidas $got"
    else
        ok "colapso é semanticamente neutro ($n5 pares de sondas: corrida ≡ asterisco único)"
    fi
else
    bad "travou na bateria de colapso" "algum pedaço da bateria não terminou em 60s"
fi

# ── 6. ORÁCULO: bate com o /bin/bash ───────────────────────────────────
# A propriedade interna da asserção 5 não basta: as duas respostas podem estar
# consistentemente erradas. O bash decide.
if [ -x "$BASH_ORACLE" ]; then
    : > "$W/p6.probe"
    {
        id=0
        for praw in '*' '**' '*****' '**********' '*a' '**a' '*****a' 'a*' 'a**' \
                    'a*b' 'a**b' 'a*****b' '*a*' '**a**' '*ab' '**ab' 'ab*' 'ab**' \
                    '?' '?*' '*?' '**?' '?**' '?*?' '?**?' '*?a' '**?a' 'a?*' 'a?**' \
                    '*[abc]' '**[abc]' '[abc]*' '[abc]**' '*[a-c]*' '**[a-c]**' \
                    '[!a]*' '[!a]**' '*.c' '**.c' '*/*' '**/*' '*/**' '**/**' \
                    '*x*' '**x**' 'x**y' '*Q9' '**********Q9' 'a' 'abc' \
                    '*a*b*' '**a**b**' ; do
            for t in "" $TEXTS; do
                emit_case "c$id" "$praw" "$t"; id=$((id+1))
                for op in $OPS; do
                    emit_strip "c$id" "$op" "$praw" "$t"; id=$((id+1))
                done
            done
        done
    } > "$W/p6.probe"
    n6=$(wc -l < "$W/p6.probe" | tr -d ' ')
    rj=0; rb=0
    run_probes "$JASH" "$W/p6.probe" "$W/p6.jash" || rj=$?
    run_probes "$BASH_ORACLE" "$W/p6.probe" "$W/p6.bash" || rb=$?
    if [ "$rj" = "124" ]; then
        bad "travou sob o oráculo" "a bateria de paridade não terminou no jash"
    elif [ "$rb" = "124" ]; then
        ok "oráculo indisponível (o próprio bash travou na bateria)"
    else
        d=$(diff "$W/p6.jash" "$W/p6.bash" | head -6)
        if [ -z "$d" ]; then
            ok "oráculo: $n6 sondas idênticas ao /bin/bash (case + as 4 expansões)"
        else
            bad "diverge do /bin/bash" "$(printf '%s' "$d" | tr '\n' '|')"
        fi
    fi

    # asterisco ESCAPADO não faz parte da corrida — continua literal (BUG-134)
    div=""
    for pr in 'a\*=a*b' 'a\*=axb' '*\**=a*xb' '\**=*abc'; do
        pt=${pr%%=*}; tx=${pr##*=}
        cmd="v='$tx'; printf '[%s]\n' \"\${v#$pt}\""
        j=$( (cd "$W" && "$DEADLINE" 10 "$JASH" -c "$cmd" 2>/dev/null) )
        b=$( (cd "$W" && "$DEADLINE" 10 "$BASH_ORACLE" -c "$cmd" 2>/dev/null) )
        [ "$j" = "$b" ] || { div="\${v#$pt} txt='$tx': jash='$j' bash='$b'"; break; }
    done
    [ -z "$div" ] && ok "asterisco escapado (\\*) continua literal, não entra na corrida" \
                  || bad "o colapso comeu um escape" "$div"
else
    ok "sem /bin/bash — oráculo não verificado"
fi

# ── 7. ORÇAMENTO: a segunda linha de defesa FALHA ALTO ─────────────────
# O colapso mata `*****`. Ele NÃO torna o backtracking linear em geral — e a
# próxima forma patológica não pode pendurar o shell. O orçamento a recusa.
#
# Esta é a asserção mais importante do arquivo, e não é sobre desempenho:
# devolver "não casou" em SILÊNCIO ao estourar o orçamento faria o shell
# responder rápido e ERRADO, sem ninguém saber. `c/core/glob.c` faz exatamente
# isso hoje. Aqui, não.
A60='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
PEXP='*a*a*a*a*a*a*a*a*a*a*a*ab'
CEXP="case '$A60' in $PEXP) printf 'M\n';; esac"
SEXP="v='$A60'; printf '[%s]\n' \"\${v#$PEXP}\""

(cd "$W" && "$DEADLINE" 20 "$JASH" -c "$CEXP" >/dev/null 2>&1)
[ $? != 124 ] && ok "padrão exponencial é RECUSADO em vez de pendurar (o /bin/bash trava nele)" \
              || bad "orçamento ausente ou alto demais" \
                     "case com '$PEXP' contra 60 'a' não terminou em 20s"

err=$( (cd "$W" && "$DEADLINE" 20 "$JASH" -c "$CEXP" 2>&1 >/dev/null) )
out=$( (cd "$W" && "$DEADLINE" 20 "$JASH" -c "$CEXP" 2>/dev/null) )
if printf '%s' "$err" | grep -qi "recusado por custo"; then
    ok "case: a recusa GRITA no stderr (nunca um 'não casou' mudo)"
else
    bad "recusa silenciosa" \
        "estourar o orçamento não produziu diagnóstico no stderr — o shell responderia rápido e ERRADO sem avisar. stderr='$err'"
fi
if printf '%s' "$out" | grep -qi "recusado por custo"; then
    bad "canal errado" "a recusa saiu no stdout, poluindo o dado (JINV-038)"
else
    ok "a recusa não polui o stdout (JINV-038)"
fi

err=$( (cd "$W" && "$DEADLINE" 20 "$JASH" -c "$SEXP" 2>&1 >/dev/null) )
if printf '%s' "$err" | grep -qi "recusado por custo"; then
    ok "\${v#p}: a recusa também GRITA (e o laço dos N comprimentos para)"
else
    bad "recusa silenciosa na expansão" \
        "\${v#$PEXP} estourou sem diagnóstico. stderr='$err'"
fi

# ── 8. o orçamento NÃO pode disparar em padrão honesto ─────────────────
# Um teto que recusa trabalho normal é pior que o bug que ele mata.
noise=""
for praw in '*' '**********' '**/**/**/*.c' '*a*b*' '**a**b**' '*[a-c]*'; do
    e=$( (cd "$W" && "$DEADLINE" 15 "$JASH" -c "case '$deep' in $praw) :;; esac" 2>&1 >/dev/null) )
    printf '%s' "$e" | grep -qi "recusado por custo" && { noise="$praw"; break; }
done
[ -z "$noise" ] && ok "padrão honesto nunca é recusado (o teto não atrapalha)" \
                || bad "teto baixo demais" "o padrão '$noise' foi recusado — é trabalho legítimo"

echo ""
echo "=== JINV-065: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
