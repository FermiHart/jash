#!/usr/bin/env bash
# JINV-062 — um laco de varredura TEM que avancar o ponteiro.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#     Nenhuma volta de um `while` sobre `*p` pode terminar onde comecou.
#
# Se termina, a volta seguinte parte do estado exato da anterior e o shell
# trava: sem mensagem, sem log, sem core, sem exit code, sem nada para
# depurar. E o unico defeito desta casa que nao deixa rastro nenhum — o
# usuario ve o cursor parado e mata o processo.
#
# Esta classe derrubou o jash DUAS VEZES em um unico dia, em lugares
# diferentes, escrita pela mesma cabeca com oito horas de intervalo:
#
#   CASO 1 · tokenize_if_block (JINV-043): o corpo do laco externo podia
#            terminar imovel. Conserto: guarda `if (p == iter_start) p++`.
#   CASO 2 · rs_collect_paths (JINV-057): dois lacos vizinhos com conjuntos
#            de separadores ASSIMETRICOS — `(` pulava e nao parava, `)`
#            parava e nao pulava. Num `)` o ponteiro ficava imovel. Travou o
#            shell instalado na maquina do dono.
#
# Duas vezes no mesmo dia e a prova de que a cabeca nao aprende esta forma.
# So o gate aprende. Um bug so morre quando vira catraca.
#
# ═══ O QUE ESTE GATE PROVA (e por que nao basta rodar o lint) ═══
#
# Um gate que so executa o lint prova que ele RODA — nao que ele SERVE. Um
# script que imprime "✓" e sai 0 passaria igual. Por isso as asserções 1 a 6
# alimentam o lint com os DOIS BUGS REAIS reconstruidos e exigem que ele
# separe um do outro:
#
#   · a versao assimetrica do CASO 2 tem que sair SUSPEITA, e nomeando o `)`;
#   · a versao consertada tem que sair SEGURA;
#   · o if_block.c de verdade, SEM a linha da guarda, tem que sair SUSPEITO,
#     e COM ela, SEGURO — a mutacao de uma linha so, no arquivo real.
#
# Se o lint deixar de distinguir, ele nao vale nada e este gate fica vermelho.
# E a prova de que o lint PODE falhar.

set -u
cd "$(dirname "$0")/.." || exit 2
LINT="tools/lint-pointer-advance.sh"
FIX="tests/fixtures/jinv062"

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 ausente"; exit 0; }
[ -f "$LINT" ] || { echo "FAIL: $LINT ausente"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

# ── 1. CASO 2 reconstruido: o lint TEM que pegar ────────────────────────
out=$(bash "$LINT" --check "$FIX/caso2_bugado.c" 2>&1); rc=$?
if [ "$rc" = "1" ] && printf '%s' "$out" | grep -q '^SUSPEITO'; then
    ok "pega o CASO 2 (conjuntos assimetricos de rs_collect_paths)"
else
    bad "CASO 2 nao detectado" \
        "o lint absolveu a versao que travou o shell (rc=$rc): $out"
fi

# ── 2. e diz QUAL caractere trava — sem isso o aviso nao conserta nada ──
if printf '%s' "$out" | grep -qF "TRAVA em ')'"; then
    ok "nomeia o caractere que congela o ponteiro: ')'"
else
    bad "diagnostico sem o caractere" \
        "disse que ha um problema mas nao qual byte trava: $out"
fi

# ── 3. a versao CONSERTADA tem que ser absolvida ────────────────────────
# Sem esta asserção o lint poderia acusar tudo e passar na de cima.
out=$(bash "$LINT" --check "$FIX/caso2_consertado.c" 2>&1); rc=$?
if [ "$rc" = "0" ] && ! printf '%s' "$out" | grep -q '^SUSPEITO'; then
    ok "absolve a versao consertada (simetria + guarda)"
else
    bad "falso positivo no codigo correto" \
        "acusou a versao que esta no repositorio hoje: $out"
fi

# ── 4. SIMETRIA sozinha basta ───────────────────────────────────────────
# As duas condicoes de seguranca sao independentes. Se o lint exigisse a
# guarda mesmo com conjuntos simetricos, estaria cobrando cerimonia em vez de
# raciocinar sobre a propriedade — e viraria ruido.
out=$(bash "$LINT" --check "$FIX/so_simetria.c" 2>&1); rc=$?
[ "$rc" = "0" ] && ok "conjuntos simetricos bastam, sem guarda" \
                || bad "simetria nao reconhecida" "$out"

# ── 5. GUARDA sozinha basta — e pelo motivo certo ───────────────────────
out=$(bash "$LINT" --check "$FIX/so_guarda.c" 2>&1); rc=$?
if [ "$rc" = "0" ] && printf '%s' "$out" | grep -qi "guarda de progresso"; then
    ok "guarda basta mesmo com conjuntos assimetricos, e o motivo e a guarda"
else
    bad "guarda nao reconhecida" "rc=$rc — $out"
fi

# ── 6. CASO 1, no arquivo REAL, por mutacao de uma linha ────────────────
# A fixture de cima e reconstrucao; esta e o c/parser/if_block.c de verdade.
# Com a linha da guarda: SEGURO. Sem ela: SUSPEITO. E a prova mais forte que
# este gate tem — o veredito depende exatamente do byte que consertou o bug.
if [ -f c/parser/if_block.c ]; then
    orig=$(bash "$LINT" --check c/parser/if_block.c 2>&1 | grep 'tokenize_if_block')
    sed '/if (p == iter_start && \*p) p++;/d' c/parser/if_block.c > "$T/sem_guarda.c"
    if ! cmp -s c/parser/if_block.c "$T/sem_guarda.c"; then
        mut=$(bash "$LINT" --check "$T/sem_guarda.c" 2>&1 | grep 'tokenize_if_block')
        case "$orig" in
            SEGURO*) ok "tokenize_if_block com a guarda: SEGURO" ;;
            *)       bad "CASO 1 com guarda" "esperava SEGURO, veio: $orig" ;;
        esac
        case "$mut" in
            SUSPEITO*) ok "tokenize_if_block SEM a guarda: SUSPEITO (mutacao de 1 linha)" ;;
            *)         bad "CASO 1 sem guarda" \
                           "removi a guarda e o lint continuou absolvendo: $mut" ;;
        esac
    else
        bad "mutacao do CASO 1" "a linha da guarda nao existe mais em if_block.c"
    fi
