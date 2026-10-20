#!/usr/bin/env bash
# JINV-044/045/046 — entrada desconhecida FALHA ALTO, nunca vira dado.
#
# Três lugares onde o jash aceitava entrada desconhecida e a reinterpretava em
# silêncio. Reportado por outra sessão (BearCode) com repro medido; verifiquei
# cada um no binário antes de corrigir.
#
# **JINV-046 — flags viravam nome/alvo em builtins.** O pior de todos, porque
# aqui a entrada errada vira EFEITO:
#
#   $ jash -c 'checkpoint --json'
#   checkpoint '--json' @ depth 0     ← criou um checkpoint CHAMADO "--json"
#   $ jash -c 'rewind --json'
#   rewind: already at or before '--json'
#   $ jash -c 'impact --json'
#   impact: map is COLD for --json
#
# Um agente que sonda capacidades — que é como um agente descobre o que o shell
# faz — criava lixo de estado sem perceber.
#
# **JINV-044 — chave desconhecida no `--exec-json`.** `{"argv":[...],
# "stdim":"typo"}` executava e devolvia 0: o autor pensava ter passado stdin e
# não passou.
#
# **JINV-045 — flag longa desconhecida no binário.** `jash --nao-existe` saía
# vazio com rc=0 — indistinguível de "a flag existe e não imprimiu nada". Quem
# sonda para saber se uma feature chegou não conseguia diferenciar.
#
# É a mesma raiz do JINV-017 (cwd/env/stdin aceitos e ignorados): aceitar-e-
# ignorar. Um erro alto custa um segundo; um dado engolido custa a depuração
# inteira, longe da causa.
#
# Este gate exige as duas metades: que o desconhecido REPROVE, e que o
# conhecido continue passando — uma recusa que também barra o uso legítimo
# troca um bug por outro.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
trap 'rm -rf "$H"' EXIT INT TERM
J() { HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 "$JASH" "$@"; }

# ── JINV-046: flags desconhecidas em builtins ────────────────────────────
# `checkpoint --json` tambem foi implementado depois (JINV-049), pela mesma
# razao do `explain`. Sobram os dois que continuam sem tratamento proprio.
for c in 'rewind --json' 'impact --json'; do
    J -c "$c" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        ok "'$c' recusa (rc=$rc)"
    else
        bad "'$c' recusa" "rc=0 — a flag foi engolida como dado"
    fi
done

# `explain --json` foi IMPLEMENTADO depois (JINV-048), entao a propriedade
# certa nao e "recusa" — e "nunca engole". Ou o builtin trata a flag, ou
# reprova; o que nao pode e trata-la como argumento.
#
# Esta assercao ja falhou uma vez, quando o JINV-048 implementou a flag e este
# gate ainda exigia recusa. Um gate que trava a IMPLEMENTACAO de uma feature
# esta medindo o mecanismo em vez da propriedade — corrigido para medir a
# propriedade.
# checkpoint: mesma propriedade — trata OU recusa, nunca engole.
out=$(J -c 'checkpoint --json' 2>&1 | tail -1)
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "'checkpoint --json' e tratada (devolve JSON), nao engolida"
elif printf '%s' "$out" | grep -qi "desconhecida"; then
    ok "'checkpoint --json' e recusada — aceitavel, contanto que nao vire nome"
else
    bad "'checkpoint --json' nunca engolida" \
        "nem JSON nem recusa: virou nome de checkpoint"
fi

out=$(J -c 'explain --json rm x' 2>&1)
if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "'explain --json' e tratada (devolve JSON), nao engolida"
elif printf '%s' "$out" | grep -qi "desconhecida"; then
    ok "'explain --json' e recusada — aceitavel, contanto que nao vire argumento"
else
    bad "'explain --json' nunca engolida" \
        "nem JSON nem recusa: a flag virou argumento — saida: $(printf '%s' "$out" | head -c 70)"
fi

# ── e o pior caso: nenhum checkpoint chamado '--json' foi criado ─────────
J -c 'checkpoint --json' >/dev/null 2>&1
if J -c 'checkpoint --list' 2>/dev/null | grep -q -- "--json"; then
    bad "nenhum estado criado pela flag" \
        "existe um checkpoint chamado '--json' — a sondagem criou lixo de estado"
else
    ok "nenhum checkpoint chamado '--json' foi criado"
fi

# ── as flags LEGÍTIMAS continuam passando ────────────────────────────────
# Uma recusa que barra o uso real troca um bug por outro.
for c in 'checkpoint --list' 'rewind --list'; do
    J -c "$c" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        ok "'$c' continua funcionando"
    else
        bad "'$c' funciona" "rc=$rc — a guarda barrou uma flag real"
    fi
done
# `impact --run` exige arquivo: rc!=0 é o comportamento certo, mas a mensagem
# NÃO pode ser de flag desconhecida.
out=$(J -c 'impact --run' 2>&1)
if printf '%s' "$out" | grep -qi "opcao desconhecida"; then
    bad "'impact --run' reconhecida" "a guarda barrou a flag real do impact"
else
    ok "'impact --run' passa pela guarda (falha por falta de arquivo, não de flag)"
fi
out=$(J -c 'explain true' 2>&1)
if printf '%s' "$out" | grep -qi "opcao desconhecida"; then
    bad "'explain true' intacto" "a guarda barrou um argumento normal"
else
    ok "'explain true' intacto"
fi

# ── JINV-044: chave desconhecida em --exec-json ─────────────────────────
J --exec-json '{"argv":["echo","x"],"stdim":"typo"}' >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    ok "--exec-json com chave desconhecida recusa (rc=$rc)"
else
    bad "chave desconhecida recusa" "rc=0 — o typo foi ignorado e o comando rodou"
fi
out=$(J --exec-json '{"argv":["echo","x"],"stdim":"typo"}' 2>&1 >/dev/null | head -1)
if printf '%s' "$out" | grep -q "stdim"; then
    ok "a mensagem NOMEIA a chave errada"
else
    bad "mensagem nomeia a chave" "não diz qual chave: '$out'"
fi
J --exec-json '{"argv":["echo","x"],"cwd":"/tmp","timeout_ms":500}' >/dev/null 2>&1
[ $? -eq 0 ] && ok "chaves válidas (incl. timeout_ms reservado) continuam passando" \
             || bad "chaves válidas" "uma chave documentada foi recusada"

# ── JINV-045: flag longa desconhecida no binário ────────────────────────
J --flag-que-nao-existe >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    ok "flag longa desconhecida recusa (rc=$rc)"
else
    bad "flag desconhecida recusa" \
        "rc=0 e saída vazia — indistinguível de 'existe e não imprimiu nada'"
fi
for f in --version --whoami --capabilities-json; do
    J "$f" >/dev/null 2>&1
    [ $? -eq 0 ] && ok "$f continua funcionando" \
                 || bad "$f" "a guarda barrou uma flag real do binário"
done

echo ""
echo "=== JINV-044/045/046: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
