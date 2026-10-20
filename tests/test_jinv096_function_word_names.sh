#!/usr/bin/env bash
# JINV-096 — nomes de função são PALAVRAS, não identificadores POSIX.
#
# bash aceita `cmake-build() { …; }`, `npm.test() { …; }` — o nome da função
# é uma palavra, não um identificador. jash rejeitava: o scan de
# `parse_function_def` (eval.c) e o do leitor multi-linha (main.c) só
# aceitavam [a-zA-Z0-9_], e um rc file com helpers kebab-case explodia no
# boot com `name(): command not found` — a definição virava comando, o corpo
# executava solto, e a chamada falhava. Três falhas por função.
#
# A classe: validar pela FORMA errada (identificador) quando a regra real é
# outra (palavra + `()` + `{`). Mesma família do JINV-072/083 (keyword é
# PALAVRA): a gramática manda, não o preconceito sobre o lexema.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=0

run(){
  local body="$1"
  mkdir -p "$TMP/h" "$TMP/wd"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$TMP/h" "$J" t.jsh 2>&1)"; RC=$?
}

echo "JINV-096 (function names are WORDs):"

# 1. kebab-case, single line.
run 'cmake-build() { echo works; }; cmake-build'
[ "$OUT" = "works" ] && [ "$RC" -eq 0 ] && ok "single-line kebab-case" \
    || no "single-line" "OUT=$OUT RC=$RC"

# 2. kebab-case com argumentos.
run 'npm-test() { echo "got:$1"; }; npm-test hello'
[ "$OUT" = "got:hello" ] && ok "args passam" || no "args" "OUT=$OUT"

# 3. pontos e plus no nome (bash aceita).
run 'foo.bar+baz() { echo dotted; }; foo.bar+baz'
[ "$OUT" = "dotted" ] && ok "nome com . e +" || no "dot/plus" "OUT=$OUT"

# 4. multi-linha (o caminho que explodia no rc do usuário).
run 'multi-line() {
  echo ml
}
multi-line'
[ "$OUT" = "ml" ] && ok "multi-linha kebab-case" || no "multi-linha" "OUT=$OUT"

# 5. keyword `function` com hífen.
run 'function other-fn { echo kw; }; other-fn'
[ "$OUT" = "kw" ] && ok "keyword function" || no "keyword" "OUT=$OUT"

# 6. source de um rc com duas funções kebab-case: zero erros, ambas chamáveis.
printf 'cmake-build() {\n  echo "build:$1"\n}\nnpm-test() { echo test; }\n' > "$TMP/rc"
run 'source '"$TMP/rc"'; cmake-build app; npm-test'
[ "$OUT" = "build:app
test" ] && ok "source rc: duas funções, nenhum command not found" \
    || no "source rc" "OUT=$OUT"

# 7. a definição NÃO executa o corpo; a chamada NÃO redefine.
run 'f-x() { echo body; }'
[ -z "$OUT" ] && [ "$RC" -eq 0 ] && ok "definição não executa o corpo" \
    || no "def executou" "OUT=$OUT RC=$RC"

# 8. identificadores POSIX continuam intocados (regressão).
run 'plain_name() { echo plain; }; plain_name'
[ "$OUT" = "plain" ] && ok "identificador POSIX intacto" || no "posix" "OUT=$OUT"

# 9. não-confusão: um comando kebab-case inexistente continua 127,
#    sem virar definição fantasma.
run 'no-such-cmd --flag'
[ "$RC" -eq 127 ] && ok "comando inexistente continua 127" \
    || no "127" "RC=$RC OUT=$OUT"

echo ""
echo "=== JINV-096: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
