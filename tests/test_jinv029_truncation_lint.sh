#!/usr/bin/env bash
# JINV-029 — truncar em silêncio é proibido, e um lint mecânico cobra isso.
#
# A família documentada em `audits/ACHADO-buffers-fixos.md` — oito invariantes,
# de JINV-021 a 028 — existe porque este padrão foi usado como se fosse
# defensivo:
#
#     if (len >= TETO) len = TETO - 1;    /* corta e segue */
#
# Ele é o oposto de defensivo. Um buffer que estoura e para produz um resultado
# **plausível e errado** — e quando o corte cai numa linha de comando, o que se
# perde é o FINAL, onde moram o `>`, o `|` e o `&&`. Foi assim que o corpo de um
# heredoc virou comandos executados (JINV-022), que um `| wc -c` desapareceu
# (JINV-026) e que um `> arquivo` sumiu em 1 KB (JINV-028).
#
# Por que um LINT e não mais um conserto: o padrão de pilha do JINV-020 já
# existia **correto em quatro varreduras** do repositório, e a quinta ficou de
# fora — ninguém percebeu porque não havia gate. Consertar o nono buffer não
# impede o décimo de nascer; um lint impede.
#
# O lint separa duas naturezas que parecem iguais no código:
#   SEGURO  — clampar um ÍNDICE de profundidade num vetor de slots (4 casos);
#   DÍVIDA  — truncar DADO (11 casos), declarado e com catraca que só encolhe.
#
# Este teste não checa que o lint existe. Checa que ele **dispara**.

set -u
cd "$(dirname "$0")/.." || exit 2
LINT="tools/lint-no-silent-truncation.sh"
ALLOW="tools/lint-no-silent-truncation.allow"
[ -f "$LINT" ] || { echo "FAIL: $LINT ausente"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

# ── 1. verde no estado atual ─────────────────────────────────────────────
if bash "$LINT" >/dev/null 2>&1; then
    ok "lint verde na árvore atual"
else
    bad "lint verde" "$(bash "$LINT" 2>&1 | head -3)"
fi

# ── 2. DISPARA quando um truncamento novo aparece ────────────────────────
# Sem esta prova, o lint pode estar apenas ignorando tudo.
TMPC="c/parser/_jinv029_probe.c"
# JINV-091: a rede de segurança era `git checkout -- "$ALLOW"`, e isso é uma
# mina. Enquanto o .allow esteve FORA do git ela não fazia nada (o pathspec não
# casava, e o erro ia para /dev/null); no dia em que o arquivo for versionado,
# ela passa a APAGAR em silêncio o trabalho não commitado de quem estiver
# editando a lista de exceções — inclusive o de outro agente rodando junto.
# Um teste restaura o que ELE mexeu, a partir da cópia que ELE fez.
ALLOW_BAK="$(mktemp)" || exit 2
cp "$ALLOW" "$ALLOW_BAK"
trap 'rm -f "$TMPC"; cp "$ALLOW_BAK" "$ALLOW"; rm -f "$ALLOW_BAK"' EXIT INT TERM
printf 'void probe(void) {\n    unsigned n = 0;\n    if (n >= 4096) n = 4096 - 1;\n    (void)n;\n}\n' > "$TMPC"
if bash "$LINT" >/dev/null 2>&1; then
    bad "lint dispara em truncamento novo" \
        "um 'n = 4096 - 1' recém-plantado passou despercebido — o lint não vê nada"
else
    ok "lint DISPARA quando um truncamento novo aparece"
fi
saida=$(bash "$LINT" 2>&1)
if printf '%s' "$saida" | grep -q "_jinv029_probe"; then
    ok "a saída aponta o arquivo culpado"
else
    bad "saída aponta o culpado" "não citou o arquivo plantado"
fi
if printf '%s' "$saida" | grep -qiE "cresça|recuse"; then
    ok "a saída diz o que fazer (crescer ou recusar)"
else
    bad "saída didática" "não diz a alternativa — um lint que só reprova vira ruído"
fi
rm -f "$TMPC"

# ── 3. DISPARA quando a dívida SOBE ──────────────────────────────────────
# A catraca é o que impede "declarar" de virar sinônimo de "resolver".
cp "$ALLOW" "$ALLOW.bak"
printf '\n# porque: DÍVIDA de teste\nc/fake/zzz.c:     if (n >= X) n = X - 1;\n' >> "$ALLOW"
if bash "$LINT" 2>&1 | grep -q "SUBIU"; then
    ok "catraca DISPARA quando a dívida sobe"
else
    bad "catraca de dívida" "acrescentei uma entrada de dívida e o lint não reclamou"
fi
mv "$ALLOW.bak" "$ALLOW"

# ── 4. DISPARA quando uma exceção não se justifica ───────────────────────
cp "$ALLOW" "$ALLOW.bak"
printf '\nc/fake/sem_justificativa.c:     if (n >= X) n = X - 1;\n' >> "$ALLOW"
if bash "$LINT" 2>&1 | grep -qi "porque"; then
    ok "DISPARA quando uma exceção não tem '# porque:'"
else
    bad "exigência de justificativa" \
        "aceitei uma exceção sem razão escrita — é assim que dívida vira invisível"
fi
mv "$ALLOW.bak" "$ALLOW"

# ── 5. as duas naturezas estão separadas no arquivo ──────────────────────
if grep -q "SEGURO" "$ALLOW" && grep -q "DÍVIDA" "$ALLOW"; then
    ok "o arquivo separa SEGURO (índice) de DÍVIDA (dado)"
else
    bad "separação das naturezas" \
        "as seções sumiram — sem elas, clampar índice e perder dado viram a mesma coisa"
fi

# ── 6. o lint está ligado à suíte ────────────────────────────────────────
if grep -q "lint-no-silent-truncation" tests/test_suite.sh 2>/dev/null; then
    ok "o lint roda na suíte (não é um script órfão)"
else
    bad "ligado à suíte" "o lint existe mas ninguém o executa — vira decoração"
fi

echo ""
echo "=== JINV-029: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
