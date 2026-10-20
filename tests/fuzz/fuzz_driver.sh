#!/bin/bash
# tests/fuzz/fuzz_driver.sh — ADR-0036 W3 process-level crash/hang fuzzer.
#
# HONEST SCOPE: this is NOT coverage-guided libFuzzer. True in-process
# libFuzzer requires refactoring c/main.c so a fuzzable entry point can be
# linked without its own main() (jash's parser/eval logic isn't currently
# separable from main() and process-lifetime globals) — tracked as future
# work in Docs/BUGFIX_ROADMAP.md rather than attempted half-done here.
#
# What this DOES give: every seed in seeds.txt is a real historical repro
# line from BUGFIX_ROADMAP.md — empirically the inputs most likely to break
# jash — plus simple byte-level mutations of them, each run through the
# real binary under a timeout. A crash (SIGSEGV/SIGABRT/SIGBUS/SIGILL) or a
# hang (killed by our own watchdog) fails the run and prints the exact
# input that triggered it, ready to paste into BUGFIX_ROADMAP.md.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
JASH="${JASH:-$REPO/build/jash}"
ITERS="${1:-300}"
TIMEOUT_SECS="${JASH_FUZZ_TIMEOUT:-5}"

# Portable array build (macOS ships bash 3.2 — no `mapfile`/`readarray`).
SEEDS=()
while IFS= read -r line; do
    SEEDS+=("$line")
done < <(grep -v '^\s*#' "$HERE/seeds.txt" | grep -v '^\s*$')
if [ "${#SEEDS[@]}" -eq 0 ]; then
    echo "fuzz: no seeds loaded from $HERE/seeds.txt"
    exit 1
fi

mutate() {
    local s="$1"
    local kind=$((RANDOM % 5))
    case $kind in
        0) echo "$s" ;;  # unmutated — keep hitting the exact historical repro
        1) echo "${s}${s}" ;;                                  # duplicate
        2) echo "${s:0:$((RANDOM % (${#s} + 1)))}" ;;          # truncate
        3) printf '%s' "$s" | perl -pe 'substr($_, int(rand(length($_))), 1) = chr(int(rand(94))+33) if length($_) > 0' ;;  # 1-byte mutation
        4) echo "$s"'\\'"$s" ;;                                 # inject backslash
    esac
}

crashes=0
hangs=0
ran=0

for ((i = 0; i < ITERS; i++)); do
    seed="${SEEDS[$((RANDOM % ${#SEEDS[@]}))]}"
    input="$(mutate "$seed")"
    ran=$((ran + 1))

    perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_SECS" "$JASH" -c "$input" >/tmp/jash_fuzz_out.$$ 2>&1
    rc=$?
    rm -f "/tmp/jash_fuzz_out.$$"

    if [ "$rc" -ge 128 ]; then
        sig=$((rc - 128))
        case $sig in
            14) hangs=$((hangs + 1)); echo "HANG (timeout ${TIMEOUT_SECS}s): $input" ;;   # SIGALRM from perl watchdog
            11|6|4|8|7) crashes=$((crashes + 1)); echo "CRASH (signal $sig): $input" ;;
        esac
    fi
done

echo "---"
echo "fuzz: $ran run(s), $crashes crash(es), $hangs hang(s)"
[ "$crashes" -eq 0 ] && [ "$hangs" -eq 0 ]
