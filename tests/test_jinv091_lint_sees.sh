#!/usr/bin/env bash
# JINV-091 — um lint que não prova que ENXERGA é indistinguível de um lint que
# passou.
#
# ═══ O DEFEITO QUE ESTE GATE EXISTE PARA IMPEDIR ═══
#
# `tools/lint-no-silent-truncation.sh` é a catraca do JINV-029: ele existe para
# impedir que a família de truncamento silencioso (JINV-021..028) volte. Ele
# rodou VERDE, dentro da suíte, com um olho cego. A alternativa `sizeof(...)`
# do padrão estava escrita assim:
#
#     sizeof\([a-zA-Z_>.\[\]-]+\)
#
# e dentro de uma classe POSIX a barra invertida não escapa: o `\]` FECHA a
# classe. O que sobrava (`-]+\)`) exigia um literal `-]` dentro do `sizeof(…)`.
# Aquele ramo nunca casou com nada, em nenhum commit, desde o dia em que foi
# escrito. O lint anunciava "4 seguro · 11 dívida"; a dívida real era 75, em 99
# sítios — entre eles o corpo do `if`, o corpo do subshell, o `read`, o
# `[ … ]` e o `fc`, ou seja, exatamente o que ele existia para pegar.
#
# É o sétimo instrumento quebrado desta auditoria e o mais grave, porque foi o
# único que teve a chance de impedir a família inteira e disse que estava tudo
# bem.
#
# ═══ O QUE ESTE GATE PROVA (e por que não basta rodar o lint) ═══
#
# Rodar o lint prova que ele EXECUTA. Um script com `echo ✓; exit 0` passa
# nisso. As asserções abaixo exigem que ele DISCRIMINE, com fixtures que são o
# bug e o conserto lado a lado:
#
#   · a forma canônica `n = sizeof(buf) - 1;` sai ACUSADA;
#   · a mesma função com "recusa alto" sai ABSOLVIDA;
#   · um arquivo com SÓ a forma `sizeof(…)` sai acusado — é o ramo que estava
#     morto, e é o único jeito de perceber se ele morrer de novo;
#   · o padrão HISTÓRICO, alimentado com essa mesma fixture, não casa com nada
#     (o defeito era real, e está provado aqui, não afirmado);
#   · o lint com o padrão histórico de volta RECUSA A RODAR (rc=2), porque ele
#     agora se calibra antes de varrer.
#
# Se o lint deixar de distinguir, este gate fica vermelho. É a prova de que ele
# PODE falhar.

set -u
cd "$(dirname "$0")/.." || exit 2
LINT="tools/lint-no-silent-truncation.sh"
ALLOW="tools/lint-no-silent-truncation.allow"
FIX="tests/fixtures/jinv091"
DL="$PWD/tools/deadline.sh"
MUT="tools/.jinv091_mutante.sh"

