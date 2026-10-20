#!/usr/bin/env bash
# JINV-127 — clustered short flags must mean exactly what separated flags mean.
#
# Root cause this gate protects: builtin_grep.c used to expand a cluster
# ("-nq") with a SECOND, inline copy of the flag->bit mapping, separate from
# the strcmp chain that handles the separated forms. The copy drifted: -q, -l,
# -L, -e and -m behaved differently clustered than separated. The fix rewrites
# a cluster into the canonical tokens the chain already parses, so there is
# only one mapping left. This test pins both halves:
#   A) behaviour: clustered == separated == reference grep (output AND rc)
#   A2) the one case the reference cannot judge (-L exit code) is pinned
#      absolutely, plus checked clustered-vs-separated inside jash
#   B) structure: the boolf/argf sets still cover every "-X" the chain accepts,
#      so adding a new short flag without landing it here fails CI.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
# NOT GNU grep: on macOS /usr/bin/grep is "BSD grep, GNU compatible"
# 2.6.0-FreeBSD. It is a good oracle for everything except the -L exit code
# (see section A2), so name it honestly.
REF_GREP=/usr/bin/grep
pass=0; fail=0

# A correctness battery must observe the shell, not its conveniences: memo
# would replay "output identical to your previous run" instead of re-running,
# and turn a differential into a false pass/fail on the second invocation.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0

if [ ! -x "$JASH" ]; then echo "SKIP: $JASH not built"; exit 0; fi
if [ ! -x "$REF_GREP" ]; then echo "SKIP: no system grep"; exit 0; fi

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
printf 'alpha\nbeta\nALPHA\ngamma\ndelta\n' > "$TMP/a.txt"
printf 'zeta\n-vn\n'                        > "$TMP/b.txt"

# --- A) differential: jash grep vs reference grep, output + exit code -------
run() {
    local args="$1"
    local go jo grc jrc
    go=$(cd "$TMP" && eval "$REF_GREP $args" 2>/dev/null); grc=$?
    jo=$(cd "$TMP" && "$JASH" -c "grep $args" 2>/dev/null); jrc=$?
    if [ "$go" = "$jo" ] && [ "$grc" = "$jrc" ]; then
        pass=$((pass+1)); printf 'ok    %-22s rc=%s\n' "$args" "$grc"
    else
        fail=$((fail+1))
        printf 'FAIL  %-22s\n  ref (rc=%s): %s\n  jash(rc=%s): %s\n' \
               "$args" "$grc" "$(echo "$go" | tr '\n' '|')" \
               "$jrc"  "$(echo "$jo" | tr '\n' '|')"
    fi
}

# absolute pin: jash must produce exactly this output and this exit code
pin() {   # pin <args> <expected-out-with-|-as-newline> <expected-rc>
    local args="$1" want="$2" wrc="$3" jo jrc
    jo=$(cd "$TMP" && "$JASH" -c "grep $args" 2>/dev/null); jrc=$?
    # `echo "" | tr` yields "|", which would make empty output unpinnable
    [ -n "$jo" ] && jo=$(printf '%s\n' "$jo" | tr '\n' '|')
    if [ "$jo" = "$want" ] && [ "$jrc" = "$wrc" ]; then
        pass=$((pass+1)); printf 'ok    pin %-18s rc=%s out=[%s]\n' "$args" "$jrc" "$jo"
    else
        fail=$((fail+1))
        printf 'FAIL  pin %-18s\n  want rc=%s out=[%s]\n  got  rc=%s out=[%s]\n' \
               "$args" "$wrc" "$want" "$jrc" "$jo"
    fi
}

# internal equivalence: the clustered form must equal the separated form
eq() {    # eq <clustered-args> <separated-args>
    local ca="$1" sa="$2" co so crc src
    co=$(cd "$TMP" && "$JASH" -c "grep $ca" 2>/dev/null); crc=$?
    so=$(cd "$TMP" && "$JASH" -c "grep $sa" 2>/dev/null); src=$?
    if [ "$co" = "$so" ] && [ "$crc" = "$src" ]; then
        pass=$((pass+1)); printf 'ok    eq  [%s] == [%s] rc=%s\n' "$ca" "$sa" "$crc"
    else
        fail=$((fail+1))
        printf 'FAIL  eq  [%s] != [%s]\n  clustered(rc=%s): %s\n  separated(rc=%s): %s\n' \
               "$ca" "$sa" "$crc" "$(echo "$co" | tr '\n' '|')" \
               "$src" "$(echo "$so" | tr '\n' '|')"
    fi
}

