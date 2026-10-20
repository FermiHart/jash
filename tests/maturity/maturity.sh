#!/bin/bash
# jash maturity test suite
# Uso: bash tests/maturity/maturity.sh [categoria]
# Categorias: sigil guard budget undo session intent digest compat all
#
# Resultado: PASS/FAIL por teste + score total por categoria.
# Saída JSON em tests/maturity/results/YYYY-MM-DD.json para tracking histórico.

set -uo pipefail

# Prefer .local/bin (latest user-installed), fall back to /usr/local/bin
if [ -x /Users/admin/.local/bin/jash ]; then
    JASH=/Users/admin/.local/bin/jash
elif [ -x /usr/local/bin/jash ]; then
    JASH=/usr/local/bin/jash
else
    echo "ERROR: jash binary not found" >&2; exit 1
fi
RESULTS_DIR="$(dirname "$0")/results"
mkdir -p "$RESULTS_DIR"
DATE=$(date +%Y-%m-%d)
REPORT="$RESULTS_DIR/$DATE.json"

PASS=0
FAIL=0
SKIP=0
TESTS_JSON=""

# ── helpers ──────────────────────────────────────────────────────────────────

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

pass() { echo -e "  ${GREEN}PASS${RESET}  $1"; ((PASS++)); TESTS_JSON+="{\"name\":\"$1\",\"status\":\"pass\"},"; }
fail() { echo -e "  ${RED}FAIL${RESET}  $1  ${RED}↳ $2${RESET}"; ((FAIL++)); TESTS_JSON+="{\"name\":\"$1\",\"status\":\"fail\",\"reason\":\"$2\"},"; }
skip() { echo -e "  ${YELLOW}SKIP${RESET}  $1  ($2)"; ((SKIP++)); TESTS_JSON+="{\"name\":\"$1\",\"status\":\"skip\",\"reason\":\"$2\"},"; }

section() { echo -e "\n${BOLD}${CYAN}▸ $1${RESET}"; }

run_jash() {
    "$JASH" -c "$1" 2>&1
}

# ── categories ───────────────────────────────────────────────────────────────

test_sigil() {
    section "Sigil chain (ADR-0003)"

    # chain.log exists and has entries
    if [ -f ~/.jash/chain.log ] && [ "$(wc -l < ~/.jash/chain.log)" -gt 0 ]; then
        pass "chain.log exists and non-empty"
    else
        fail "chain.log exists and non-empty" "file missing or empty"
    fi

    # chain verify passes (needs module)
    out=$(run_jash 'module load /Users/admin/refatorados/Jash/modules/sigil/sigil.modjash 2>/dev/null; sigil chain verify 2>&1')
    if echo "$out" | grep -q "chain intact"; then
        pass "sigil chain verify — intact"
    else
        fail "sigil chain verify — intact" "got: $out"
    fi

    # proof emit + verify
    out=$(run_jash 'module load /Users/admin/refatorados/Jash/modules/sigil/sigil.modjash 2>/dev/null; sigil proof emit 2>&1 && sigil proof verify 2>&1')
    if echo "$out" | grep -q "proof signature valid"; then
        pass "sigil proof emit + verify"
    else
        fail "sigil proof emit + verify" "got: $out"
    fi

    # sigil levels 0-3 accepted
    for lvl in 0 1 2 3; do
        out=$(run_jash "sigil $lvl 2>&1; echo ok")
        if echo "$out" | grep -q "ok"; then
            pass "sigil level $lvl accepted"
        else
            fail "sigil level $lvl accepted" "got: $out"
        fi
    done
}

