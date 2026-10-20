#!/usr/bin/env bash
# tests/bench_aggressive.sh — Aggressive shell benchmark (extended feature coverage)
# Adds: string-concat, func-overhead, glob-expand, process-sub, nested-subshell,
#       recursive fibonacci, native-vs-external (jash unique), background-job
# Fish: most tests N/A (POSIX-incompatible); shown with marker; fish-native where possible
#
# Usage:
#   ./tests/bench_aggressive.sh [jash_path]
#   RUNS=5 ./tests/bench_aggressive.sh

set -uo pipefail

JASH="${1:-./build/jash-release}"
RUNS="${RUNS:-5}"
WARMUP=2
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

_has() { command -v "$1" >/dev/null 2>&1; }
BASH_BIN=$(command -v bash)
ZSH_BIN=$(command -v zsh 2>/dev/null || echo "")
FISH_BIN=$(command -v fish 2>/dev/null || echo "")

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

winner() {
    local best=2147483647 name=""
    local names=("jash" "bash" "zsh") vals=("$1" "$2" "$3") n
    for n in 0 1 2; do
        local v="${vals[$n]}"
        [[ "$v" =~ ^[0-9]+$ ]] && ((v < best)) && { best=$v; name="${names[$n]}"; }
    done
    echo "${name:-?}"
}

HFMT=" %-28s│ %9s │ %9s │ %9s │ %s\n"
SEP="─────────────────────────────┼───────────┼───────────┼───────────┼──────────"

print_header() {
    printf "\n${bold}${blu}${HFMT}${rst}" "Test" "jash" "bash" "zsh" "Winner (fish excl.)"
    echo " $SEP"
}

print_row() {
    local label="$1" jv="$2" bv="$3" zv="$4" note="${5:-}"
    local win; win=$(winner "$jv" "$bv" "$zv")
    local jms bms zms
    jms=$( [[ "$jv" =~ ^[0-9]+$ ]] && echo "${jv}ms" || echo "---" )
    bms=$( [[ "$bv" =~ ^[0-9]+$ ]] && echo "${bv}ms" || echo "---" )
    zms=$( [[ "$zv" =~ ^[0-9]+$ ]] && echo "${zv}ms" || echo "---" )
    [[ "$win" == "jash" ]] && jms="${jms} 🥇"
    [[ "$win" == "bash" ]] && bms="${bms} 🥇"
    [[ "$win" == "zsh"  ]] && zms="${zms} 🥇"
    printf " %-28s│ %9s │ %9s │ %9s │ ${grn}%s${rst}${dim} %s${rst}\n" \
        "$label" "$jms" "$bms" "$zms" "$win" "$note"
}

printf "${bold}"
echo "╔═══════════════════════════════════════════════════════════════════╗"
printf "║    JASH AGGRESSIVE BENCHMARK — %-34s║\n" "$(date '+%Y-%m-%d %H:%M')"
echo "╚═══════════════════════════════════════════════════════════════════╝"
printf "${rst}"
echo ""
echo "  jash: $JASH | runs: $RUNS + $WARMUP warmup"
echo "  Note: fish excluded from POSIX tests (fish-native results appended below)"
echo "  Note: 'Native vs External' is jash-exclusive (builtin ls vs /bin/ls)"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# TEST DEFINITIONS
# ─────────────────────────────────────────────────────────────────────────────

# 1. String concatenation: build a 1000-char string in a loop
CMD_STRCAT='s=""; i=0; while [ $i -lt 1000 ]; do s="${s}x"; i=$((i+1)); done; echo ${#s}'

# 2. Function call overhead: call a function 1000 times (no redirect — avoids jash fork-on-redir bug)
CMD_FUNCALL='f() { x=1; }; i=0; while [ $i -lt 1000 ]; do f; i=$((i+1)); done'

# 3. Glob expansion: count .c files in codebase
CMD_GLOB='files=$(echo c/**/*.c 2>/dev/null || echo c/*.c); echo done'

# 4. Process substitution: diff two sorted streams
CMD_PROCSUB='diff <(seq 1 100 | sort) <(seq 1 100 | sort) && echo ok'

# 5. Nested subshell (5 levels deep)
CMD_NESTED='echo $(echo $(echo $(echo $(echo hello))))'

# 6. Fibonacci recursive function via cmdsub (uses $1 not ${1}; jash bug: ${N} broken in arith)
#    FIB_N=10 for QEMU/slow environments; default 18 for native hardware
_FIB_N="${FIB_N:-18}"
CMD_FIB="fib() { [ \"\$1\" -le 1 ] && echo \"\$1\" || echo \$((\$(fib \$((\$1-1)))+\$(fib \$((\$1-2))))); }; fib $_FIB_N"

# 7. Arithmetic float (bc-based): 10K iterations writing to /dev/null
CMD_BCARITH='for i in $(seq 1 100); do echo "scale=4; $i * 3.14" | bc; done >/dev/null'

