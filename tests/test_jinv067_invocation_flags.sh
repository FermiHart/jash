#!/usr/bin/env bash
# JINV-067 — flag antes do `-c` NÃO PODE engolir o comando.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
# Uma opção de shell na linha de comando ou é APLICADA, ou é RECUSADA ALTO.
# O que ela nunca pode fazer é desaparecer levando junto o argumento seguinte.
#
# ═══ POR QUE ISTO IMPORTA MAIS QUE PARECE ═══
#
#   $ jash -e -c 'echo OK' ; echo $?
#   (saída vazia)  0
#
# `sh -e -c …`, `sh -eu -c …`, `sh -ec …` é a forma canônica de invocar um
# shell em CI, Makefile, Dockerfile e unidade systemd. Com o defeito, um
# pipeline INTEIRO passava verde sem ter executado uma única linha: rc 0 e
# stdout vazio são exatamente o que um passo bem-sucedido e silencioso produz.
# Não há como distinguir "rodou e não imprimiu nada" de "nunca rodou".
#
# A causa era um `break` em c/main.c que engolia qualquer letra fora de
# `l i s c`. O JINV-045 já tinha matado esta doença para flags LONGAS
# (`--xxx` devolve rc 2 nomeando a flag); as curtas continuavam sumindo — e
# piores, porque `-c 'comando'` vinha logo atrás.
#
# ═══ COMO ESTE GATE MEDE ═══
#
# `/bin/bash` é o ORÁCULO de toda asserção: nenhum rc é escrito à mão. Para
# cada invocação rodamos a MESMA linha nos dois shells e comparamos.
#
# Onde o jash tem divergência conhecida e ANTERIOR a esta onda (o rc do
# `nounset` é 1 aqui e 127 no bash 3.2), a asserção é sobre a propriedade
# derivada do oráculo — "o bash reprovou, logo o jash tem que reprovar" — e
# não sobre o número. Continua falsificável: some com a checagem no código e
# esta linha fica vermelha.
#
# Gate: bash tests/test_jinv067_invocation_flags.sh

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
BASH_BIN="${BASH_BIN:-/bin/bash}"
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$BASH_BIN" ] || { echo "SKIP: oráculo $BASH_BIN ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
W=$(mktemp -d) || exit 2
trap 'rm -rf "$H" "$W"' EXIT INT TERM
DL="$PWD/tools/deadline.sh"

# Cada invocação passa pelo cão de guarda (nunca `timeout(1)`, que não existe
# nesta máquina). Saída e rc vão para arquivo — sem pipe, para que o `$?`
# lido seja o do SHELL sob teste e não o de um filtro que sempre sucede.
J() {   HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
        "$DL" 15 "$JASH" "$@" >"$W/j.out" 2>"$W/j.err"; echo $?; }
# --norc --noprofile: HOME is already redirected, but that only hides ~/.bashrc.
# /etc/bash.bashrc is global, and on Debian/Ubuntu it WRITES TO STDOUT for an
# interactive shell ("To run a command as administrator ... use sudo"). Without
# these, the `-i -c` oracle reads as "bash did not print OK" and the case is
# scored against jash for something the distro's rc file did.
B() {   HOME="$H" "$DL" 15 "$BASH_BIN" --norc --noprofile "$@" >"$W/b.out" 2>"$W/b.err"; echo $?; }

# ══ 1. O CASO CENTRAL: a flag não pode levar o comando embora ═══════════
# Para CADA letra POSIX/bash, `SHELL <flag> -c 'echo OK'` tem que imprimir
# OK nos dois shells. É literalmente a linha do defeito.
for f in -e -u -x -v -f -a -b -C -m -h -k -p -t; do
    jrc=$(J "$f" -c 'echo OK'); jout=$(cat "$W/j.out")
    brc=$(B "$f" -c 'echo OK'); bout=$(cat "$W/b.out")
    # O oráculo tem que estar de pé: se o bash não imprimiu OK, o caso não
    # mede nada e dizer "ok" seria mentira.
    if [ "$bout" != "OK" ]; then
        bad "oráculo para $f" "bash não imprimiu OK (out='$bout' rc=$brc) — caso inválido"
        continue
    fi
    if [ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ]; then
        ok "'$f -c' executa o comando (igual ao bash: out='$bout' rc=$brc)"
    else
        bad "'$f -c' executa o comando" \
            "jash out='$jout' rc=$jrc  ≠  bash out='$bout' rc=$brc — a flag engoliu o -c"
    fi
done

# Formas coladas e a forma longa.
jrc=$(J -ec 'echo OK'); jout=$(cat "$W/j.out")
brc=$(B -ec 'echo OK'); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-ec CMD' (cluster colado) igual ao bash: out='$bout' rc=$brc" \
    || bad "'-ec CMD'" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"

