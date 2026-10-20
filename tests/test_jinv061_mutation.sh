#!/usr/bin/env bash
# JINV-061 — um gate que NAO PODE FALHAR e indistinguivel de um gate que passa.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
# Todo gate desta suite afirma proteger alguma coisa. Esta afirmacao so vale se
# for FALSIFICAVEL: sabotando o que ele diz proteger, ele tem que ficar
# VERMELHO. Um gate que continua verde com o codigo sabotado nao esta testando
# nada — e um teste AUSENTE que ocupa a vaga de um presente, imprime `ok`, e
# convence o proximo agente a nao olhar. Custa mais que nao ter gate nenhum.
#
# ═══ ORIGEM (o gate que nunca esteve verde por merito) ═══
#
# `tests/test_jinv019_cmdword_quotes.sh` passou desde que foi escrito, por dois
# defeitos independentes e invisiveis numa revisao:
#
#   1. `out=$(... 2>&1 | tail -1)` comparado por igualdade exata. O scorecard de
#      adoption sai no stderr real a cada 10o comando, com contador PERSISTENTE
#      entre invocacoes. O aviso era fundido na captura, entao o gate passava ou
#      falhava conforme o contador global caisse num multiplo de 10. Verde por
#      sorte, nunca por merito.
#
#   2. `out=$(cmd | tail -1); rc=$?` — o `$?` de um pipeline e o do ULTIMO
#      comando do pipe. `tail` sempre sucede, logo `rc` era constante 0, logo a
#      assercao "aspa nao terminada continua sendo erro" NAO PODIA FALHAR.
#
# ═══ AS TRES PERGUNTAS QUE ESTE GATE FAZ ═══
#
#   1. O LINT PODE FALHAR? Antes de acreditar num lint que diz "esta tudo
#      limpo", ele leva um arquivo ENVENENADO com uma instancia de cada classe
#      e tem que achar TODAS. Um lint que nao acha nada em codigo podre e ele
#      proprio um gate infalsificavel — a doenca examinando a si mesma. Esta e
#      a assercao mais importante do arquivo: sem ela, as outras sao decoracao.
#
#   2. A SUITE ESTA LIMPA? O lint varre `tests/*.sh` com catraca (excecoes
#      declaradas com `# porque:`, teto que so desce), no molde do
#      `tools/lint-no-silent-truncation.sh` (JINV-029).
#
#   3. OS GATES MORREM COM O SHELL MORTO? Mutante nulo: `build/jash` vira
#      `exit 0` dentro de um shadow root de symlinks (o binario real nunca e
#      tocado) e os gates declarados TEM que ficar vermelhos.
#
# Gate: bash tests/test_jinv061_mutation.sh
# Harness: tools/mutation-check.sh (a escolha das estrategias esta documentada la)

set -u
cd "$(dirname "$0")/.." || exit 2
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H="tools/mutation-check.sh"
[ -f "$H" ] || { echo "SKIP: $H ausente"; exit 0; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# ═══ 1. O LINT PODE FALHAR? ═══════════════════════════════════════════════
# Um arquivo com uma instancia de cada classe. Se o lint ficar quieto aqui,
# o silencio dele sobre a suite real nao significa nada.
cat > "$T/envenenado.sh" <<'FIM'
#!/usr/bin/env bash
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }

# PIPE-RC (mesma linha) — o rc e do `tail`, nunca do jash
out=$("$JASH" -c "'sem-fechar arg" 2>&1 | tail -1); rc=$?
[ "$rc" -ne 0 ] && ok "aspa nao terminada" || bad "aspa" "rc=$rc"

# PIPE-RC (linha seguinte) — mesma doenca, outra forma
res=$("$JASH" -c 'echo x' | head -1)
rc2=$?
[ "$rc2" -eq 0 ] && ok "rc2" || bad "rc2" "rc=$rc2"

# CONST-TEST — os dois lados sao literais
[ 1 = 1 ] && ok "sempre verde" || bad "inalcancavel" "nunca"

# ALWAYS-TRUE-GUARD — `echo` sempre sai 0, o `bad` e codigo morto
echo "algo" && ok "guarda sempre verdadeira" || bad "inalcancavel" "nunca"

