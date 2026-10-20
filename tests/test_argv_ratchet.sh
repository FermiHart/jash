#!/bin/bash
# tests/test_argv_ratchet.sh — ADR-0036 W2 ratchet.
#
# Every builtin listed in tests/argv_migrated.txt must have a non-NULL
# argv_fn in its registry.c entry ({ "name", 0, "help", jash_b_name }).
# The list only ever grows — a name disappearing or losing its argv_fn is a
# regression back to the legacy string-joined path that reintroduces the
# BUG-038/BUG-044 class (quoted multi-word args re-split on whitespace).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
REGISTRY="$REPO/c/builtins/registry.c"
LIST="$HERE/argv_migrated.txt"

fail=0
checked=0

while IFS= read -r name; do
    [ -z "$name" ] && continue
    case "$name" in \#*) continue ;; esac
    checked=$((checked + 1))

    # JINV-066: o nome de um builtin pode SER um metacaractere de regex — `[`
    # e exatamente esse caso. Sem escapar, o proprio grep recusa o padrao
    # ("brackets not balanced"), a busca volta vazia, e o ratchet acusa
    # "not found" para um builtin que esta presente e migrado. Um gate que
    # erra pela sintaxe da propria busca acusa o inocente.
    esc=$(printf '%s' "$name" | sed 's/[][\\.*^$(){}?+|]/\\&/g')
    line=$(grep -E "\{ *\"$esc\"," "$REGISTRY")
    if [ -z "$line" ]; then
        echo "RATCHET FAIL: builtin '$name' not found in registry.c"
        fail=1
        continue
    fi

    # Migrated shape: { "name", 0, "help text", jash_b_name },  — a 4th
    # field naming a jash_b_* / jash_builtin_* function right before the
    # closing brace.
    if ! echo "$line" | grep -qE ',[[:space:]]*jash_[A-Za-z_0-9]+[[:space:]]*\}'; then
        echo "RATCHET FAIL: '$name' is listed in argv_migrated.txt but its"
        echo "  registry.c entry has no argv_fn set:"
        echo "  $line"
        fail=1
    fi
done < "$LIST"

if [ "$fail" -eq 0 ]; then
    echo "argv ratchet: $checked builtin(s) confirmed migrated, 0 regressions"
fi
exit $fail