# 8. Array operations: create, iterate, sum 1000-element array (bash arrays)
CMD_ARRAY='a=(); for i in $(seq 1 1000); do a+=($i); done; sum=0; for v in "${a[@]}"; do sum=$((sum+v)); done; echo $sum'

# 9. Background job: spawn 10 background processes, wait
CMD_BGJOB='for i in $(seq 1 10); do sleep 0 & done; wait'

# 10. Pipe throughput: pipe 10000 lines through sort+uniq
CMD_PIPETHRU='seq 1 10000 | sort -R | sort | uniq | wc -l | tr -d " "'

# ─────────────────────────────────────────────────────────────────────────────
# jash-exclusive: native builtin ls vs /bin/ls
# ─────────────────────────────────────────────────────────────────────────────
CMD_NATIVE_LS='for i in $(seq 1 100); do ls /tmp >/dev/null; done'
CMD_EXTERN_LS='for i in $(seq 1 100); do /bin/ls /tmp >/dev/null; done'

# ─────────────────────────────────────────────────────────────────────────────
# RUN — all per shell, not interleaved
# ─────────────────────────────────────────────────────────────────────────────

printf "${cyn}[ jash ]${rst} running aggressive suite...\n"
j_sc=$(bench "$JASH" "$CMD_STRCAT" "$RUNS")
j_fn=$(bench "$JASH" "$CMD_FUNCALL" "$RUNS")
j_gl=$(bench "$JASH" "$CMD_GLOB" "$RUNS")
j_ps=$(bench "$JASH" "$CMD_PROCSUB" "$RUNS")
j_ns=$(bench "$JASH" "$CMD_NESTED" "$RUNS")
j_fb=$(bench "$JASH" "$CMD_FIB" "$RUNS")
j_bc=$(bench "$JASH" "$CMD_BCARITH" "$RUNS")
j_ar=$(bench "$JASH" "$CMD_ARRAY" "$RUNS")
j_bg=$(bench "$JASH" "$CMD_BGJOB" "$RUNS")
j_pt=$(bench "$JASH" "$CMD_PIPETHRU" "$RUNS")
# native vs external ls (jash only)
j_nat=$(bench "$JASH" "$CMD_NATIVE_LS" "$RUNS")
j_ext=$(bench "$JASH" "$CMD_EXTERN_LS" "$RUNS")

printf "${cyn}[ bash ]${rst} running aggressive suite...\n"
b_sc=$(bench "$BASH_BIN" "$CMD_STRCAT" "$RUNS")
b_fn=$(bench "$BASH_BIN" "$CMD_FUNCALL" "$RUNS")
b_gl=$(bench "$BASH_BIN" "$CMD_GLOB" "$RUNS")
b_ps=$(bench "$BASH_BIN" "$CMD_PROCSUB" "$RUNS")
b_ns=$(bench "$BASH_BIN" "$CMD_NESTED" "$RUNS")
b_fb=$(bench "$BASH_BIN" "$CMD_FIB" "$RUNS")
b_bc=$(bench "$BASH_BIN" "$CMD_BCARITH" "$RUNS")
b_ar=$(bench "$BASH_BIN" "$CMD_ARRAY" "$RUNS")
b_bg=$(bench "$BASH_BIN" "$CMD_BGJOB" "$RUNS")
b_pt=$(bench "$BASH_BIN" "$CMD_PIPETHRU" "$RUNS")

if [[ -n "$ZSH_BIN" ]]; then
    printf "${cyn}[ zsh  ]${rst} running aggressive suite...\n"
    z_sc=$(bench "$ZSH_BIN" "$CMD_STRCAT" "$RUNS")
    z_fn=$(bench "$ZSH_BIN" "$CMD_FUNCALL" "$RUNS")
    z_gl=$(bench "$ZSH_BIN" "$CMD_GLOB" "$RUNS")
    z_ps=$(bench "$ZSH_BIN" "$CMD_PROCSUB" "$RUNS")
    z_ns=$(bench "$ZSH_BIN" "$CMD_NESTED" "$RUNS")
    z_fb=$(bench "$ZSH_BIN" "$CMD_FIB" "$RUNS")
    z_bc=$(bench "$ZSH_BIN" "$CMD_BCARITH" "$RUNS")
    z_ar=$(bench "$ZSH_BIN" "$CMD_ARRAY" "$RUNS")
    z_bg=$(bench "$ZSH_BIN" "$CMD_BGJOB" "$RUNS")
    z_pt=$(bench "$ZSH_BIN" "$CMD_PIPETHRU" "$RUNS")
else
    z_sc=NA; z_fn=NA; z_gl=NA; z_ps=NA; z_ns=NA
    z_fb=NA; z_bc=NA; z_ar=NA; z_bg=NA; z_pt=NA
fi

