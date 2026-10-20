#!/usr/bin/env bash
# JINV-110 — a pipeline inside a `do…done` body must survive the compound
# itself being piped or redirected.
#
# The line-level pipeline splitter decides where a top-level `|` lives. It
# tracked (), {}, [[ ]] and extglob depth but not `do…done`, so a `|` inside a
# loop body was mistaken for a top-level pipe. The tail segment then became the
# word-list `cat; done`, executed as a simple command whose argv[0] is the
# literal token `cat;` — an error naming something the user never typed.
#
# The contrast is the diagnosis: the same compound standing ALONE always worked.
# The splitter is only consulted when a fork is required (pipeline/redirect),
# which is precisely the path that went untested. So every case below pairs the
# broken form with its working twin.
set -u

JASH="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
[ -x "$JASH" ] || { echo "SKIP: no jash at $JASH"; exit 0; }

# Correctness probes must not be answered from cache, and must not be throttled
# by the loop guard — both have produced false greens in this repo before.
export JASH_MEMO=0 JASH_LOOPGUARD=off

pass=0; fail=0

# Differential: jash must agree with bash, which is the only definition of
# "right" that cannot be quietly bent to match whatever jash happens to do.
check() {
    local desc="$1" script="$2"
    local want got wrc grc
    # JINV-127: stdout SEPARADO do stderr. jash escreve UX no stderr (nudge de
    # adoption, loopguard, `sigil append blocked`) que o bash nao escreve;
    # fundir os dois com 2>&1 e comparar por igualdade exata faz o ruido decidir
    # o veredito — verde por sorte, nao por merito. Filtrar UMA linha conhecida
    # com `grep -v` so adia o problema ate a proxima linha nova aparecer.
    # O contrato honesto e: mesma SAIDA e mesmo CODIGO DE SAIDA.
    want=$(bash   -c "$script" 2>/dev/null); wrc=$?
    got=$("$JASH" -c "$script" 2>/dev/null); grc=$?
    if [ "$got" = "$want" ] && [ "$grc" = "$wrc" ]; then
        pass=$((pass + 1))
        printf 'ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n     script: %s\n     bash: %s (rc=%s)\n     jash: %s (rc=%s)\n' \
               "$desc" "$script" "$want" "$wrc" "$got" "$grc"
    fi
}

echo "=== compound ALONE (the forms that always worked — guard against losing them) ==="
check 'while alone, pipeline in body'   'while false; do echo x | cat; done'
check 'for alone, pipeline in body'     'for i in 1; do echo x | cat; done'
check 'until alone, pipeline in body'   'until true; do echo x | cat; done'

echo "=== compound as RHS of a pipeline (the reported bug) ==="
check 'pipe into while, pipe in body'   'echo a | while read p; do echo $p | cat; done'
check 'pipe into for, pipe in body'     'echo a | for i in 1; do echo x | cat; done'
check 'pipe into while, two pipes'      'echo a | while read p; do echo $p | cat | cat; done'

echo "=== compound as LHS of a pipeline ==="
check 'while piped out, pipe in body'   'while false; do echo x | cat; done | cat'
check 'for piped out, pipe in body'     'for i in 1 2; do echo $i | cat; done | cat'

echo "=== compound carrying a redirect ==="
check 'while redirected in'             'while read p; do echo $p | cat; done </dev/null'
check 'while redirected out'            'while false; do echo x | cat; done >/dev/null'
check 'for redirected out'              'for i in 1; do echo $i | cat; done >/dev/null'

echo "=== nesting and neighbours of the same scanner ==="
check 'nested loops, inner pipe'        'echo a | while read p; do for i in 1; do echo $i | cat; done; done'
check 'case in pipeline, pipe in arm'   'echo a | while read p; do case $p in a) echo hit | cat;; esac; done'
check 'if in pipeline, pipe in branch'  'echo a | while read p; do if true; then echo y | cat; fi; done'
check 'word containing done is safe'    'echo a | while read p; do echo done_marker | cat; done'
check 'word containing do is safe'      'echo a | while read p; do echo dog | cat; done'

echo "=== the real-world shape that surfaced this ==="
check 'read loop feeding a filter'      'printf "%s\n" 1 2 | while read p; do echo "n$p" | cut -c1-9; done'

echo
echo "=== JINV-110: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ] || exit 1