jrc=$(J -eu -c 'echo OK'); jout=$(cat "$W/j.out")
brc=$(B -eu -c 'echo OK'); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-eu -c CMD' (duas letras num cluster) igual ao bash" \
    || bad "'-eu -c CMD'" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"

jrc=$(J -o errexit -c 'echo OK'); jout=$(cat "$W/j.out")
brc=$(B -o errexit -c 'echo OK'); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-o errexit -c CMD' (forma longa com operando) igual ao bash" \
    || bad "'-o errexit -c CMD'" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"

jrc=$(J +e -c 'echo OK'); jout=$(cat "$W/j.out")
brc=$(B +e -c 'echo OK'); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'+e -c CMD' (forma de DESLIGAR do POSIX) igual ao bash" \
    || bad "'+e -c CMD'" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"

# ══ 2. E AS FLAGS SÃO DE FATO APLICADAS ════════════════════════════════
# Executar o comando não basta: `-e` tem que ter o efeito de `-e`. Sem esta
# seção, aceitar-e-ignorar passaria pela seção 1 inteira.
#
# errexit: `false; echo NAO` para no `false`.
jrc=$(J -e -c 'false; echo NAO'); jout=$(cat "$W/j.out")
brc=$(B -e -c 'false; echo NAO'); bout=$(cat "$W/b.out")
if [ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ]; then
    ok "-e realmente ABORTA no primeiro erro (bash: out='$bout' rc=$brc)"
else
    bad "-e aplicado" \
        "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc — a flag foi aceita e ignorada"
fi

# nounset: variável indefinida é erro. O bash 3.2 usa rc 127 e o jash 1 —
# divergência ANTERIOR a esta onda — então a propriedade medida é a do
# oráculo: "o bash reprovou ⇒ o jash reprova".
jrc=$(J -u -c 'echo ${NAO_DEFINIDA_JINV067}'); jout=$(cat "$W/j.out")
brc=$(B -u -c 'echo ${NAO_DEFINIDA_JINV067}'); bout=$(cat "$W/b.out")
if [ "$brc" = "0" ]; then
    bad "oráculo do -u" "bash não reprovou variável indefinida sob -u — caso inválido"
elif [ "$jrc" != "0" ] && [ "$jout" = "$bout" ]; then
    ok "-u realmente reprova variável indefinida (bash rc=$brc, jash rc=$jrc)"
else
    bad "-u aplicado" "bash reprovou (rc=$brc) e o jash saiu rc=$jrc out='$jout'"
fi

# noglob: `-f` desliga a expansão de caminho.
jrc=$(J -f -c 'echo /etc/*'); jout=$(cat "$W/j.out")
brc=$(B -f -c 'echo /etc/*'); bout=$(cat "$W/b.out")
if [ "$bout" != '/etc/*' ]; then
    bad "oráculo do -f" "bash sob -f expandiu o glob (out='$bout') — caso inválido"
elif [ "$jout" = "$bout" ]; then
    ok "-f realmente desliga o glob (bash e jash: '$bout')"
else
    bad "-f aplicado" "jash='$jout' ≠ bash='$bout' — o glob expandiu mesmo com -f"
fi

# `+e` DEPOIS de `-e` tem que desligar: a ordem das opções é significativa.
jrc=$(J -e +e -c 'false; echo A'); jout=$(cat "$W/j.out")
brc=$(B -e +e -c 'false; echo A'); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-e +e' respeita a ORDEM (o + desliga): out='$bout' rc=$brc" \
    || bad "ordem de -e/+e" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"

# ══ 3. FLAG DESCONHECIDA REPROVA — E NOMEIA ════════════════════════════
# A outra metade: recusar sem nomear é quase tão ruim quanto engolir.
jrc=$(J -Z -c 'echo OK'); jerr=$(cat "$W/j.err"); jout=$(cat "$W/j.out")
brc=$(B -Z -c 'echo OK')
if [ "$brc" = "0" ]; then
    bad "oráculo do -Z" "bash aceitou -Z — a letra deixou de ser inválida, escolha outra"
elif [ "$jrc" = "0" ]; then
    bad "flag curta desconhecida reprova" \
        "jash rc=0 (bash rc=$brc) — '-Z' foi engolida, exatamente o defeito"
elif [ -n "$jout" ]; then
    bad "flag desconhecida não executa nada" "'-Z -c echo OK' imprimiu '$jout'"
else
    ok "'-Z' reprova (jash rc=$jrc, bash rc=$brc) e não executa o -c"
fi
if printf '%s' "$jerr" | grep -q -- "-Z"; then
    ok "a mensagem NOMEIA a flag recusada"
else
    bad "mensagem nomeia a flag" "stderr não cita '-Z': $(printf '%s' "$jerr" | head -c 90)"
fi