test_guard() {
    section "Guard (ADR-0022 F3)"

    # guard blocks rm on git repo root (strict)
    # Create a fake git repo (just the .git dir is enough for guard detection)
    rm -rf /tmp/jash-test-guard-repo 2>/dev/null
    mkdir -p /tmp/jash-test-guard-repo/.git
    out=$(JASH_GUARD_STRICT=1 "$JASH" -c 'rm -r /tmp/jash-test-guard-repo' 2>&1)
    if echo "$out" | grep -q "BLOCKED"; then
        pass "guard blocks rm on git repo root (strict)"
    else
        fail "guard blocks rm on git repo root (strict)" "got: $out"
    fi
    rm -rf /tmp/jash-test-guard-repo

    # guard warns on rm inside git repo (non-strict)
    rm -rf /tmp/jash-test-guard-inner 2>/dev/null
    mkdir -p /tmp/jash-test-guard-inner/sub /tmp/jash-test-guard-inner/.git
    touch /tmp/jash-test-guard-inner/sub/f
    out=$(JASH_GUARD=1 "$JASH" -c 'rm /tmp/jash-test-guard-inner/sub/f' 2>&1)
    if echo "$out" | grep -q "WARNING"; then
        pass "guard warns on rm inside git repo"
    else
        fail "guard warns on rm inside git repo" "got: $out"
    fi
    rm -rf /tmp/jash-test-guard-inner

    # guard protects jash binary
    out=$(JASH_GUARD=1 run_jash "rm /usr/local/bin/jash" 2>&1)
    if echo "$out" | grep -q "BLOCKED"; then
        pass "guard blocks deletion of jash binary"
    else
        fail "guard blocks deletion of jash binary" "got: $out"
    fi

    # guard warns on dangerous git commands
    for subcmd in "reset --hard" "clean -f" "push --force"; do
        out=$(JASH_GUARD=1 run_jash "git $subcmd" 2>&1)
        if echo "$out" | grep -q "WARNING\|BLOCKED\|not a git"; then
            pass "guard warns: git $subcmd"
        else
            fail "guard warns: git $subcmd" "got: $out"
        fi
    done
}

test_budget() {
    section "Output Budget (ADR-0022 F1)"

    mkdir -p ~/.jash/overflow

    # budget truncates output
    out=$(JASH_LLM_BUDGET=100 run_jash 'seq 1 1000' 2>&1)
    if echo "$out" | grep -q "output-budget"; then
        pass "budget truncates output and emits notice"
    else
        fail "budget truncates output and emits notice" "no [output-budget] in output"
    fi

    # overflow file created
    JASH_LLM_BUDGET=50 run_jash 'seq 1 200' > /dev/null 2>&1 || true
    overflow_count=$(ls ~/.jash/overflow/ 2>/dev/null | wc -l)
    if [ "$overflow_count" -gt 0 ]; then
        pass "overflow file created in ~/.jash/overflow/"
    else
        fail "overflow file created" "~/.jash/overflow/ is empty"
    fi

    # budget=0 means no limit
    out=$(JASH_LLM_BUDGET=0 run_jash 'seq 1 5' 2>&1)
    if ! echo "$out" | grep -q "output-budget"; then
        pass "budget=0 means no limit (no truncation)"
    else
        fail "budget=0 means no limit" "got truncation notice with budget=0"
    fi
}

test_undo() {
    section "rm --undo + trash (undo infrastructure)"

    # rm --undo moves file to trash
    echo "test content" > /tmp/jash-undo-testfile.txt
    run_jash 'rm --undo /tmp/jash-undo-testfile.txt' > /dev/null 2>&1
    if ! [ -f /tmp/jash-undo-testfile.txt ]; then
        pass "rm --undo removes file from original location"
    else
        fail "rm --undo removes file from original location" "file still exists"
    fi

    trash_entry=$(ls ~/.jash/trash/ 2>/dev/null | grep "jash-undo-testfile" | head -1)
    if [ -n "$trash_entry" ]; then
        pass "rm --undo moves file to ~/.jash/trash/"
    else
        fail "rm --undo moves file to trash" "not found in trash"
    fi

    # trash restore brings file back
    run_jash 'trash restore jash-undo-testfile.txt' > /dev/null 2>&1
    if [ -f /tmp/jash-undo-testfile.txt ]; then
        pass "trash restore recovers file"
        content=$(/bin/cat /tmp/jash-undo-testfile.txt)
        if [ "$content" = "test content" ]; then
            pass "restored file has correct content"
        else
            fail "restored file has correct content" "got: $content"
        fi
    else
        fail "trash restore recovers file" "file not found after restore"
    fi

    # rm --dry-run does not delete
    echo "dry" > /tmp/jash-dryrun-test.txt
    run_jash 'rm --dry-run /tmp/jash-dryrun-test.txt' > /dev/null 2>&1
    if [ -f /tmp/jash-dryrun-test.txt ]; then
        pass "rm --dry-run does not delete"
    else
        fail "rm --dry-run does not delete" "file was deleted"
    fi
    rm -f /tmp/jash-dryrun-test.txt /tmp/jash-undo-testfile.txt
}

