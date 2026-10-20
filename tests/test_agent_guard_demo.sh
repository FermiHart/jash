#!/usr/bin/env bash
# tests/test_agent_guard_demo.sh — demonstração prática do agent-guard.
# Mostra, passo a passo, como o anti-bypass se comporta em cada modo.
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$PROJECT_ROOT/build/jash"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Isolamos as métricas para não poluir o HOME do usuário.
export HOME="$TMP"
mkdir -p "$HOME/.jash"

cd "$PROJECT_ROOT"

header() {
    echo
    echo "════════════════════════════════════════════════════════════════"
    echo "  $1"
    echo "════════════════════════════════════════════════════════════════"
}

run() {
    echo "\$ $1"
    eval "$1" 2>&1 || true
    echo
}

header "1. MODO USUÁRIO (sem guarda)"
echo "Nenhuma variável de agente ativa. bash/sh/python -c funcionam normalmente."
run "$JASH -c 'bash -c echo livre'"
run "$JASH -c 'python3 -c print(123)'"
run "$JASH -c 'agent-guard status'"

header "2. MODO AGENTE — WATCH (avisa, mas deixa passar)"
echo "JASH_AGENT=1 ativa o monitoramento. Bypasses são contados e avisados."
run "JASH_AGENT=1 $JASH -c 'bash -c echo bypass'"
run "JASH_AGENT=1 $JASH -c 'sh -c echo bypass2'"
run "JASH_AGENT=1 $JASH -c 'python3 -c print(456)'"
run "$JASH -c 'agent-guard status'"

header "3. MODO AGENTE — STRICT (bloqueia bypass)"
echo "JASH_GUARD_BYPASS_STRICT=1 bloqueia qualquer tentativa de escapar de jash."
run "JASH_AGENT=1 JASH_GUARD_BYPASS_STRICT=1 $JASH -c 'bash -c echo bloqueado'"
echo "rc do comando anterior: $?"
run "JASH_AGENT=1 JASH_GUARD_BYPASS_STRICT=1 $JASH -c 'python3 -c print(789)'"
echo "rc do comando anterior: $?"

header "4. COMANDOS JASH-NATIVE são contados como adoção"
echo "Comandos como speculate, contract, cost, digest, state, context contam como nativos."
run "JASH_AGENT=1 $JASH -c 'speculate echo native_command'"
run "JASH_AGENT=1 $JASH -c 'contract --expect rc 0 -- echo ok'"
run "$JASH -c 'agent-guard status'"

header "5. DASHBOARD mostra bypass rate"
echo "O dashboard agrega tanto o adoption chain quanto as métricas de bypass."
run "$JASH -c 'dashboard'"

header "6. CONTROLE VIA BUILTIN"
echo "agent-guard on/strict/relaxed/off permitem ligar/desligar em tempo de execução."
run "$JASH -c 'agent-guard on'"
run "$JASH -c 'agent-guard strict'"
run "$JASH -c 'agent-guard status'"
run "$JASH -c 'agent-guard off'"
run "$JASH -c 'agent-guard status'"

header "RESUMO"
echo "  • Sem JASH_AGENT: usuário livre — nada muda."
echo "  • Com JASH_AGENT=1: bypasses são detectados e avisados."
echo "  • Com JASH_GUARD_BYPASS_STRICT=1: bypasses são BLOQUEADOS."
echo "  • Comandos jash-native aumentam a adoção e nunca são bloqueados."
