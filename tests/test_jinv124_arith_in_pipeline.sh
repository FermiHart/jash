#!/usr/bin/env bash
# JINV-124 — `$((…))` must expand on a line that also contains a pipe.
#
#   $ x=2; echo $((x*3))          6          <- always worked
#   $ x=2; echo $((x*3)) | cat    $((x*3))   <- bash: 6
#
# The arithmetic engine is innocent: it never ran. eval.c decides whether a
# piped line holds a compound command (`( subshell )`, `{ group; }`) whose body
# must stay unexpanded so the pipeline engine can expand it per segment. That
# detector walked the line asking "am I at a command position?" without quote
# or `$` awareness, so the SECOND `(` of `$((` looked like a subshell opening
# at a command position:
#
#     echo $((x*3)) | cat
#          ^ '$'  at_cmd = 0
#           ^ '(' no trigger, but sets at_cmd = 1     <- the wound
#            ^ '(' at_cmd && '(' => "compound" => skip ALL global expansion
#
# Skipping is line-wide, so command substitution dies with it. The pipeline
# engine re-expands only PARAMETERS per segment, which is why on the very same
# line `$HOME` survives and `$((x*3))` does not — the asymmetry that makes the
# bug look arbitrary from outside.
#
# Same family as JINV-098/116/117: a hand-rolled scanner reading shell text
# without carrying quote state. Hence the quoted cases below.
set -u

JASH="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
[ -x "$JASH" ] || { echo "SKIP: no jash at $JASH"; exit 0; }

# Correctness probes must not be answered from cache, and must not be throttled
# by the loop guard — both have produced false greens in this repo before.
export JASH_MEMO=0 JASH_LOOPGUARD=off

pass=0; fail=0

# Differential: bash is the only definition of "right" that cannot be quietly
# bent to match whatever jash happens to do.
check() {
    local desc="$1" script="$2"
    local want got wrc grc
    # JINV-127: stdout separado do stderr + paridade de rc. Com 2>&1 qualquer
    # aviso de UX que so o jash emite entra na captura e decide o veredito.
    want=$(bash -c "$script" 2>/dev/null); wrc=$?
    got=$(SHELL=/bin/bash "$JASH" -c "$script" 2>/dev/null); grc=$?
    if [ "$got" = "$want" ] && [ "$grc" = "$wrc" ]; then
        pass=$((pass+1)); printf 'ok   %s\n' "$desc"
    else
        fail=$((fail+1))
        printf 'FAIL %s\n     script: %s\n     bash: %s (rc=%s)\n     jash: %s (rc=%s)\n' \
               "$desc" "$script" "$want" "$wrc" "$got" "$grc"
    fi
}

# ── the report, and its working twin ──────────────────────────────────────
check "arith alone (twin that always worked)" 'x=2; echo $((x*3))'
check "arith piped"                           'x=2; echo $((x*3)) | cat'
check "arith literal piped"                   'echo $((2*3)) | cat'
check "arith spaced piped"                    'echo $(( 1 + 1 )) | cat'
check "arith glued to text piped"             'echo A$((1+1))B | cat'

# ── the collateral: cmdsub is suppressed by the same flag ──────────────────
check "cmdsub after arith, piped"     'echo $((1+1)) $(echo cs) | cat'
check "cmdsub alone, piped"           'echo $(echo cs) | cat'
check "param survives (asymmetry)"    'x=2; echo $((x*3)) ok | cat'

# ── it must reach every segment, not just the first ────────────────────────
check "arith in second segment"       'echo $((2+2)) | cat | cat'
check "arith after a ; then a pipe"   'echo hi >/dev/null; echo $((2+2)) | cat'
check "arith on both sides"           'echo $((1+1)) | cat; echo $((3+3)) | cat'

# ── nesting: arithmetic and command substitution feeding each other ────────
check "arith inside cmdsub, piped"    'echo $(echo $((2+2))) | cat'
check "cmdsub inside arith, piped"    'echo $(( $(echo 2) + 3 )) | cat'
check "two ariths one line, piped"    'echo $((1+1)) $((2+2)) | cat'

# ── quote awareness: the scanner must not be fooled by data ────────────────
check "pipe char as data, then paren" 'echo "| (" | cat'
check "paren as data"                 'echo "(" | cat'
check "arith inside double quotes"    'echo "v=$((2+2))" | cat'
check "arith text in single quotes"   "echo '\$((2+2))' | cat"

# ── the genuine compound must STILL be protected (no over-correction) ──────
# This is what skip_global_expand exists for: the subshell body is expanded by
# the pipeline engine in its own context. Breaking this to fix the above would
# trade one silent wrong answer for another.
check "real subshell piped"           'echo a | ( read v; echo "got $v" )'
check "real brace group piped"        'echo a | { read v; echo "got $v"; }'
check "while body keeps its context"  'printf "%s\n" 1 2 | while read p; do echo "n$p"; done'
check "subshell with arith inside"    'echo a | ( echo $((5+5)) )'

printf '\n=== JINV-124: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
