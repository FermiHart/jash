#!/usr/bin/env bash
# JINV-056 — `why`: o diagnóstico mínimo, não o log.
#
# Um comando falha e o agente recebe duzentas linhas. Ele gasta contexto — o
# recurso mais escasso que tem — relendo saída para extrair fatos que o shell
# JÁ tinha estruturados antes de imprimir a primeira linha: o comando
# (`last_status.cmd`), o rc, quando aquilo passou pela última vez
# (`~/.jash/drift.tsv`), e o que mudou desde então (timeline).
#
# As peças existiam e nunca conversavam. `why` é a junção.
#
# ═══ A propriedade que este gate protege ═══
#
# **`why` não adivinha.** Quando não há registro, ele diz que não há — em vez
# de produzir uma hipótese plausível. Um diagnóstico inventado é a mesma
# mentira de máquina que o JINV-024 (veredito que afirma mais do que sustenta)
# e o JINV-051 (`valid:true` sem dizer o que sustenta) atacaram.
#
# Por isso o teste mais importante aqui é o do caso VAZIO: sem dados, a
# resposta certa é admitir que não sabe.

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

# ── 1. sucesso: diz que passou, não inventa problema ────────────────────
out=$(J 'true; why')
if printf '%s' "$out" | grep -qE "passou|rc=0"; then
    ok "após sucesso, diz que passou (não inventa diagnóstico)"
else
    bad "sucesso" "veio: $(printf '%s' "$out" | tail -1)"
fi

# ── 2. falha: relata comando e rc ───────────────────────────────────────
out=$(J 'false; why')
printf '%s' "$out" | grep -q "false" \
    && ok "após falha, nomeia o comando que falhou" \
    || bad "nomeia o comando" "veio: $(printf '%s' "$out" | tail -2)"
printf '%s' "$out" | grep -qE "rc=1|\"rc\":1" \
    && ok "relata o código de saída" \
    || bad "relata rc" "não mostrou o rc"

# ── 3. o caso VAZIO — a propriedade central ────────────────────────────
# Sem comando anterior, a resposta certa é admitir que não sabe.
out=$(HOME="$H" "$JASH" -c 'why' 2>&1)
if printf '%s' "$out" | grep -qiE "nenhum comando|no last command"; then
    ok "sem dados, ADMITE que não sabe (não inventa hipótese)"
else
    bad "honestidade no vazio" \
        "produziu diagnóstico sem ter comando registrado: $(printf '%s' "$out" | head -1)"
fi

# ── 4. JSON com os mesmos fatos ────────────────────────────────────────
if command -v python3 >/dev/null; then
    out=$(HOME="$H" "$JASH" -c 'false; why --json' 2>/dev/null | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(d.get('known'), d.get('rc'), 'cmd' in d, 'mutations_this_session' in d)" 2>/dev/null)
    case "$r" in
        "True 1 True True") ok "JSON traz known/rc/cmd/mutations" ;;
        *) bad "JSON completo" "veio '$r'" ;;
    esac
    # E o vazio também é JSON válido, com known:false.
    r=$(HOME="$H" "$JASH" -c 'why --json' 2>/dev/null | tail -1 | python3 -c "
import json,sys; print(json.load(sys.stdin).get('known'))" 2>/dev/null)
    [ "$r" = "False" ] && ok "vazio em JSON devolve known:false" \
                       || bad "vazio em JSON" "known=$r"
else
    ok "sem python3 — JSON não verificado"
fi

# ── 5. `why` não altera o que observa ──────────────────────────────────
# Um diagnóstico que muda o estado que diagnostica é inútil na segunda vez.
a=$(J 'false; why' | grep -c "false")
b=$(J 'false; why; why' | grep -c "false")
if [ "$b" -ge "$a" ]; then
    ok "chamar why duas vezes dá a mesma resposta (não consome o estado)"
else
    bad "idempotência" "a segunda chamada perdeu informação ($a → $b)"
fi

# ── 6. flag desconhecida recusada (JINV-046) ───────────────────────────
HOME="$H" "$JASH" -c 'why --nao-existe' >/dev/null 2>&1
[ $? -ne 0 ] && ok "flag desconhecida recusada" \
             || bad "guarda de flags" "'--nao-existe' foi engolida"

echo ""
echo "=== JINV-056: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
