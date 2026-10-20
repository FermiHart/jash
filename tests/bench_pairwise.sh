#!/usr/bin/env bash
# tests/bench_pairwise.sh — Isolated pairwise benchmark: jash vs ONE other shell
#
# Strategy: ALL tests for shell-1 run to completion, then ALL for shell-2.
#   No interleaving → OS scheduler and cache effects are contained per shell.
#   A brief sync+sleep between shells lets I/O settle.
#
# Usage:
#   ./tests/bench_pairwise.sh [jash_path] <other_shell>
#   ./tests/bench_pairwise.sh ./build/jash-release bash
#   ./tests/bench_pairwise.sh ./build/jash-release zsh
#   ./tests/bench_pairwise.sh ./build/jash-release fish     # fish-native variants auto-applied
#   RUNS=10 ./tests/bench_pairwise.sh ./build/jash-release bash
#
# Exit code: 0 if jash wins ≥50% of tests, 1 otherwise

set -uo pipefail

# ── Args ──────────────────────────────────────────────────────────────────────
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 [jash_path] <other_shell>"
    echo "  jash_path defaults to ./build/jash-release"
    exit 1
fi

if [[ $# -ge 2 ]]; then
    JASH="$1"; OTHER="$2"
else
    JASH="./build/jash-release"; OTHER="$1"
fi

RUNS="${RUNS:-7}"
WARMUP=2
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

# Resolve other shell binary
OTHER_BIN=$(command -v "$OTHER" 2>/dev/null || echo "")
if [[ -z "$OTHER_BIN" ]]; then
    # Maybe it's already a path
    OTHER_BIN="$OTHER"
fi
if [[ ! -x "$OTHER_BIN" ]]; then
    echo "Error: shell not found or not executable: $OTHER"
    exit 1
fi
OTHER_NAME=$(basename "$OTHER_BIN")
IS_FISH=0
[[ "$OTHER_NAME" == "fish" ]] && IS_FISH=1

bold='\033[1m'; dim='\033[2m'; rst='\033[0m'
red='\033[0;31m'; grn='\033[0;32m'; ylw='\033[1;33m'
blu='\033[0;34m'; cyn='\033[0;36m'

ms_now() {
    local t
    t=$(date +%s%N 2>/dev/null | cut -b1-13) && [[ "$t" =~ ^[0-9]{13}$ ]] && { echo "$t"; return; }
    python3 -c 'import time; print(int(time.time()*1000))'
}

bench() {
    local shell="$1" cmd="$2" n="$3" w="${4:-$WARMUP}"
    local i s e sum=0
    for ((i=0; i<w; i++)); do "$shell" -c "$cmd" >/dev/null 2>&1 || true; done
    for ((i=0; i<n; i++)); do
        s=$(ms_now); "$shell" -c "$cmd" >/dev/null 2>&1 || true; e=$(ms_now)
        sum=$((sum + e - s))
    done
    echo $((sum / n))
}

ram_kb() {
    local shell="$1"
    local out
    if [[ "$(uname)" == "Darwin" ]]; then
        out=$( { /usr/bin/time -l "$shell" -c 'exit'; } 2>&1 | awk '/maximum resident/{print int($1/1024)}' )
    else
        out=$( { /usr/bin/time -v "$shell" -c 'exit'; } 2>&1 | awk '/Maximum resident/{print int($NF)}' )
    fi
    [[ -z "$out" ]] && out="NA"
    echo "$out"
}

flush_between_shells() {
    sync 2>/dev/null || true
    # macOS: 'purge' needs sudo — skip silently. Just yield the scheduler.
    sleep 1
}

# ── Header ────────────────────────────────────────────────────────────────────
printf "${bold}"
echo "╔═══════════════════════════════════════════════════════════════════╗"
printf "║   PAIRWISE BENCHMARK: jash vs %-8s — %-18s║\n" "$OTHER_NAME" "$(date '+%Y-%m-%d %H:%M')"
echo "╚═══════════════════════════════════════════════════════════════════╝"
printf "${rst}"
echo ""
echo "  jash:  $JASH"
echo "  other: $OTHER_BIN"
echo "  runs:  $RUNS measured + $WARMUP warmup"
echo "  isolation: ALL jash tests → sync+sleep → ALL ${OTHER_NAME} tests"
[[ $IS_FISH -eq 1 ]] && echo "  fish:  POSIX-incompatible tests use fish-native syntax (marked ★)"
echo ""

# ── Test definitions ──────────────────────────────────────────────────────────
# BUG-058: these used to live in one "label|cmd|fish" array — but `|` also
# appears INSIDE commands, so every piped test was silently truncated at its
# first pipe (Pipeline measured bare `seq 1000`; Fib/Process-sub measured a
# syntax error — identically in BOTH shells, so it even passed validation).
# Three parallel arrays: no delimiter, no truncation. F E R M I ∞ H A R T
# T_FISH="SKIP" = no sensible fish-native equivalent.
declare -a T_LABEL=() T_CMD=() T_FISH=()
add_test() { T_LABEL+=("$1"); T_CMD+=("$2"); T_FISH+=("$3"); }

add_test "Startup (exit)" \
    'exit' \
    'exit'
add_test "Echo" \
    'echo hello' \
    'echo hello'
add_test "Cmd sub" \
    'v=$(echo hello); echo "$v"' \
    'set v (echo hello); echo $v'
add_test "Arith 10K" \
    'i=0; while [ $i -lt 10000 ]; do i=$((i+1)); done; echo $i' \
    'set i 0; while test $i -lt 10000; set i (math $i + 1); end; echo $i'
add_test "String concat 1K" \
    's=""; i=0; while [ $i -lt 1000 ]; do s="${s}x"; i=$((i+1)); done; echo ${#s}' \
    "set s ''; set i 0; while test \$i -lt 1000; set s \$s'x'; set i (math \$i + 1); end; echo (string length \$s)"
add_test "File ops 100" \
    'd=$(mktemp -d); for i in $(seq 1 100); do touch "$d/f$i"; done; ls "$d" | wc -l | tr -d " "; rm -rf "$d"' \
    'set d (mktemp -d); for i in (seq 1 100); touch $d/f$i; end; ls $d | wc -l | string trim; rm -rf $d'
add_test "Pipeline (ext)" \
    'seq 1000 | sort | head -1' \
    'seq 1000 | sort | head -1'
add_test "Func call 1K" \
    'f() { echo x; }; i=0; while [ $i -lt 1000 ]; do f >/dev/null; i=$((i+1)); done; echo done' \
    'function f; echo x; end; set i 0; while test $i -lt 1000; f >/dev/null; set i (math $i + 1); end; echo done'
add_test "Nested sub ×5" \
    'echo $(echo $(echo $(echo $(echo hi))))' \
    'echo (echo (echo (echo (echo hi))))'
add_test "Process sub" \
    'diff <(seq 1 50 | sort) <(seq 1 50 | sort) && echo ok' \
    'SKIP'
add_test "Fib(12) recur." \
    'fib() { [ "$1" -le 1 ] && echo "$1" || echo $((($(fib $(($1-1))))+($(fib $(($1-2)))))); }; fib 12' \
    'SKIP'
add_test "Pipe thru 10K" \
    'seq 1 10000 | sort -R | sort | uniq | wc -l | tr -d " "' \
    'seq 1 10000 | sort -R | sort | uniq | wc -l | string trim'
add_test "Background ×10" \
    'for i in $(seq 1 10); do sleep 0 & done; wait; echo done' \
    'for i in (seq 1 10); sleep 0 &; end; wait; echo done'

# ── Phase 0: output validation ────────────────────────────────────────────────
# BUG-056 lesson: this bench used to time `>/dev/null 2>&1 || true` blind — a
# shell that FAILS fast (usage error, wrong value) "won" the timing. Every test
# here has deterministic stdout, so run each once per shell and compare. A test
# whose outputs differ is timed anyway but flagged ✗ and excluded from the
# win/loss score. (Skipped for fish — different syntax/whitespace.)
declare -a V_RESULTS=()
invalid_count=0
if [[ $IS_FISH -eq 0 ]]; then
    printf "${cyn}Phase 0:${rst} Validating outputs (jash vs ${OTHER_NAME})...\n"
    for i in "${!T_LABEL[@]}"; do
        jout=$("$JASH" -c "${T_CMD[$i]}" 2>/dev/null)
        oout=$("$OTHER_BIN" -c "${T_CMD[$i]}" 2>/dev/null)
        if [[ "$jout" == "$oout" ]]; then
            V_RESULTS+=("1")
        else
            V_RESULTS+=("0")
            ((invalid_count++))
            printf "  ${red}✗ %-22s output differs${rst} (jash: '%.40s' | %s: '%.40s')\n" \
                   "${T_LABEL[$i]}" "$jout" "$OTHER_NAME" "$oout"
        fi
    done
    if ((invalid_count == 0)); then
        printf "  ${grn}✓ all %d tests produce identical output${rst}\n" "${#T_LABEL[@]}"
    fi
    echo ""
else
    for i in "${!T_LABEL[@]}"; do V_RESULTS+=("1"); done
fi

# ── Phase 1: ALL jash tests ───────────────────────────────────────────────────
printf "${cyn}Phase 1:${rst} Running ALL tests on ${bold}jash${rst}...\n"
declare -a J_RESULTS=()
for i in "${!T_LABEL[@]}"; do
    t=$(bench "$JASH" "${T_CMD[$i]}" "$RUNS")
    J_RESULTS+=("$t")
    printf "  %-22s %sms\n" "${T_LABEL[$i]}" "$t"
done
j_ram=$(ram_kb "$JASH")

# ── Isolation gap ─────────────────────────────────────────────────────────────
printf "\n${dim}[isolating — sync + sleep 1]${rst}\n\n"
flush_between_shells

# ── Phase 2: ALL other-shell tests ───────────────────────────────────────────
printf "${cyn}Phase 2:${rst} Running ALL tests on ${bold}${OTHER_NAME}${rst}...\n"
declare -a O_RESULTS=()
for i in "${!T_LABEL[@]}"; do
    if [[ $IS_FISH -eq 1 && "${T_FISH[$i]}" == "SKIP" ]]; then
        t="NA"
    elif [[ $IS_FISH -eq 1 ]]; then
        t=$(bench "$OTHER_BIN" "${T_FISH[$i]}" "$RUNS")
    else
        t=$(bench "$OTHER_BIN" "${T_CMD[$i]}" "$RUNS")
    fi
    O_RESULTS+=("$t")
    printf "  %-22s %sms\n" "${T_LABEL[$i]}" "$t"
done
o_ram=$(ram_kb "$OTHER_BIN")

# ── Comparison table ──────────────────────────────────────────────────────────
HFMT=" %-24s│ %9s │ %9s │ %12s\n"
SEP="─────────────────────────┼───────────┼───────────┼──────────────"

printf "\n${bold}"
printf "${HFMT}" "Test" "jash" "$OTHER_NAME" "Speed ratio"
printf "${rst}"
echo " $SEP"

jash_wins=0 other_wins=0 ties=0 tested=0
for i in "${!T_LABEL[@]}"; do
    label="${T_LABEL[$i]}"
    fish_cmd="${T_FISH[$i]}"
    jv="${J_RESULTS[$i]}"
    ov="${O_RESULTS[$i]}"

    # fish-native marker
    marker=""
    [[ $IS_FISH -eq 1 && "$fish_cmd" != "SKIP" && "$fish_cmd" != "${T_CMD[$i]}" ]] && marker=" ★"
    # output-mismatch: flag and exclude from the score (timing shown for reference)
    [[ "${V_RESULTS[$i]:-1}" == "0" ]] && marker="${marker} ✗"

    if [[ "${V_RESULTS[$i]:-1}" == "0" ]]; then
        ratio="${red}invalid${rst}"
        winner_str="${red}✗ output${rst}"
    elif [[ "$ov" == "NA" ]]; then
        ratio="N/A"
        winner_str="${dim}N/A${rst}"
    elif [[ "$jv" =~ ^[0-9]+$ && "$ov" =~ ^[0-9]+$ ]]; then
        ((tested++))
        if ((jv < ov)); then
            ratio=$(awk "BEGIN{printf \"jash %.1fx\", $ov/$jv}")
            winner_str="${grn}jash 🥇${rst}"
            ((jash_wins++))
        elif ((ov < jv)); then
            ratio=$(awk "BEGIN{printf \"${OTHER_NAME} %.1fx\", $jv/$ov}")
            winner_str="${ylw}${OTHER_NAME} 🥇${rst}"
            ((other_wins++))
        else
            ratio="tie"
            winner_str="${dim}tie${rst}"
            ((ties++))
        fi
    else
        ratio="---"
        winner_str="---"
    fi

    jms=$( [[ "$jv" =~ ^[0-9]+$ ]] && echo "${jv}ms" || echo "---" )
    oms=$( [[ "$ov" =~ ^[0-9]+$ ]] && echo "${ov}ms" || echo "N/A" )
    printf " %-24s│ %9s │ %9s │ %-12s\n" "${label}${marker}" "$jms" "$oms" "$ratio"
done
echo " $SEP"

echo ""
printf "  ${dim}RAM at startup (KB):${rst} jash: %sKB | %s: %sKB\n" \
    "$j_ram" "$OTHER_NAME" "$o_ram"
[[ $IS_FISH -eq 1 ]] && echo -e "  ${dim}★ = fish-native equivalent (different semantics)${rst}"

echo ""
printf "${bold}Summary:${rst} jash won %d/%d | %s won %d/%d | ties %d\n" \
    "$jash_wins" "$tested" "$OTHER_NAME" "$other_wins" "$tested" "$ties"
if ((invalid_count > 0)); then
    printf "  ${red}⚠ %d test(s) EXCLUDED — jash output differs from %s (fix correctness first)${rst}\n" \
        "$invalid_count" "$OTHER_NAME"
fi

# ── JSON ──────────────────────────────────────────────────────────────────────
mkdir -p tests/benchmark_history
TS=$(date +%Y%m%d_%H%M%S)
JSON_OUT="tests/benchmark_history/bench_pairwise_jash_vs_${OTHER_NAME}_${TS}.json"

{
    printf '{\n  "benchmark": "pairwise",\n'
    printf '  "date": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "jash": "%s",\n  "other": "%s",\n' "$JASH" "$OTHER_BIN"
    printf '  "runs": %d, "warmup": %d,\n' "$RUNS" "$WARMUP"
    printf '  "isolation": "all-jash-first-then-other",\n'
    printf '  "results": {\n'
    first=1
    for i in "${!T_LABEL[@]}"; do
        label="${T_LABEL[$i]}"
        jv="${J_RESULTS[$i]}"
        ov="${O_RESULTS[$i]}"
        key=$(echo "$label" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '_' | tr -s '_')
        [[ $first -eq 0 ]] && printf ',\n'
        jval=$( [[ "$jv" =~ ^[0-9]+$ ]] && echo "$jv" || echo "null" )
        oval=$( [[ "$ov" =~ ^[0-9]+$ ]] && echo "$ov" || echo "null" )
        vmatch=$( [[ "${V_RESULTS[$i]:-1}" == "1" ]] && echo "true" || echo "false" )
        printf '    "%s": {"jash": %s, "%s": %s, "output_match": %s}' "$key" "$jval" "$OTHER_NAME" "$oval" "$vmatch"
        first=0
    done
    printf '\n  },\n'
    printf '  "ram_kb": {"jash": %s, "%s": %s},\n' \
        "${j_ram:-null}" "$OTHER_NAME" "${o_ram:-null}"
    printf '  "summary": {"jash_wins": %d, "other_wins": %d, "ties": %d, "tested": %d}\n' \
        "$jash_wins" "$other_wins" "$ties" "$tested"
    printf '}\n'
} > "$JSON_OUT"

printf "\n${grn}✓${rst} JSON: $JSON_OUT\n"

# Exit code: 0 if jash wins majority
[[ $jash_wins -ge $other_wins ]] && exit 0 || exit 1