# NO-FAIL-BRANCH — quando a condicao e falsa, nada e contado
[ "$out" = "esperado" ] && ok "sem ramo de falha"

# MERGED-EXACT — igualdade exata sobre captura com stderr fundido
m=$("$JASH" -c 'echo oi' 2>&1)
[ "$m" = "oi" ] && ok "igualdade exata" || bad "m" "veio $m"
FIM

det=$(bash "$H" lint "$T/envenenado.sh" 2>/dev/null)
for classe in PIPE-RC CONST-TEST ALWAYS-TRUE-GUARD NO-FAIL-BRANCH MERGED-EXACT; do
    if printf '%s' "$det" | grep -q ":$classe:"; then
        ok "o lint detecta $classe no arquivo envenenado"
    else
        bad "detector cego: $classe" \
            "o lint nao achou $classe em codigo que a contem — ele nao pode falhar, logo nao prova nada"
    fi
done

# as DUAS formas de PIPE-RC (mesma linha e linha seguinte) tem que aparecer
n_piperc=$(printf '%s\n' "$det" | grep -c ':PIPE-RC:')
if [ "$n_piperc" -ge 2 ]; then
    ok "PIPE-RC pega as duas formas (';rc=\$?' e rc na linha seguinte)"
else
    bad "PIPE-RC parcial" "achou $n_piperc de 2 formas — uma delas escapa do detector"
fi

# e o rc do modo cru tem que ser 1 (achado = erro), nao 0
bash "$H" lint "$T/envenenado.sh" >/dev/null 2>&1
if [ "$?" -ne 0 ]; then
    ok "arquivo envenenado devolve rc≠0"
else
    bad "rc do lint" "achou defeitos e saiu 0 — o gate que o chama nunca ficaria vermelho"
fi

# ═══ 2. E O LINT SE CALA NO CODIGO LIMPO? ════════════════════════════════
# Um detector que acusa tudo e tao inutil quanto um que nao acusa nada: vira
# ruido, alguem poe na lista de excecoes em bloco, e a catraca morre. As formas
# abaixo PARECEM as classes acima e sao todas legitimas.
cat > "$T/limpo.sh" <<'FIM'
#!/usr/bin/env bash
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
no()  { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }

# rc capturado direto do jash, sem pipe no meio
"$JASH" -c 'echo x' >/dev/null 2>&1; rc=$?
[ "$rc" -eq 0 ] && ok "rc direto" || no "rc direto" "rc=$rc"

# teste literal DENTRO do programa sob teste: a entrada, nao a assercao
t "string equal" '[ abc = abc ] && echo eq' 'eq'

# comparacao com variavel dos dois lados
out=$("$JASH" -c 'echo x' 2>/dev/null)
[ "$out" = "$esperado" ] && ok "comparacao" || no "comparacao" "$out"

# captura de SO stderr (2>&1 >/dev/null) — deliberada, nao e fusao
err=$("$JASH" -c 'falha' 2>&1 >/dev/null)
[ -n "$err" ] && ok "stderr capturado" || no "stderr" "vazio"

# pipeline cuja guarda e o grep (o rc do grep E o que interessa)
printf '%s' "$out" | grep -q 'x' && ok "grep guarda" || no "grep" "$out"

# verbo de falha que contem a palavra 'no' num texto nao pode enganar
[ "$out" = "x" ] && ok "texto com no problema aqui" || no "texto" "$out"
FIM

lim=$(bash "$H" lint "$T/limpo.sh" 2>/dev/null)
if [ -z "$lim" ]; then
    ok "o lint fica quieto nas formas legitimas (sem falso positivo)"
else
    bad "falso positivo" "acusou codigo legitimo: $(printf '%s' "$lim" | head -3)"
fi

