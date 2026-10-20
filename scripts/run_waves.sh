#!/bin/bash
# Automated Wave Executor - jash Silent Truncation Project
# Runs waves sequentially with quality checks

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAVES_DIR="$SCRIPT_DIR/../docs/Waves"
PATCHES_DIR="$SCRIPT_DIR/../patches-proposal"

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║     Automatic Wave Execution - Silent Truncation Fixes     ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Wave execution function
run_wave() {
    local wave_num=$1
    local wave_name=$2
    local status=$3
    
    echo -e "${BLUE}══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Wave ${wave_num}: ${wave_name} [${status}]${NC}"
    echo -e "${BLUE}══════════════════════════════════════════════════════════════${NC}"
    
    if [ "$status" = "COMPLETED" ]; then
        echo -e "${GREEN}✅ Wave ${wave_num} concluída${NC}"
    elif [ "$status" = "IN_PROGRESS" ]; then
        echo -e "${YELLOW}🔄 Wave ${wave_num} em progresso...${NC}"
        return 0
    else
        echo -e "${RED}❌ Wave ${wave_num} pendente${NC}"
        return 1
    fi
}

# Check prerequisites
check_build() {
    echo "Verificando build..."
    if [ ! -f "./build/jash" ]; then
        echo "Compilando jash..."
        cmake -B build -DCMAKE_BUILD_TYPE=Release
        cmake --build build --target jash
    fi
    echo -e "${GREEN}✅ Build verificado${NC}"
}

check_git() {
    echo "Verificando estado git..."
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        echo -e "${RED}❌ Não é um repositório git${NC}"
        exit 1
    fi
    echo -e "${GREEN}✅ Repositório git está ok${NC}"
}

run_wave 1 "for_loop.c Expr Buffer" COMPLETED
run_wave 2 "eval.c Arithmetic Buffer" PENDING
run_wave 3 "jash_brain Core State" PENDING
run_wave 4 "declare.c Array Entries" PENDING
run_wave 5 "builtin_set Word Splitting" PENDING
run_wave 6 "${#A[@]} Array Count" PENDING
run_wave 7 "mapfile Line Buffer" PENDING
run_wave 8 "cmdsub Cache Deps" PENDING
run_wave 9 "builtin_graph Chain" PENDING
run_wave 10 "Lint Improvements" PENDING

echo ""
echo "══════════════════════════════════════════════════════════════"
echo -e "${BLUE}Next Steps:${NC}"
echo "1. Review Wave 002 (eval.c) - highest exploitability remaining"
echo "2. Run: ./patches-proposal/test_for_expr.sh"
echo "3. Check: patches-proposal/PATCH-002-eval-arith-trunc.patch.md"
echo ""
echo "To execute a specific wave:"
echo "  cd /home/alf/OS/Jash"
echo "  ./build/jash -c 'your_test_command'"
echo ""

echo -e "${GREEN}🚀 Ready for Wave 002 Implementation${NC}"