# Fish-native equivalents for reference
if [[ -n "$FISH_BIN" ]]; then
    printf "${cyn}[ fish ]${rst} running fish-native tests...\n"
    FISH_STRCAT='set s ""; set i 0; while test $i -lt 1000; set s $s"x"; set i (math $i + 1); end; echo (string length $s)'
    FISH_FUNCALL='function f; echo x; end; set i 0; while test $i -lt 1000; f >/dev/null; set i (math $i + 1); end'
    FISH_PIPE='seq 1 10000 | sort -R | sort | uniq | wc -l | string trim'
    f_sc=$(bench "$FISH_BIN" "$FISH_STRCAT" "$RUNS")
    f_fn=$(bench "$FISH_BIN" "$FISH_FUNCALL" "$RUNS")
    f_pt=$(bench "$FISH_BIN" "$FISH_PIPE" "$RUNS")
else
    f_sc=NA; f_fn=NA; f_pt=NA
fi

# ─────────────────────────────────────────────────────────────────────────────
# RESULTS
# ─────────────────────────────────────────────────────────────────────────────
print_header
print_row "String concat ×1000"   "$j_sc" "$b_sc" "${z_sc:-NA}"
print_row "Function call ×1000"   "$j_fn" "$b_fn" "${z_fn:-NA}"
print_row "Glob expand (**/*.c)"  "$j_gl" "$b_gl" "${z_gl:-NA}"
print_row "Process sub <(cmd)"    "$j_ps" "$b_ps" "${z_ps:-NA}"
print_row "Nested subshell ×5"    "$j_ns" "$b_ns" "${z_ns:-NA}"
print_row "Fibonacci(18) recur."  "$j_fb" "$b_fb" "${z_fb:-NA}"
print_row "bc arith ×100"         "$j_bc" "$b_bc" "${z_bc:-NA}"
print_row "Array ops ×1000"       "$j_ar" "$b_ar" "${z_ar:-NA}"
print_row "Background job ×10"    "$j_bg" "$b_bg" "${z_bg:-NA}"
print_row "Pipe throughput 10K"   "$j_pt" "$b_pt" "${z_pt:-NA}"
echo " $SEP"

echo ""
printf "  ${ylw}jash-exclusive: native builtin ls vs /bin/ls (×100)${rst}\n"
speedup=$(python3 -c "print(f'{int(\"$j_ext\")/int(\"$j_nat\"):.1f}')" 2>/dev/null || echo "?")
printf "    jash builtin ls: %sms | jash /bin/ls: %sms | speedup: %sx\n" \
    "$j_nat" "$j_ext" "$speedup"

echo ""
printf "  ${ylw}Fish-native equivalents (different semantics, informational):${rst}\n"
printf "    String concat ×1000:  %sms (vs jash %sms)\n" "$f_sc" "$j_sc"
printf "    Function call ×1000:  %sms (vs jash %sms)\n" "$f_fn" "$j_fn"
printf "    Pipe throughput 10K:  %sms (vs jash %sms)\n" "$f_pt" "$j_pt"

# ─────────────────────────────────────────────────────────────────────────────
# JSON
# ─────────────────────────────────────────────────────────────────────────────
mkdir -p tests/benchmark_history
TS=$(date +%Y%m%d_%H%M%S)
JSON_OUT="tests/benchmark_history/bench_aggressive_${TS}.json"

cat > "$JSON_OUT" <<ENDJSON
{
  "benchmark": "aggressive",
  "date": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "jash_bin": "$JASH",
  "runs": $RUNS,
  "warmup": $WARMUP,
  "results": {
    "strcat1000":  { "jash": $j_sc, "bash": $b_sc, "zsh": ${z_sc:-null} },
    "funcall1000": { "jash": $j_fn, "bash": $b_fn, "zsh": ${z_fn:-null} },
    "glob_expand": { "jash": $j_gl, "bash": $b_gl, "zsh": ${z_gl:-null} },
    "procsub":     { "jash": $j_ps, "bash": $b_ps, "zsh": ${z_ps:-null} },
    "nested_sub":  { "jash": $j_ns, "bash": $b_ns, "zsh": ${z_ns:-null} },
    "fib18":       { "jash": $j_fb, "bash": $b_fb, "zsh": ${z_fb:-null} },
    "bc_arith100": { "jash": $j_bc, "bash": $b_bc, "zsh": ${z_bc:-null} },
    "array1000":   { "jash": $j_ar, "bash": $b_ar, "zsh": ${z_ar:-null} },
    "bgjob10":     { "jash": $j_bg, "bash": $b_bg, "zsh": ${z_bg:-null} },
    "pipe10k":     { "jash": $j_pt, "bash": $b_pt, "zsh": ${z_pt:-null} }
  },
  "jash_exclusive": {
    "native_ls_100":   $j_nat,
    "external_ls_100": $j_ext,
    "builtin_speedup": $(awk "BEGIN{printf \"%.2f\", $j_ext/$j_nat}")
  },
  "fish_native": {
    "strcat1000":  ${f_sc:-null},
    "funcall1000": ${f_fn:-null},
    "pipe10k":     ${f_pt:-null}
  }
}
ENDJSON

printf "\n${grn}✓${rst} JSON: $JSON_OUT\n\n"