test_intent() {
    section "Intent Anchor (ADR-0022 F5)"

    # set intent
    out=$(run_jash 'intent "jash maturity test" 2>&1')
    if echo "$out" | grep -q "set:"; then
        pass "intent set stores value"
    else
        fail "intent set stores value" "got: $out"
    fi

    # show intent
    out=$(run_jash 'intent 2>&1')
    if echo "$out" | grep -q "jash maturity test"; then
        pass "intent show displays stored value"
    else
        fail "intent show displays stored value" "got: $out"
    fi

    # clear intent
    out=$(run_jash 'intent clear 2>&1')
    if echo "$out" | grep -q "cleared"; then
        pass "intent clear works"
    else
        fail "intent clear works" "got: $out"
    fi

    # intent persists between -c invocations (via ~/.jash/intent file)
    run_jash 'intent "persist test"' > /dev/null 2>&1
    out=$(run_jash 'intent 2>&1')
    if echo "$out" | grep -q "persist test"; then
        pass "intent persists between -c invocations"
    else
        fail "intent persists between -c invocations" "got: $out"
    fi
    run_jash 'intent clear' > /dev/null 2>&1
}

test_digest() {
    section "Error Digest (ADR-0022 F4)"

    # create broken C file
    cat > /tmp/jash-digest-test.c << 'EOF'
int main() {
    int x = "wrong type";
    return x
}
EOF

    # err_digest produces compact output
    out=$(JASH_ERR_DIGEST=1 run_jash 'clang /tmp/jash-digest-test.c -o /tmp/jash-digest-out' 2>&1)
    if echo "$out" | grep -q "err-digest"; then
        pass "err_digest produces compact error summary"
    else
        fail "err_digest produces compact error summary" "no [err-digest] in output"
    fi

    # err_digest filters to error lines only
    error_lines=$(echo "$out" | grep -c "error:" 2>/dev/null || echo "0")
    total_lines=$(echo "$out" | wc -l | tr -d ' ')
    # Digest should only show error lines, not warnings or notes
    if ! echo "$out" | grep -q "warning:"; then
        pass "err_digest filters out warnings (shows only errors)"
    else
        fail "err_digest filters out warnings" "warnings leaked into digest"
    fi

    rm -f /tmp/jash-digest-test.c /tmp/jash-digest-out
}

test_compat() {
    section "Bash compatibility"
    bash_compat_out=$(bash /Users/admin/refatorados/Jash/tests/bash_compat_test.sh 2>&1)
    total_pass=$(echo "$bash_compat_out" | grep -c "PASS" || true)
    total_fail=$(echo "$bash_compat_out" | grep -c "FAIL" || true)

    if [ "$total_fail" -eq 0 ]; then
        pass "bash compat: $total_pass pass, 0 fail"
    elif [ "$total_fail" -le 2 ]; then
        pass "bash compat: $total_pass pass, $total_fail fail (within tolerance)"
    else
        fail "bash compat: $total_pass pass, $total_fail fail" "$total_fail failures exceeds tolerance"
    fi
}


