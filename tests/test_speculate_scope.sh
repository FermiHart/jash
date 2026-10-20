#!/usr/bin/env bash
# tests/test_speculate_scope.sh — o sandbox do speculate é uma CÓPIA da cwd.
#
# Sem chroot e sem sandbox de kernel, argumento com caminho absoluto (ou `..`)
# atravessa a cópia e atinge o disco real. Medido em 2026-07-28:
#
#   speculate touch /abs/novo.txt → arquivo CRIADO de verdade, diff dizia "1 deleted"
#   speculate rm    /abs/alvo.txt → arquivo APAGADO de verdade, diff dizia "1 created"
#
# A inversão é consequência: o efeito caiu fora da cópia, então comparar
# cópia × real dá o sinal trocado. Apagar de verdade e reportar "created" é o
# pior resultado possível para uma primitiva que promete "mostro sem fazer".
#
# As duas pontas:
#   RECUSAR   caminho absoluto e `..` — o que o sandbox não consegue conter
#   CONTINUAR caminho relativo funcionando, com o disco intacto e o sinal certo
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_spec.XXXXXX)"
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }

# ── 1. caminho absoluto que CRIA: tem de ser recusado, e nada no disco ──────
out="$("$JASH" -c "speculate touch $T/abs_novo.txt" 2>&1)"; rc=$?
if [[ "$rc" -ne 0 ]] && grep -q "recuso" <<<"$out" && [[ ! -e "$T/abs_novo.txt" ]]; then
    ok "absoluto que cria: recusado, disco intacto"
else no "SANDBOX VAZOU (create)" "rc=$rc existe=$([[ -e $T/abs_novo.txt ]] && echo sim || echo nao)"; fi

# ── 2. caminho absoluto que APAGA: o pior caso ──────────────────────────────
echo dado > "$T/abs_alvo.txt"
out="$("$JASH" -c "speculate rm $T/abs_alvo.txt" 2>&1)"; rc=$?
if [[ "$rc" -ne 0 ]] && [[ -f "$T/abs_alvo.txt" ]]; then
    ok "absoluto que apaga: recusado, arquivo sobreviveu"
else no "SANDBOX VAZOU (delete) — speculate APAGOU o arquivo de verdade" "rc=$rc"; fi

# ── 3. `..` também sai da cópia ─────────────────────────────────────────────
mkdir -p "$T/dentro"
echo dado > "$T/vizinho.txt"
out="$(cd "$T/dentro" && "$JASH" -c "speculate rm ../vizinho.txt" 2>&1)"; rc=$?
if [[ "$rc" -ne 0 ]] && [[ -f "$T/vizinho.txt" ]]; then
    ok "'..' recusado, vizinho sobreviveu"
else no "SANDBOX VAZOU via '..'" "rc=$rc"; fi

# ── 4. a outra ponta: relativo continua funcionando, e certo ────────────────
mkdir -p "$T/rel"
out="$(cd "$T/rel" && "$JASH" -c "speculate touch novo.txt" 2>&1)"
if grep -qE "1 created" <<<"$out" && [[ ! -e "$T/rel/novo.txt" ]]; then
    ok "relativo que cria: prevê '1 created' e não toca no disco"
else no "relativo quebrou (create)" "$(tr '\n' ' ' <<<"$out" | tail -c 90)"; fi

echo dado > "$T/rel/alvo.txt"
out="$(cd "$T/rel" && "$JASH" -c "speculate rm alvo.txt" 2>&1)"
if grep -qE "1 deleted" <<<"$out" && [[ -f "$T/rel/alvo.txt" ]]; then
    ok "relativo que apaga: prevê '1 deleted' e o arquivo sobrevive"
else no "relativo quebrou (delete)" "$(tr '\n' ' ' <<<"$out" | tail -c 90)"; fi

# ── 6. rede desconhecida não pode virar falso `false` ────────────────────────
# A implementação antiga chamava lsof DEPOIS de waitpid: o processo já não
# existia, logo a sonda nunca poderia observar seus sockets. Além de mentir
# `false`, lsof pode varrer /proc por dezenas de segundos para um PID morto.
mkdir -p "$T/bin"
printf '#!/bin/sh\n: > "$SPEC_LSOF_MARKER"\nexit 0\n' > "$T/bin/lsof"
chmod +x "$T/bin/lsof"
lsof_marker="$T/lsof-foi-chamado"
out="$(cd "$T/rel" && SPEC_LSOF_MARKER="$lsof_marker" PATH="$T/bin:$PATH" \
    "$JASH" -c "speculate --json touch semrede.txt" 2>&1)"
if [[ ! -e "$lsof_marker" ]] && grep -q '"network": null' <<<"$out"; then
    ok "PID morto não é sondado; rede fica explicitamente desconhecida"
else no "speculate sondou PID morto ou inventou estado de rede" "$(tr '\n' ' ' <<<"$out" | tail -c 120)"; fi

# ── 7. e o preço disso: o speculate não pode custar segundos ────────────────
ini=$(date +%s)
(cd "$T/rel" && SPEC_LSOF_MARKER="$lsof_marker" PATH="$T/bin:$PATH" \
    "$JASH" -c "speculate touch perf.txt" >/dev/null 2>&1)
dur=$(( $(date +%s) - ini ))
if [[ "$dur" -le 3 ]]; then ok "speculate em ${dur}s (sem sonda pós-morte)"
else no "speculate lento demais — ${dur}s" "o custo fixo voltou"; fi

echo "── speculate: escopo do sandbox ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