# -q: the bug that started this — clustered -q must stay silent AND set rc.
run "-q alpha a.txt";      run "-q zzzz a.txt"
run "-nq alpha a.txt";     run "-nq zzzz a.txt"
run "-cq alpha a.txt";     run "-cq zzzz a.txt"
run "-qL alpha a.txt";     run "-qL zzzz a.txt"
run "-ql alpha a.txt";     run "-ql zzzz a.txt"
# -e / -m / -A / -B / -C in clustered form (attached and separated values)
run "-e alpha a.txt";      run "-ne alpha a.txt";    run "-nealpha a.txt"
run "-m1 a a.txt";         run "-nm1 a a.txt";       run "-nm 1 a a.txt"
run "-nA1 beta a.txt";     run "-n -A 1 beta a.txt"
run "-nB1 gamma a.txt";    run "-nC1 gamma a.txt"
# plain boolean clusters
run "-in alpha a.txt";     run "-vn alpha a.txt";    run "-ivn alpha a.txt"
run "-c a a.txt";          run "-nc a a.txt"
run "-l a a.txt";          run "-nl a a.txt"
# a value that LOOKS like a cluster must stay a literal pattern
run "-e -vn b.txt"
# a token that is not a cluster at all must remain untouched
run "-- -vn b.txt"

# --- A2) -L exit code: the reference genuinely disagrees with GNU -----------
# BSD grep 2.6.0-FreeBSD (this machine's /usr/bin/grep) keeps the POSIX rule
# "status 0 iff a LINE was selected". So it prints the filename for
# `grep -L nomatch f` and still exits 1, and prints nothing for
# `grep -L match f` and still exits 0 — the status contradicts the output.
# GNU grep 3.1 fixed that: "grep -L now exits with status 0 if a file is
# listed". jash follows GNU, so the reference cannot arbitrate here and -L is
# pinned absolutely instead. Under -q nothing is listed, so the rule falls
# back to lines and both greps agree again (that pair stays differential).
pin "-L zzzz a.txt"    "a.txt|" 0     # file listed  -> 0 (GNU >= 3.1)
pin "-L alpha a.txt"   ""       1     # nothing listed -> 1
pin "-l alpha a.txt"   "a.txt|" 0     # -l is not affected by the change
pin "-l zzzz a.txt"    ""       1
# ...and whatever -L means, clustered must mean the same thing.
eq  "-nL zzzz a.txt"   "-n -L zzzz a.txt"
eq  "-nL alpha a.txt"  "-n -L alpha a.txt"
eq  "-qL zzzz a.txt"   "-q -L zzzz a.txt"

# --- B) structural: the sets must still cover the whole chain ---------------
SRC="$ROOT/c/builtins/builtin_grep.c"
chain=$("$REF_GREP" -o 'strcmp(argv\[i\], "-[A-Za-z]")' "$SRC" \
        | sed 's/.*"-\(.\)".*/\1/' | sort -u | tr -d '\n')
sets=$("$REF_GREP" -o 'const char\* \(boolf\|argf\)  *= *"[A-Za-z]*"' "$SRC" \
        | sed 's/.*"\([A-Za-z]*\)".*/\1/' | tr -d '\n' | fold -w1 | sort -u | tr -d '\n')
# -V is a terminal action (prints version); it needs no clustered form.
# NOTE: no `case` inside $( ) here — bash 3.2 (what macOS ships, and what runs
# this suite) mis-parses the pattern's ')' as the end of the substitution.
missing=""
for ch in $(echo "$chain" | fold -w1 | sort -u); do
    [ "$ch" = "V" ] && continue
    if [ "${sets#*"$ch"}" = "$sets" ]; then missing="$missing$ch"; fi
done
if [ -z "$missing" ]; then
    pass=$((pass+1)); echo "ok    structural: boolf+argf cover the chain [$chain]"
else
    fail=$((fail+1))
    echo "FAIL  structural: chain accepts -$missing but the cluster sets omit it"
    echo "      chain=[$chain] sets=[$sets]"
    echo "      => add it to boolf (no value) or argf (takes a value) in $SRC"
fi

echo "=== JINV-127: $pass pass, $fail fail ==="
[ "$fail" -eq 0 ]
