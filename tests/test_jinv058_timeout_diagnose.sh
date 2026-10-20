#!/usr/bin/env bash
# JINV-058 — `timeout --diagnose`: onde ele estava quando morreu.
#
# Um comando que trava é indistinguível de um comando lento. O `timeout` já
# resolvia metade: mata e devolve 124. O agente ficava sabendo QUE travou e
# nada sobre ONDE — que é a única informação que muda o que ele faz em seguida.
#
# Nesta sessão isso custou caro. Duas correções travaram o shell; três
# hipóteses foram refutadas em sequência; o ponto real (`split_statements`,
# sem consciência de aspas) só apareceu quando um `sample` foi rodado à mão
# contra o PID. O `timeout` estava a um passo dessa resposta e não deu.
#
# ═══ A propriedade que este gate protege ═══
#
# **`--diagnose` nunca devolve silêncio.**
#
# Ou entrega as frames, ou diz por que não pôde — plataforma sem amostrador,
# amostrador que não rodou, processo que já tinha saído. Um campo vazio seria
# indistinguível de "falhei", e essa ambiguidade é exatamente o defeito do
# JINV-045 (flag que devolve rc=0 sem fazer nada) e do JINV-017 (campo aceito
# e ignorado).
#
# Por isso as asserções 4 e 5 são as que importam: o campo `where` tem que
# estar presente e NÃO-VAZIO em todos os caminhos, inclusive naquele em que
# não havia nada a diagnosticar. Este gate roda em qualquer plataforma
# justamente porque afirma sobre a honestidade, não sobre o amostrador.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM
J() { HOME="$H" "$JASH" -c "$1" 2>&1; }

# ── 1. sem --diagnose, o comportamento antigo é IDÊNTICO ────────────────
# Uma feature nova não pode mudar o caminho de quem não pediu por ela.
out=$(J 'timeout 1s sleep 20')
HOME="$H" "$JASH" -c 'timeout 1s sleep 20' >/dev/null 2>&1; rc=$?
if [ "$rc" = "124" ] && ! printf '%s' "$out" | grep -qi "onde estava"; then
    ok "sem --diagnose: rc=124 e nenhuma saída nova"
else
    bad "não-regressão" "rc=$rc, saída inesperada: $(printf '%s' "$out" | head -1)"
fi

# ── 2. com --diagnose, o rc continua sendo 124 ──────────────────────────
HOME="$H" "$JASH" -c 'timeout --diagnose 1s sleep 20' >/dev/null 2>&1
[ $? = "124" ] && ok "--diagnose não altera o rc do timeout" \
               || bad "rc com --diagnose" "o contrato do timeout mudou"

# ── 3. em texto, diz alguma coisa sobre onde ────────────────────────────
out=$(J 'timeout --diagnose 1s sleep 20')
if printf '%s' "$out" | grep -qiE "onde estava|diagnostico indisponivel"; then
    ok "texto: entrega as frames OU declara a indisponibilidade"
else
    bad "silêncio em texto" \
        "--diagnose não produziu nem diagnóstico nem razão: $(printf '%s' "$out" | head -2)"
fi

# ── 4. JSON: o campo existe e é NÃO-VAZIO — a propriedade central ───────
if command -v python3 >/dev/null; then
    out=$(HOME="$H" "$JASH" -c 'timeout --diagnose --json 1s sleep 20' 2>/dev/null | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d.get('timed_out'), d.get('exit_code'),
      'where' in d, bool(d.get('where','').strip()), 'where_available' in d)" 2>/dev/null)
    case "$r" in
        "True 124 True True True")
            ok "JSON: where presente e NÃO-VAZIO no caminho do estouro" ;;
        *) bad "JSON no estouro" "veio '$r' (esperado 'True 124 True True True')" ;;
    esac

    # ── 5. e TAMBÉM quando não havia o que diagnosticar ─────────────────
    # Este é o caminho que se perde de vista: sem estouro, um `where` vazio
    # seria indistinguível de uma amostragem que falhou.
    out=$(HOME="$H" "$JASH" -c 'timeout --diagnose --json 5s true' 2>/dev/null | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d.get('timed_out'), d.get('where_available'), bool(d.get('where','').strip()))" 2>/dev/null)
    case "$r" in
        "False False True")
            ok "sem estouro: where_available=false MAS where explica por quê" ;;
        "False False False")
            bad "silêncio no caminho vazio" \
                "where saiu vazio — indistinguível de 'falhei em amostrar' (JINV-045)" ;;
        *) bad "JSON sem estouro" "veio '$r'" ;;
    esac

    # ── 6. sem --diagnose o JSON não ganha campos ───────────────────────
    out=$(HOME="$H" "$JASH" -c 'timeout --json 1s sleep 20' 2>/dev/null | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys; print('where' in json.load(sys.stdin))" 2>/dev/null)
    [ "$r" = "False" ] && ok "sem --diagnose, o JSON antigo fica intacto" \
                       || bad "contrato do JSON" "campos novos apareceram sem serem pedidos"
else
    ok "sem python3 — JSON não verificado"
fi

# ── 7. o JSON continua sendo JSON válido (a ordem dos campos) ──────────
# A amostra tem quebras de linha e aspas; se o escape falhar, o consumidor
# recebe lixo em vez de dado — a mesma família do JINV-026/028.
if command -v python3 >/dev/null; then
    HOME="$H" "$JASH" -c 'timeout --diagnose --json 1s sleep 20' 2>/dev/null | tail -1 \
        | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null \
        && ok "a amostra multilinha não quebra o JSON (escape correto)" \
        || bad "JSON malformado" "as frames vazaram sem escape"
fi

# ── 8. flag desconhecida continua recusada ─────────────────────────────
HOME="$H" "$JASH" -c 'timeout --diagnostico 1s true' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag parecida mas errada é recusada" \
             || bad "guarda de flags" "'--diagnostico' foi engolida"

echo ""
echo "=== JINV-058: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