# `-o` com nome inexistente também reprova (é o mesmo defeito com operando).
jrc=$(J -o naoexiste_jinv067 -c 'echo OK'); jout=$(cat "$W/j.out")
brc=$(B -o naoexiste_jinv067 -c 'echo OK')
if [ "$brc" = "0" ]; then
    bad "oráculo do -o inválido" "bash aceitou '-o naoexiste_jinv067' — caso inválido"
elif [ "$jrc" != "0" ] && [ -z "$jout" ]; then
    ok "'-o NOME' inexistente reprova (jash rc=$jrc, bash rc=$brc) e nada executa"
else
    bad "'-o NOME' inexistente reprova" "jash rc=$jrc out='$jout' (bash rc=$brc)"
fi

# ══ 4. O MESMO DEFEITO NO MODO SCRIPT ══════════════════════════════════
# `jash -e script.sh` caía no mesmo `break`: o script nunca abria.
printf 'false\necho DEPOIS\n' > "$W/errexit.sh"
jrc=$(J -e "$W/errexit.sh"); jout=$(cat "$W/j.out")
brc=$(B -e "$W/errexit.sh"); bout=$(cat "$W/b.out")
if [ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ]; then
    ok "'-e ARQUIVO' roda o script COM errexit (igual ao bash: rc=$brc)"
else
    bad "'-e ARQUIVO'" \
        "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc — a flag comeu o script"
fi

printf 'echo ARG1=$1\n' > "$W/args.sh"
jrc=$(J -e "$W/args.sh" alpha); jout=$(cat "$W/j.out")
brc=$(B -e "$W/args.sh" alpha); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-e ARQUIVO ARG' preserva os parâmetros posicionais ('$bout')" \
    || bad "posicionais com flag" "jash='$jout' rc=$jrc ≠ bash='$bout' rc=$brc"

# `--` é fim-de-opções: o que vem depois é caminho, mesmo começando com '-'.
jrc=$(J -- "$W/args.sh" beta); jout=$(cat "$W/j.out")
brc=$(B -- "$W/args.sh" beta); bout=$(cat "$W/b.out")
[ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ] \
    && ok "'-- ARQUIVO ARG' (fim de opções) igual ao bash: '$bout'" \
    || bad "'--' fim de opções" "jash='$jout' rc=$jrc ≠ bash='$bout' rc=$brc"

# ══ 5. O QUE JÁ FUNCIONAVA NÃO PODE QUEBRAR ════════════════════════════
# Uma guarda que barra o uso legítimo troca um bug por outro (JINV-046).
for inv in "-c" "-l -c" "-i -c" "-s -c" "-lc"; do
    # shellcheck disable=SC2086
    jrc=$(J $inv 'echo OK'); jout=$(cat "$W/j.out")
    # shellcheck disable=SC2086
    brc=$(B $inv 'echo OK'); bout=$(cat "$W/b.out")
    if [ "$bout" != "OK" ]; then
        bad "oráculo para '$inv'" "bash não imprimiu OK (out='$bout') — caso inválido"
    elif [ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ]; then
        ok "'$inv CMD' continua funcionando (igual ao bash)"
    else
        bad "'$inv CMD' intacto" "jash out='$jout' rc=$jrc ≠ bash out='$bout' rc=$brc"
    fi
done

# `-c` sem argumento continua sendo erro nos dois.
jrc=$(J -c); brc=$(B -c)
if [ "$brc" = "0" ]; then
    bad "oráculo de '-c' sem argumento" "bash aceitou -c sem operando — caso inválido"
elif [ "$jrc" != "0" ]; then
    ok "'-c' sem argumento reprova (jash rc=$jrc, bash rc=$brc)"
else
    bad "'-c' sem argumento" "jash rc=0 enquanto o bash reprovou (rc=$brc)"
fi

# ══ 6. UMA PORTA SÓ: `set -X` e `-X` concordam ═════════════════════════
# A raiz do defeito era haver DUAS tabelas de opções (a do `set` e uma
# embutida no laço de argv). Esta asserção morre se alguém reintroduzir a
# segunda: para cada letra, o veredito de aceitação tem que ser o MESMO nas
# duas portas.
for f in e u x v f C m a b h k p t; do
    src=$(J -c "set -$f")
    jrc=$(J "-$f" -c 'true')
    if [ "$src" = "0" ] && [ "$jrc" = "0" ]; then
        ok "letra '$f': aceita por 'set -$f' E por '-$f' na invocação"
    elif [ "$src" != "0" ] && [ "$jrc" != "0" ]; then
        ok "letra '$f': recusada pelas DUAS portas (coerente)"
    else
        bad "porta única para '$f'" \
            "'set -$f' rc=$src mas '-$f' na invocação rc=$jrc — duas tabelas de novo"
    fi
done

echo ""
echo "=== JINV-067: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
