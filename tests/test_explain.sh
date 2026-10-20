#!/usr/bin/env bash
# test_explain.sh — `explain` blast-radius preview without executing (ADR-0023 E3b).
# Predicts jash-known effects (rm/mv/cp/redirect/network), marks externals opaque,
# and NEVER runs the command. A quoted command line keeps &&/|/; for explain's own
# splitter (otherwise the shell consumes them).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; mkdir -p "$TH/.jash"
ex(){ JASH_SIGIL=0 HOME="$TH" "$J" -c "explain \"$1\"" 2>&1 | strip; }

echo "== explain blast-radius (ADR-0023 E3b) =="

# sentinel: a real file we'd 'delete' must still exist after explain (no execution)
W="$ROOT/.exptest_run"; rm -rf "$W"; mkdir -p "$W"; echo X > "$W/keep.txt"
JASH_SIGIL=0 HOME="$TH" "$J" -c "explain \"rm $W/keep.txt\"" >/dev/null 2>&1
[ -f "$W/keep.txt" ] && ok 'does NOT execute (file survives)' || no 'no-exec' 'file deleted!'

out="$(ex 'rm -rf build')";        printf '%s' "$out" | grep -q 'DELETE'   && ok 'rm → DELETE'        || no 'rm' "$out"
out="$(ex 'mv a b')";              printf '%s' "$out" | grep -q 'MOVE'     && ok 'mv → MOVE'          || no 'mv' "$out"
out="$(ex 'cp a b')";              printf '%s' "$out" | grep -q 'COPY'     && ok 'cp → COPY'          || no 'cp' "$out"
out="$(ex 'cc -o app main.c')";    printf '%s' "$out" | grep -q 'external' && ok 'cc → external'      || no 'cc' "$out"
out="$(ex 'curl http://x')";       printf '%s' "$out" | grep -q 'NETWORK'  && ok 'curl → NETWORK'     || no 'curl' "$out"
out="$(ex 'echo hi > out.txt')";   printf '%s' "$out" | grep -q 'WRITE'    && ok '> → WRITE'          || no 'write' "$out"
out="$(ex 'echo hi >> out.txt')";  printf '%s' "$out" | grep -q 'APPEND'   && ok '>> → APPEND'        || no 'append' "$out"
out="$(ex 'ls -la')";              printf '%s' "$out" | grep -q 'read-only' && ok 'ls → read-only'     || no 'ls' "$out"

# multi-step count
out="$(ex 'rm x && cc y && echo z > w')"
printf '%s' "$out" | grep -q '3 step' && ok 'multi-step split (&&)' || no 'multi-step' "$out"

# trash off vs on changes the delete verdict
JASH_SIGIL=0 HOME="$TH" "$J" -c 'trash off' >/dev/null 2>&1
printf '%s' "$(ex 'rm x')" | grep -q 'PERMANENT' && ok 'trash off → PERMANENT' || no 'perm' "$(ex 'rm x')"
JASH_SIGIL=0 HOME="$TH" "$J" -c 'trash on' >/dev/null 2>&1
printf '%s' "$(ex 'rm x')" | grep -q 'recoverable' && ok 'trash on → recoverable' || no 'recov' "$(ex 'rm x')"

rm -rf "$TH" "$W"
echo "== explain: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
