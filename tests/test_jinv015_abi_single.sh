#!/usr/bin/env bash
# JINV-015 — a ABI de módulo tem que ter UMA definição só.
#
# `modules/build_module.sh` compila com `-I$ROOT/c`. Existiam DOIS headers
# declarando `jmod_call_t`: `c/modjash.h` e `c/module/modjash.h` — com ordem de
# campos totalmente diferente e `jmod_writer_t` de assinatura divergente
# (`void (*)(...)` vs `long (*)(...)`).
#
# Um módulo que escrevesse o nome natural — `#include <modjash.h>` — pegava o
# stale, enquanto o host usa o canônico. O módulo então lia `state` de onde
# estava `args` (desreferenciando um `const char*` como `jash_state_t*`) e
# chamava o writer com convenção errada. Corrupção de memória, NÃO erro de
# compilação: os dois lados compilam limpos e só quebram em runtime.
#
# Este teste COMPILA um módulo de mentira pelos dois caminhos de include e
# exige que os offsets batam. É a única forma de trancar isto — nenhum teste
# de comportamento pega uma divergência de layout.

set -u
cd "$(dirname "$0")/.." || exit 2

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

CC_BIN=${CC_FOR_TEST:-cc}
command -v "$CC_BIN" >/dev/null || { echo "SKIP: sem compilador"; exit 0; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

# Programa que imprime os offsets de jmod_call_t, parametrizado pelo include.
gen_probe() { # gen_probe <header> <saida.c>
    cat > "$2" <<EOF
#include <stdio.h>
#include <stddef.h>
#include <$1>
int main(void) {
    printf("state=%zu writer=%zu writer_ctx=%zu verb=%zu args=%zu size=%zu\n",
           offsetof(jmod_call_t, state),
           offsetof(jmod_call_t, writer),
           offsetof(jmod_call_t, writer_ctx),
           offsetof(jmod_call_t, verb),
           offsetof(jmod_call_t, args),
           sizeof(jmod_call_t));
    return 0;
}
EOF
}

INCS="-Ic -Ic/sys -Ic/osif -Ilibjef"

# ── 1. Os dois caminhos de include tem que COMPILAR ───────────────────────
gen_probe "modjash.h"        "$TMP/a.c"
gen_probe "module/modjash.h" "$TMP/b.c"

if ! "$CC_BIN" $INCS -o "$TMP/a" "$TMP/a.c" 2>"$TMP/a.err"; then
    bad "'#include <modjash.h>' compila" "$(head -3 "$TMP/a.err")"
    echo ""; echo "=== JINV-015: $pass ok / $((fail)) fail ==="; exit 1
fi
ok "'#include <modjash.h>' compila"

if ! "$CC_BIN" $INCS -o "$TMP/b" "$TMP/b.c" 2>"$TMP/b.err"; then
    bad "'#include <module/modjash.h>' compila" "$(head -3 "$TMP/b.err")"
    echo ""; echo "=== JINV-015: $pass ok / $((fail)) fail ==="; exit 1
fi
ok "'#include <module/modjash.h>' compila"

# ── 2. E tem que produzir o MESMO layout ──────────────────────────────────
la=$("$TMP/a"); lb=$("$TMP/b")
if [ "$la" = "$lb" ]; then
    ok "layout identico pelos dois caminhos ($la)"
else
    bad "layout identico" "modjash.h        -> $la
        module/modjash.h -> $lb
        Dois headers divergentes de novo: um modulo compilado pelo caminho
        errado corrompe memoria em runtime, sem erro de compilacao."
fi

# ── 3. E o writer tem que ter a MESMA assinatura ──────────────────────────
cat > "$TMP/w.c" <<'EOF'
#include <modjash.h>
#include <module/modjash.h>
/* Se os dois headers declararem jmod_writer_t diferente, este TU nao compila
 * (redefinicao conflitante) — que e exatamente o que queremos detectar. */
int main(void) { return 0; }
EOF
if "$CC_BIN" $INCS -o "$TMP/w" "$TMP/w.c" 2>"$TMP/w.err"; then
    ok "os dois headers coexistem no mesmo TU (typedefs compativeis)"
else
    bad "typedefs compativeis" "$(head -4 "$TMP/w.err")"
fi

echo ""
echo "=== JINV-015: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
