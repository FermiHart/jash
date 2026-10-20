#!/bin/bash
# ============================================================================
# JASH PROFESSIONAL BENCHMARK SUITE - BEFORE IMPLEMENTATION
# ============================================================================
# Shells: bash, zsh, dash, jash
# Metrics: Performance, Memory, Binary Size, Compatibility
# ============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Output file
OUTPUT_FILE="benchmark_results_before.txt"

log() {
    echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $1" | tee -a "$OUTPUT_FILE"
}

separator() {
    echo "================================================================================" | tee -a "$OUTPUT_FILE"
}

# Clear output file
> "$OUTPUT_FILE"

log "JASH PROFESSIONAL BENCHMARK SUITE - BEFORE PREDICTIVE EXECUTION"
log "Date: $(date)"
log "System: $(uname -srm)"
separator

# ============================================================================
# 1. BINARY SIZE
# ============================================================================
log "1. BINARY SIZE (stripped)"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    size=$(strip -o /tmp/${shell}_stripped "$binary" 2>/dev/null && ls -lh /tmp/${shell}_stripped | awk '{print $5}')
    log "$shell: $size"
done

separator

# ============================================================================
# 2. STARTUP TIME (100 runs each)
# ============================================================================
log "2. STARTUP TIME (100 runs, -c 'exit')"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    for i in {1..100}; do
        "$binary" -c 'exit' 2>/dev/null || true
    done
    end=$(date +%s%N)
    
    total_ms=$(( (end - start) / 1000000 ))
    avg_ms=$(( total_ms / 100 ))
    
    log "$shell: ${avg_ms}ms avg (${total_ms}ms total for 100 runs)"
done

separator

# ============================================================================
# 3. SIMPLE COMMAND EXECUTION
# ============================================================================
log "3. SIMPLE COMMAND (100x 'echo hello')"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    for i in {1..100}; do
        "$binary" -c 'echo hello' >/dev/null 2>&1 || true
    done
    end=$(date +%s%N)
    
    total_ms=$(( (end - start) / 1000000 ))
    avg_ms=$(( total_ms / 100 ))
    
    log "$shell: ${avg_ms}ms avg"
done

separator

# ============================================================================
# 4. COMMAND SUBSTITUTION (cache effectiveness)
# ============================================================================
log "4. COMMAND SUBSTITUTION (100x 'echo \$(whoami)')"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    for i in {1..100}; do
        "$binary" -c 'echo $(whoami)' >/dev/null 2>&1 || true
    done
    end=$(date +%s%N)
    
    total_ms=$(( (end - start) / 1000000 ))
    avg_ms=$(( total_ms / 100 ))
    
    log "$shell: ${avg_ms}ms avg"
done

separator

# ============================================================================
# 5. ARITHMETIC LOOP (10K iterations)
# ============================================================================
log "5. ARITHMETIC LOOP (10K iterations)"
separator

cat > /tmp/arith_test.sh << 'EOF'
x=0
for i in $(seq 1 10000); do
    x=$((x + i))
done
echo $x
EOF

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    result=$("$binary" /tmp/arith_test.sh 2>/dev/null || echo "ERROR")
    end=$(date +%s%N)
    
    elapsed_ms=$(( (end - start) / 1000000 ))
    
    if [ "$result" = "50005000" ]; then
        log "$shell: ${elapsed_ms}ms (correct: $result)"
    else
        log "$shell: ${elapsed_ms}ms (WRONG: $result, expected 50005000)"
    fi
done

separator

# ============================================================================
# 6. RECURSIVE FIBONACCI (fib(20))
# ============================================================================
log "6. RECURSIVE FIBONACCI (fib(20))"
separator

cat > /tmp/fib_test.sh << 'EOF'
fib() {
    if [ $1 -le 1 ]; then
        echo $1
    else
        a=$(fib $(($1 - 1)))
        b=$(fib $(($1 - 2)))
        echo $(($a + $b))
    fi
}
fib 20
EOF

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    result=$("$binary" /tmp/fib_test.sh 2>/dev/null || echo "ERROR")
    end=$(date +%s%N)
    
    elapsed_ms=$(( (end - start) / 1000000 ))
    
    if [ "$result" = "6765" ]; then
        log "$shell: ${elapsed_ms}ms (correct: $result)"
    else
        log "$shell: ${elapsed_ms}ms (WRONG: $result, expected 6765)"
    fi