[ -f "$LINT" ] || { echo "FAIL: $LINT ausente"; exit 1; }
[ -d "$FIX"  ] || { echo "FAIL: fixtures $FIX ausentes"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

trap 'rm -f "$MUT"' EXIT INT TERM

# `veredito FIXTURE` → imprime "rc=N" e a saída, sem deixar o rc escapar.
veredito() { bash "$DL" 30 bash "$LINT" --check "$FIX/$1" 2>&1; }
rc_de()    { bash "$DL" 30 bash "$LINT" --check "$FIX/$1" >/dev/null 2>&1; echo $?; }

# ── 1. a forma canônica é ACUSADA ────────────────────────────────────────
rc=$(rc_de acusado.c)
if [ "$rc" = "1" ]; then
    ok "acusa a forma canônica (n = sizeof(buf) - 1)"
else
    bad "forma canônica absolvida (rc=$rc)" \
        "é a forma exata da família JINV-021..028: $(veredito acusado.c | head -2)"
fi

# ── 2. e diz ONDE — um lint que reprova sem apontar não conserta nada ────
if veredito acusado.c | grep -q "acusado.c:"; then
    ok "a saída aponta arquivo e linha"
else
    bad "saída sem o culpado" "acusou mas não disse onde: $(veredito acusado.c | head -2)"
fi

# ── 3. o conserto é ABSOLVIDO ────────────────────────────────────────────
# Sem isto, um lint que acusa TUDO passaria na asserção 1.
rc=$(rc_de consertado.c)
if [ "$rc" = "0" ]; then
    ok "absolve a versão que cresce ou recusa alto"
else
    bad "falso positivo no conserto (rc=$rc)" \
        "acusou código que não trunca nada: $(veredito consertado.c | head -3)"
fi

# ── 4. O RAMO QUE ESTAVA MORTO: só `sizeof(…)`, nada mais ────────────────
rc=$(rc_de so_sizeof.c)
if [ "$rc" = "1" ]; then
    ok "enxerga a forma sizeof(…) sozinha — o ramo que ficou cego"
else
    bad "o ramo do sizeof morreu de novo (rc=$rc)" \
        "é o defeito de 2026-07-27 voltando: 79 sítios somem do relatório sem um sinal"
fi

# ── 5. e as três escritas de `sizeof(…)` que a classe quebrada não exprimia ─
saida=$(veredito so_sizeof.c)
if printf '%s' "$saida" | grep -q 'sizeof(nome)' && printf '%s' "$saida" | grep -q 'sizeof(save\[i\].k)'; then
    ok "cobre nome simples E membro indexado (save[i].k)"
else
    bad "cobertura parcial do sizeof" \
        "casou uma forma e perdeu outra — foi assim que a classe de caracteres falhou: $saida"
fi

# ── 6. os ramos que sempre funcionaram continuam vivos ───────────────────
rc=$(rc_de so_macro.c)
if [ "$rc" = "1" ]; then
    ok "os ramos MACRO e literal numérico continuam acusando"
else
    bad "conserto do sizeof quebrou os outros ramos (rc=$rc)" "$(veredito so_macro.c | head -3)"
fi

# ── 7. o SEGUNDO ramo cego: `*` ou `/` antes da atribuição ───────────────
# A âncora antiga (`^[^*/]*`) sumia com qualquer linha que tivesse um deref na
# guarda. Nenhum código real segura essa propriedade hoje — só esta fixture.
rc=$(rc_de estrela_no_caminho.c)
if [ "$rc" = "1" ]; then
    ok 'enxerga o sítio mesmo com "*" antes da atribuição'
else
    bad "a âncora cega voltou (rc=$rc)" \
        'um "if (*p) n = sizeof(b) - 1;" ficou invisível — mesma doença, outro ramo'
fi

# ── 8. e não chora em cima da própria documentação ───────────────────────
rc=$(rc_de so_comentario.c)
if [ "$rc" = "0" ]; then
    ok "ignora a forma citada dentro de comentário"
else
    bad "acusou documentação (rc=$rc)" \
        "um lint que acusa o texto que explica o bug vira ruído e depois vira ignorado"
fi

# ── 9. O DEFEITO ERA REAL: o padrão histórico não casa com a fixture ─────
# Provado aqui, não afirmado. O padrão vem de um arquivo para que nenhuma camada
# de aspas o conserte por acidente no caminho.
pat_hist=$(sed -E "s/^PAT='(.*)'$/\1/" "$FIX/pat_historico.txt")
pat_hoje=$(grep -m1 '^PAT=' "$LINT" | sed -E "s/^PAT='(.*)'$/\1/")
n_hist=$(grep -cE "$pat_hist" "$FIX/so_sizeof.c" 2>/dev/null || true)
n_hoje=$(grep -cE "$pat_hoje" "$FIX/so_sizeof.c" 2>/dev/null || true)
if [ "$n_hist" = "0" ] && [ "$n_hoje" -ge 1 ]; then
    ok "padrão histórico: 0 casamentos · padrão de hoje: $n_hoje (o defeito era real)"
else
    bad "a comparação histórica não fecha" \
        "histórico casou $n_hist (esperado 0) e o de hoje casou $n_hoje (esperado >=1)"
fi

# ── 10. CALIBRAGEM: com o padrão histórico de volta, o lint RECUSA rodar ──
# É o que impede o retorno silencioso: não basta o gate pegar, o próprio
# instrumento tem que se recusar a dizer "verde" quando está cego.
awk 'NR==FNR{novo=$0; next} /^PAT=/{print novo; next} {print}' \
    "$FIX/pat_historico.txt" "$LINT" > "$MUT"
mut_out=$(bash "$DL" 30 bash "$MUT" --check "$FIX/so_sizeof.c" 2>&1); mut_rc=$?
if [ "$mut_rc" = "2" ] && printf '%s' "$mut_out" | grep -q "CEGO"; then
    ok "o lint com o padrão histórico se recusa a rodar (rc=2, diz CEGO)"
else
    bad "a calibragem não pegou o padrão histórico (rc=$mut_rc)" \
        "sem ela, um regex cego volta a passar por árvore limpa: $mut_out"
fi
rm -f "$MUT"

# ── 11. a árvore está verde com os números honestos ──────────────────────
if bash "$DL" 60 bash "$LINT" >/dev/null 2>&1; then
    ok "o lint está verde na árvore atual"
else
    bad "lint vermelho" "$(bash "$LINT" 2>&1 | head -4)"
fi

# ── 12. o teto novo está EXPLICADO, com data ─────────────────────────────
# Um teto que sobe sem explicação é sabotagem. Esta asserção obriga quem
# mexer no número a escrever a razão junto — inclusive quem subir de novo.
teto=$(sed -nE 's/^DEBT_CEILING=([0-9]+).*/\1/p' "$LINT" | head -1)
if grep -q "→ $teto" "$LINT" && grep -qE '20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]' "$LINT"; then
    ok "o teto ($teto) tem a explicação e a data no cabeçalho"
else
    bad "teto sem justificativa escrita" \
        "DEBT_CEILING=$teto não aparece em nenhuma linha '→ $teto' do cabeçalho"
fi

# ── 13. a ordem das seções do .allow é a que a contagem pressupõe ────────
# A catraca conta do marcador DÍVIDA até o FIM do arquivo. Se alguém abrir
# outra seção depois dela, a dívida passa a contar entradas que não são
# dívida — e o número volta a mentir, que é a doença desta auditoria.
# Os marcadores são tokens ASCII fixos numa linha só, não prosa: medido, um
# título com travessão casava no awk e NÃO casava no grep -E, no mesmo arquivo.
l_seg=$(awk '/^# @@SEGURO@@$/{print NR; exit}' "$ALLOW")
l_mor=$(awk '/^# @@MORTO@@$/{print NR; exit}' "$ALLOW")
l_div=$(awk '/^# @@DIVIDA@@$/{print NR; exit}' "$ALLOW")
if [ -n "$l_seg" ] && [ -n "$l_mor" ] && [ -n "$l_div" ] &&
   [ "$l_seg" -lt "$l_mor" ] && [ "$l_mor" -lt "$l_div" ]; then
    ok "seções na ordem SEGURO($l_seg) < MORTO($l_mor) < DÍVIDA($l_div)"
else
    bad "ordem/nome das seções do .allow" \
        "a catraca conta da DÍVIDA até o fim: seguro=$l_seg morto=$l_mor dívida=$l_div"
fi

# ── 13b. a perda do marcador é DENUNCIADA, não contada como zero ─────────
# "zero porque a seção está vazia" e "zero porque o marcador quebrou" têm que
# ser respostas diferentes. É a mesma lição do @@DIVIDA@@ do JINV-061.
sem_marcador="$(mktemp)" || exit 2
grep -v '^# @@DIVIDA@@$' "$ALLOW" > "$sem_marcador"
out=$(JINV029_ALLOW="$sem_marcador" bash "$DL" 60 bash "$LINT" 2>&1); rc=$?
rm -f "$sem_marcador"
if [ "$rc" != "0" ] && printf '%s' "$out" | grep -q '@@DIVIDA@@'; then
    ok "a perda do marcador @@DIVIDA@@ é denunciada (rc=$rc)"
else
    bad "marcador não vigiado (rc=$rc)" \
        "sem o marcador o contador devolve 0 em silêncio e o teto vira decoração: $out"
fi

# ── 14. toda entrada mora numa das três seções ───────────────────────────
n_total=$(grep -cvE '^\s*#|^\s*$' "$ALLOW")
n_debt=$(awk '/^# @@DIVIDA@@$/{f=1;next} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
n_morto=$(awk '/^# @@MORTO@@$/{f=1;next} /^# @@DIVIDA@@$/{f=0} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
n_seg=$(awk '/^# @@SEGURO@@$/{f=1;next} /^# @@MORTO@@$/{f=0} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
if [ "$((n_seg + n_morto + n_debt))" = "$n_total" ]; then
    ok "as $n_total entradas estão classificadas (seguro $n_seg · morto $n_morto · dívida $n_debt)"
else
    bad "entrada fora de seção" \
        "$n_total entradas, mas seguro($n_seg)+morto($n_morto)+dívida($n_debt)=$((n_seg+n_morto+n_debt))"
fi

# ── 15. este gate roda na suíte ──────────────────────────────────────────
if grep -q "test_jinv091_lint_sees" tests/test_suite.sh 2>/dev/null; then
    ok "o gate está ligado à suíte (não é um script órfão)"
else
    bad "gate desligado" "existe mas ninguém executa — é como o ramo morto do regex"
fi

echo ""
echo "=== JINV-091: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
