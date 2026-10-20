#!/bin/bash
JASH=./build/jash

echo "=== Detailed Performance Profile ==="
echo ""

# Profile com JASH_BENCH
echo "1. Startup breakdown (JASH_BENCH=1):"
JASH_BENCH=1 $JASH -c "exit" 2>&1

echo ""
echo "2. Memory allocation profile:"
# Verificar alocações
echo "  State size: $(ls -lh $JASH | awk '{print $5}')"
echo "  Rust lib: $(ls -lh rust/target/release/libbearjash.a 2>/dev/null | awk '{print $5}' || echo 'N/A')"

echo ""
echo "3. Syscall count (comparativo):"
echo "  Jash:"
/usr/bin/time -l $JASH -c "exit" 2>&1 | grep -E "real|user|sys|maximum"

echo ""
echo "  Bash:"
/usr/bin/time -l bash -c "exit" 2>&1 | grep -E "real|user|sys|maximum"

echo ""
echo "4. Binary composition:"
size $JASH 2>/dev/null || echo "size command not available"

echo ""
echo "5. Link dependencies:"
otool -L $JASH 2>/dev/null | head -10 || ldd $JASH 2>/dev/null | head -10 || echo "N/A"