done

separator

# ============================================================================
# 7. FILE OPERATIONS (create 100 files, ls, delete)
# ============================================================================
log "7. FILE OPERATIONS (100 files: create, ls, delete)"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    cat > /tmp/file_ops.sh << 'EOF'
mkdir -p /tmp/test_dir_$$
for i in $(seq 1 100); do
    touch /tmp/test_dir_$$/file_$i
done
ls /tmp/test_dir_$$ | wc -l
rm -rf /tmp/test_dir_$$
EOF
    
    start=$(date +%s%N)
    result=$("$binary" /tmp/file_ops.sh 2>/dev/null || echo "ERROR")
    end=$(date +%s%N)
    
    elapsed_ms=$(( (end - start) / 1000000 ))
    
    if [ "$result" = "100" ]; then
        log "$shell: ${elapsed_ms}ms (correct: $result files)"
    else
        log "$shell: ${elapsed_ms}ms (WRONG: $result, expected 100)"
    fi
done

separator

# ============================================================================
# 8. MEMORY USAGE (RSS at startup)
# ============================================================================
log "8. MEMORY USAGE (RSS at startup)"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    # Start shell in background, measure RSS, kill it
    "$binary" -c 'sleep 0.1' &
    pid=$!
    sleep 0.05
    
    if [ -f /proc/$pid/status ]; then
        rss=$(grep VmRSS /proc/$pid/status | awk '{print $2}')
        log "$shell: ${rss} KB RSS"
    else
        # macOS fallback
        rss=$(ps -o rss= -p $pid 2>/dev/null || echo "N/A")
        log "$shell: ${rss} KB RSS"
    fi
    
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
done

separator

# ============================================================================
# 9. PIPELINE PERFORMANCE (10-stage pipeline)
# ============================================================================
log "9. PIPELINE PERFORMANCE (10 stages)"
separator

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    for i in {1..100}; do
        "$binary" -c 'echo test | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat' >/dev/null 2>&1 || true
    done
    end=$(date +%s%N)
    
    total_ms=$(( (end - start) / 1000000 ))
    avg_ms=$(( total_ms / 100 ))
    
    log "$shell: ${avg_ms}ms avg"
done

separator

# ============================================================================
# 10. STRING MANIPULATION (1000 iterations)
# ============================================================================
log "10. STRING MANIPULATION (1000 iterations)"
separator

cat > /tmp/string_test.sh << 'EOF'
str="hello world"
for i in $(seq 1 1000); do
    upper=$(echo "$str" | tr '[:lower:]' '[:upper:]')
    len=${#str}
    sub=${str:0:5}
done
echo "$upper $len $sub"
EOF

for shell in bash zsh dash jash; do
    if [ "$shell" = "jash" ]; then
        binary="./build/jash"
    else
        binary=$(which $shell 2>/dev/null || echo "")
    fi
    
    if [ -z "$binary" ] || [ ! -f "$binary" ]; then
        log "$shell: NOT FOUND"
        continue
    fi
    
    start=$(date +%s%N)
    result=$("$binary" /tmp/string_test.sh 2>/dev/null || echo "ERROR")
    end=$(date +%s%N)
    
    elapsed_ms=$(( (end - start) / 1000000 ))
    
    if [ "$result" = "HELLO WORLD 11 hello" ]; then
        log "$shell: ${elapsed_ms}ms (correct)"
    else
        log "$shell: ${elapsed_ms}ms (WRONG: $result)"
    fi
done

separator

# ============================================================================
# SUMMARY
# ============================================================================
log "BENCHMARK COMPLETE"
log "Results saved to: $OUTPUT_FILE"
separator

echo ""
echo -e "${GREEN}Benchmark complete!${NC}"
echo "Results saved to: $OUTPUT_FILE"