# ═══ 3. A SUITE REAL ESTA LIMPA (catraca) ════════════════════════════════
saida=$(bash "$H" lint 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then
    ok "tests/*.sh sem assercao infalsificavel nova"
else
    bad "assercao infalsificavel na suite" "$(printf '%s' "$saida" | head -8)"
fi

# a lista de excecoes nao pode virar deposito: toda entrada precisa de razao
A="tools/mutation-check.allow"
if [ -f "$A" ]; then
    orfas=$(awk '/^[[:space:]]*#/ { if ($0 ~ /porque:/) v=1; next }
                 /^[[:space:]]*$/ { v=0; next }
                 { if (!v) print; v=0 }' "$A")
    if [ -z "$orfas" ]; then
        ok "toda excecao declarada tem '# porque:' escrito"
    else
        bad "excecao sem razao" "divida escondida: $(printf '%s' "$orfas" | head -2)"
    fi
else
    ok "sem lista de excecoes (nada declarado)"
fi

# ═══ 3b. A CATRACA CONSEGUE REPROVAR ═════════════════════════════════════
# Uma catraca que nunca foi vista reprovando e mais uma afirmacao
# infalsificavel. Aqui ela leva um caso de cada lado, num ALLOW de mentira.
#
# O teto de divida era contado com um regex sobre a PROSA do cabecalho da
# secao. Um travessao multi-byte ou uma reescrita do titulo faria o marcador
# nunca casar — e "zero por secao vazia" e "zero por marcador quebrado" dao a
# mesma resposta. Por isso o marcador virou o token fixo @@DIVIDA@@, e por isso
# ele e testado aqui.
printf '# @@DIVIDA@@\n# porque: entrada sintetica de teste\nfoo.sh:PIPE-RC: x=$(a | b); rc=$?\n' > "$T/allow_divida"
MUTCHECK_ALLOW="$T/allow_divida" MUTCHECK_CEILING=0 bash "$H" lint >"$T/o1" 2>&1
if grep -q 'SUBIU' "$T/o1"; then
    ok "a catraca reprova quando a divida passa do teto"
else
    bad "catraca inerte" "uma entrada de divida com teto 0 nao produziu erro — o teto nao esta contando nada"
fi

printf '# secao sem o marcador\n' > "$T/allow_semmarcador"
MUTCHECK_ALLOW="$T/allow_semmarcador" MUTCHECK_CEILING=0 bash "$H" lint >"$T/o2" 2>&1
if grep -q '@@DIVIDA@@' "$T/o2"; then
    ok "a perda do marcador @@DIVIDA@@ e denunciada"
else
    bad "marcador nao vigiado" \
        "sem o marcador o contador devolve 0 em silencio e o teto vira decoracao"
fi

# ═══ 4. OS GATES MORREM COM O SHELL MORTO ════════════════════════════════
# A pergunta central da mutacao, na forma mais radical: o shell inteiro deixa
# de existir. Quem continuar verde nao estava testando o shell.
saida=$(bash "$H" null 2>&1); rc=$?
if [ "$rc" -eq 0 ]; then
    n=$(printf '%s' "$saida" | sed -n 's/.*morto por todos os \([0-9]*\) gates.*/\1/p')
    ok "mutante nulo morto por todos os ${n:-?} gates declarados"
else
    bad "gate sobreviveu ao mutante nulo" "$(printf '%s' "$saida" | grep -E 'SOBREVIVEU|MUTACAO PERDIDA' | head -4)"
fi

# o conjunto nao pode esvaziar em silencio: zero gate declarado passaria sempre
n_decl=$(awk '/^NULL_GATES=/{f=1;next} f&&/^"/{exit} f&&NF' "$H" | grep -c 'tests/')
if [ "$n_decl" -ge 5 ]; then
    ok "o conjunto do mutante nulo tem $n_decl gates (nao esvaziou)"
else
    bad "conjunto vazio" "so $n_decl gates declarados — o modo 'null' passaria por vacuidade"
fi

# ═══ 5. O HARNESS RECUSA MODO DESCONHECIDO (JINV-046) ════════════════════
# Um harness que aceita `--modo-que-nao-existe` e sai 0 mente sobre ter rodado.
bash "$H" modo-inexistente >/dev/null 2>&1
[ "$?" -ne 0 ] && ok "modo desconhecido recusado" \
               || bad "guarda de modo" "'modo-inexistente' saiu 0 — silencio confundido com sucesso"

echo ""
echo "=== JINV-061: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