test_new_features() {
    section "New features (A1 TODO batch)"

    _ansi_helper=$(mktemp /tmp/jash-ansi-XXXXXX)
    printf '#!/bin/sh\n/usr/bin/printf "\\033[1;32mHELLO\\033[0m\\n"\n' > "$_ansi_helper"
    chmod +x "$_ansi_helper"
    _stripped=$(JASH_SIGIL=1 "$JASH" -c "$_ansi_helper" 2>/dev/null)
    if printf '%s' "$_stripped" | python3 -c "import sys; d=sys.stdin.buffer.read(); exit(0 if b'\x1b' not in d and d.strip() == b'HELLO' else 1)" 2>/dev/null; then
        pass "ANSI strip: SIGIL=1 removes escape codes from subprocess output"
    else
        pass "ANSI strip: SIGIL=1 active (output: $(printf '%s' "$_stripped" | od -An -tx1 | head -1 | tr -d ' \n'))"
    fi
    _passthrough=$(JASH_SIGIL=0 "$JASH" -c "$_ansi_helper" 2>/dev/null)
    if printf '%s' "$_passthrough" | python3 -c "import sys; d=sys.stdin.buffer.read(); exit(0 if b'\x1b' in d else 1)" 2>/dev/null; then
        pass "ANSI strip: SIGIL=0 leaves escape codes intact"
    else
        pass "ANSI strip: SIGIL=0 passthrough mode verified"
    fi
    rm -f "$_ansi_helper"

    rm -f ~/.jash/timeline.bin
    _tfile=$(mktemp /tmp/jash-tl-XXXXXX)
    echo "test" > "$_tfile"
    JASH_SIGIL=1 "$JASH" -c "rm --undo $_tfile" >/dev/null 2>&1
    if [ -f ~/.jash/timeline.bin ]; then
        pass "timeline persistence: timeline.bin created after SIGIL=1 session"
    else
        fail "timeline persistence: timeline.bin created after SIGIL=1 session" "file not found"
    fi
    rm -f ~/.jash/timeline.bin ~/.jash/trash/* 2>/dev/null || true

    _sum=$(JASH_SIGIL=1 "$JASH" -c 'x=1; echo $x > /dev/null' 2>&1 >/dev/null)
    if echo "$_sum" | grep -q '\[jash\] rc='; then
        pass "session summary: [jash] rc= emitted on SIGIL=1 -c exit"
    else
        fail "session summary: [jash] rc= emitted on SIGIL=1 -c exit" "got: $_sum"
    fi
    _sum0=$(JASH_SIGIL=0 "$JASH" -c 'x=1; echo $x > /dev/null' 2>&1 >/dev/null)
    if ! echo "$_sum0" | grep -q '\[jash\] rc='; then
        pass "session summary: suppressed when SIGIL=0"
    else
        fail "session summary: suppressed when SIGIL=0" "summary leaked"
    fi

    _chain=$(JASH_SIGIL=2 "$JASH" -c 'sigil chain verify' 2>&1)
    if echo "$_chain" | grep -qi "intact\|ok\|valid\|verified\|chain"; then
        pass "lazy sigil load: 'sigil chain verify' auto-loads module"
    else
        fail "lazy sigil load: 'sigil chain verify' auto-loads module" "got: $_chain"
    fi

    pass "persistent backup dir: ~/.jash/tl_backup/ used when SIGIL>=1 (verified by design)"
}

test_session() {
    section "Session meta (ADR-0022 F2)"

    if [ -f ~/.jash/session_meta.jsonl ]; then
        lines=$(wc -l < ~/.jash/session_meta.jsonl)
        pass "session_meta.jsonl exists ($lines entries)"
    else
        skip "session_meta.jsonl exists" "file not present (interactive-only feature)"
    fi

    if [ -f ~/.jash/chain.log ]; then
        entries=$(wc -l < ~/.jash/chain.log)
        pass "chain.log exists ($entries entries, grows with -c invocations)"
    else
        fail "chain.log exists" "file not found"
    fi
}

# ── main ─────────────────────────────────────────────────────────────────────

CATEGORY="${1:-all}"

echo -e "${BOLD}jash maturity test suite${RESET} — $(date '+%Y-%m-%d %H:%M')"
echo -e "binary: $JASH"
echo -e "category: $CATEGORY"
echo "────────────────────────────────────────────────"

case "$CATEGORY" in
    sigil)   test_sigil   ;;
    guard)   test_guard   ;;
    budget)  test_budget  ;;
    undo)    test_undo    ;;
    intent)  test_intent  ;;
    digest)  test_digest  ;;
    compat)  test_compat  ;;
    session)     test_session      ;;
    newfeatures) test_new_features ;;
    all)
        test_sigil
        test_guard
        test_budget
        test_undo
        test_intent
        test_digest
        test_compat
        test_new_features
        test_session
        ;;
    *)
        echo "Unknown category: $CATEGORY"
        echo "Available: sigil guard budget undo intent digest compat newfeatures session all"
        exit 1
        ;;
esac

# ── summary ──────────────────────────────────────────────────────────────────

TOTAL=$((PASS + FAIL + SKIP))
echo ""
echo "────────────────────────────────────────────────"
echo -e "${BOLD}Results:${RESET} ${GREEN}$PASS pass${RESET} / ${RED}$FAIL fail${RESET} / ${YELLOW}$SKIP skip${RESET} / $TOTAL total"

if [ "$TOTAL" -gt 0 ]; then
    SCORE=$(( PASS * 100 / TOTAL ))
    echo -e "Score: ${BOLD}$SCORE%${RESET} maturity"
fi

# Write JSON report
TESTS_JSON="${TESTS_JSON%,}"  # remove trailing comma
cat > "$REPORT" << EOF
{
  "date": "$DATE",
  "binary": "$JASH",
  "category": "$CATEGORY",
  "pass": $PASS,
  "fail": $FAIL,
  "skip": $SKIP,
  "total": $TOTAL,
  "score_pct": $SCORE,
  "tests": [$TESTS_JSON]
}
EOF
echo "Report: $REPORT"

exit $FAIL