else
    bad "CASO 1" "c/parser/if_block.c ausente"
fi

# ── 7. o lint nao e um carimbo de "suspeito" ────────────────────────────
# Um lint que acusa tudo e igual a um lint que nao acusa nada: ninguem lê os
# dois. Exijo que a maioria esmagadora dos lacos analisados saia SEGURA.
res=$(python3 tools/lint-pointer-advance.py --counts c 2>/dev/null)
n_ok=$(printf '%s\n' "$res" | awk '/^SEGURO/{print $2}')
n_susp=$(printf '%s\n' "$res" | awk '/^SUSPEITO/{print $2}')
if [ "${n_ok:-0}" -ge 50 ] && [ "${n_susp:-999}" -le 10 ]; then
    ok "sinal/ruido: $n_ok seguros contra $n_susp suspeitos em c/"
else
    bad "lint ruidoso ou cego" "seguros=$n_ok suspeitos=$n_susp (quero >=50 e <=10)"
fi

# ── 8. excecao sem justificativa TEM que ser recusada ───────────────────
# Uma excecao sem `# porque:` e divida escondida — o habito de onde esta
# familia inteira veio. Nao basta conferir que as de hoje tem justificativa:
# tem que provar que a catraca RECUSA uma que nao tenha. Por isso o teste
# adultera uma copia da lista em vez de contar linhas.
A=tools/lint-pointer-advance.allow
if [ -f "$A" ]; then
    cp "$A" "$T/allow_ruim"
    printf '\nc/inventado.c: PAR funcao_que_nao_existe p\n' >> "$T/allow_ruim"
    # escopo minusculo de proposito: a conferencia do '# porque:' independe do
    # que foi varrido, e varrer c/ inteiro de novo so gastaria segundos.
    saida=$(JINV062_ALLOW="$T/allow_ruim" bash "$LINT" c/builtins/builtin_hash.c 2>&1)
    if printf '%s' "$saida" | grep -q "porque"; then
        ok "recusa excecao declarada sem '# porque:'"
    else
        bad "catraca frouxa" \
            "aceitou uma excecao sem justificativa — a lista vira depósito: $saida"
    fi
else
    bad "lista de excecoes" "$A ausente"
fi

# ── 9. a catraca da arvore inteira ──────────────────────────────────────
# Nenhum laco suspeito NOVO — os conhecidos estao declarados no .allow com o
# porque de cada um, e a secao de divida so encolhe.
out=$(bash "$LINT" 2>&1); rc=$?
if [ "$rc" = "0" ]; then
    ok "catraca verde: nenhum laco de varredura suspeito novo em c/"
else
    bad "catraca vermelha" "$out"
fi

echo ""
echo "=== JINV-062: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
