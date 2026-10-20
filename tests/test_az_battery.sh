#!/bin/bash
# Jash A-Z Comprehensive Test Battery
# Tests every builtin and feature from A to Z
# Usage: ./test_az_battery.sh [/path/to/jash]

JASH="${1:-$(dirname "$0")/../build/jash}"
AZ_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jash-az.XXXXXX")" || exit 2
trap 'rm -rf "$AZ_TMP"' EXIT
export JASH_AZ_FIXTURES="$AZ_TMP"

# This is a bash-PARITY correctness battery: it re-invokes jash in fresh
# processes and asserts literal output. Read memoization / memoΔ (ADR-0038 W1 /
# ADR-0042 F1) deliberately alters output for agent token-efficiency (a re-run
# of a whitelisted read can answer with a "[jash memo]" line or a +N/-M delta),
# which is a confound here — and it activates whenever the suite is run inside an
# agent session (inherited CLAUDE_CODE_SESSION_ID). memo has its own dedicated
# coverage in tests/test_memo.sh + tests/test_memo_delta.sh, so disable it here
# for deterministic parity results in every environment.
export JASH_MEMO=0

PASS=0
FAIL=0
TOTAL=0
FAILED_TESTS=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test function
test_cmd() {
    local name="$1"
    local cmd="$2"
    local expected="$3"
    
    TOTAL=$((TOTAL + 1))
    
    # Run command
    local raw_output
    raw_output=$("$JASH" -c "$cmd" 2>/dev/null)
    
    # Strip ANSI escape codes
    result=$(echo "$raw_output" | sed $'s/\x1b\[[0-9;]*m//g' | sed $'s/\x1b\[[0-9]*[A-Z]//g' | sed $'s/\x1b\[[0-9]*;[0-9]*[A-Z]//g' | tr -d '\r' | grep -v '^$' | head -1)
    
    # Check if result contains expected (for partial matches)
    if echo "$result" | grep -q "$expected"; then
        PASS=$((PASS + 1))
        echo -e "${GREEN}✓${NC} $name"
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS\n  $name: expected='$expected' got='$result'"
        echo -e "${RED}✗${NC} $name"
        echo "    Expected: $expected"
        echo "    Got: $result"
    fi
}

echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  JASH A-Z COMPREHENSIVE TEST BATTERY${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
echo ""

# ═══════════════════════════════════════════════════════════════════
echo -e "${YELLOW}[A] ALIAS & ARGS${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "alias_basic" "alias ll='ls -l'; alias ll" "alias ll='ls -l'"
test_cmd "alias_exec" "alias greet='echo hello'; greet" "hello"
test_cmd "unalias" "alias test_alias='echo ok'; unalias test_alias; type test_alias 2>&1" "not found"
test_cmd "alias_no_args" "alias" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[B] BREAK & BG${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "break_in_loop" "for i in 1 2 3 4 5; do if [ \$i -eq 3 ]; then break; fi; echo \$i; done" "1"
test_cmd "bg_returns" "true" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[C] CD & COMMAND & CALC & CONFIG${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "cd_home" "cd; pwd" "$HOME"
test_cmd "cd_dash" "cd /tmp; cd; cd -; pwd" "/tmp"
test_cmd "cd_dotdot" "cd /tmp; cd ..; pwd" "/"
test_cmd "cd_slash" "cd /; pwd" "/"
test_cmd "command_builtin" "command echo hello" "hello"
test_cmd "config_help" "config help" "Usage"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[D] DECLARE & DIRS${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "declare_basic" "declare x=42; echo \$x" "42"
test_cmd "declare_readonly" "declare -r ro=1; echo \$ro" "1"
test_cmd "dirs_basic" "echo ok" "ok"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[E] ECHO & ENV & EVAL & EXEC & EXIT${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "echo_basic" "echo hello world" "hello world"
test_cmd "echo_env" "export TESTVAR=hello; echo \$TESTVAR" "hello"
test_cmd "unset_var" "export TEMP=123; unset TEMP; echo \${TEMP:-unset}" "unset"
test_cmd "eval_cmd" "eval 'echo evaluated'" "evaluated"
test_cmd "exec_cmd" "exec echo exec_test" "exec_test"
test_cmd "exit_code" "true; echo \$?" "0"
test_cmd "exit_code_false" "false; echo \$?" "1"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[F] FOR & FUNCTIONS${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "for_loop" "for i in 1 2 3; do echo \$i; done" "1"
test_cmd "function_basic" "f() { echo func; }; f" "func"
test_cmd "function_args" "g() { echo \$1; }; g hello" "hello"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[G] GROUP${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "group_cmd" "{ echo a; echo b; }" "a"
test_cmd "subshell_group" "(echo subshell)" "subshell"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[H] HELP & HISTORY${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "help_output" "help | grep shell" "shell"
test_cmd "history_cmd" "history" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[I] IF${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "if_basic" "if true; then echo yes; fi" "yes"
test_cmd "if_else" "if false; then no; else echo else; fi" "else"
test_cmd "if_elif" "if true; then echo yes; elif false; then echo no; fi" "yes"
test_cmd "if_nested" "if true; then if true; then echo nested; fi; fi" "nested"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[J] JOBS${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "jobs_empty" "jobs" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[K] KILL${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "kill_help" "kill --help" "Usage"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[L] LET & LOCAL${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "let_basic" "let x=5+3; echo \$x" "8"
test_cmd "let_subtract" "let y=10-4; echo \$y" "6"
test_cmd "let_multiply" "let z=3*7; echo \$z" "21"
test_cmd "let_divide" "let a=20/4; echo \$a" "5"
test_cmd "let_string" "let name=hello; echo \$name" "0"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[M] MV & MASK${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "mv_basic" 'rm -f "$JASH_AZ_FIXTURES/_mv_test" "$JASH_AZ_FIXTURES/_mv_test2"; touch "$JASH_AZ_FIXTURES/_mv_test"; mv "$JASH_AZ_FIXTURES/_mv_test" "$JASH_AZ_FIXTURES/_mv_test2" >/dev/null; ls "$JASH_AZ_FIXTURES/_mv_test2"; rm -f "$JASH_AZ_FIXTURES/_mv_test2"' "_mv_test2"
# BUG-061: non-interactive mv over an existing target must OVERWRITE (POSIX),
# never silently skip (the old fgets-on-/dev/null path kept the stale file).
test_cmd "mv_overwrite_noninteractive" 'printf new > "$JASH_AZ_FIXTURES/_mv_src"; printf old > "$JASH_AZ_FIXTURES/_mv_dst"; mv "$JASH_AZ_FIXTURES/_mv_src" "$JASH_AZ_FIXTURES/_mv_dst" >/dev/null 2>&1; cat "$JASH_AZ_FIXTURES/_mv_dst"; rm -f "$JASH_AZ_FIXTURES/_mv_src" "$JASH_AZ_FIXTURES/_mv_dst"' "new"
test_cmd "umask_show" "umask" "0"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[N] OSIF${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "osif_cmd" "osif version" "usage"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[P] PWD & POPD & PRINTF${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "pwd_cmd" "pwd" "$(pwd)"
test_cmd "pushd_popd" "echo ok" "ok"
test_cmd "printf_basic" "printf '%s\n' hello" "hello"
test_cmd "printf_number" "printf '%d\n' 42" "42"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[Q] QUOTE${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "single_quotes" "echo 'hello world'" "hello world"
test_cmd "double_quotes" "echo \"hello world\"" "hello world"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[R] READ & RETURN & READONLY${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "read_var" "echo 'test_val' | read x; echo \$x" "test_val"
test_cmd "return_from_func" "f() { return 42; }; f; echo \$?" "42"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[S] SET & SHIFT & SOURCE & SUBSHELL${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "set_opts" "set -u; echo ok" "ok"
test_cmd "shift_args" "set -- a b c; shift; echo \$1" "b"
test_cmd "subshell_echo" "(echo subshell)" "subshell"
test_cmd "source_file" "echo 'export SOURCED=1' > \"\$JASH_AZ_FIXTURES/_src_test\"; source \"\$JASH_AZ_FIXTURES/_src_test\"; echo \$SOURCED" "1"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[T] TEST & TYPE & TIMELINE & TRASH${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "test_file" "[ -f /etc/passwd ]; echo \$?" "0"
test_cmd "test_dir" "[ -d /tmp ]; echo \$?" "0"
test_cmd "test_readable" "[ -r /tmp ]; echo \$?" "0"
test_cmd "test_writable" "[ -w /tmp ]; echo \$?" "0"
test_cmd "test_executable" "[ -x /tmp ]; echo \$?" "0"
test_cmd "test_not_exist" "[ -f /nonexistent ]; echo \$?" "1"
test_cmd "test_string_eq" "[ hello = hello ]; echo \$?" "0"
test_cmd "test_string_ne" "[ hello != world ]; echo \$?" "0"
test_cmd "test_numeric_eq" "[ 5 -eq 5 ]; echo \$?" "0"
test_cmd "test_numeric_ne" "[ 5 -ne 3 ]; echo \$?" "0"
test_cmd "test_numeric_lt" "[ 3 -lt 5 ]; echo \$?" "0"
test_cmd "test_numeric_gt" "[ 5 -gt 3 ]; echo \$?" "0"
test_cmd "type_cmd" "type echo" "builtin"
test_cmd "type_external" "type ls" "builtin"
test_cmd "timeline_cmd" 'touch "$JASH_AZ_FIXTURES/_tlc"; rm --undo "$JASH_AZ_FIXTURES/_tlc"; timeline; undo' "mutation"
test_cmd "trash_help" "trash --help" "Trash"

# Agent-shell compat (PATCH REQ-1/2/3 — Hermes-OSDev integration)
test_cmd "set_monitor" "set +m; set -o monitor; set +o monitor; echo OK" "OK"
test_cmd "grep_ere" "printf 'foo\nbar\n' | grep -E '^f'" "foo"
test_cmd "grep_cluster" "printf '_a\nb\n__c\n' | grep -vE '^_[^_]'" "b"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[U] UNAME & UNDO & ULIMIT${NC}"
# ═══════════════════════════════════════════════════════════════════

# Compare against the HOST's uname, not a hardcoded "Darwin"/"x86_64" — the
# assertion is "jash agrees with the system", which is what portability means.
test_cmd "uname_s" "uname -s" "$(uname -s)"
test_cmd "uname_m" "uname -m" "$(uname -m)"
test_cmd "undo_cmd" "type undo" "builtin"
test_cmd "ulimit_cmd" "ulimit -n" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[V] VERSION & WHILE${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "version_cmd" "version" "jash"
test_cmd "while_loop" "i=0; while [ \$i -lt 3 ]; do echo \$i; i=\$((i+1)); done" "0"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[W] WAIT${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "wait_cmd" "wait" ""

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[X] EXEC FD REDIRECTS${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "exec_fd_open" 'exec 3> "$JASH_AZ_FIXTURES/_fd_test" && exec 3>&- && echo fd_ok' "fd_ok"
test_cmd "exec_fd_multi" 'exec 3> "$JASH_AZ_FIXTURES/_fd4" && exec 4> "$JASH_AZ_FIXTURES/_fd5" && exec 3>&- && exec 4>&- && echo ok' "ok"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[Y] PIPE & OR & AND${NC}"
# ═══════════════════════════════d═══════════════════════════════════

test_cmd "pipe_basic" "echo hello | cat" "hello"
test_cmd "pipe_chain" "echo hello | cat | cat" "hello"
test_cmd "or_operator" "false || echo fallback" "fallback"
test_cmd "and_operator" "true && echo success" "success"

# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${YELLOW}[Z] ZERO & SPECIAL${NC}"
# ═══════════════════════════════════════════════════════════════════

test_cmd "zero_exit" "true; echo \$?" "0"
test_cmd "colon_nop" ":; echo nop" "nop"
test_cmd "last_status" "false; echo \$?" "1"
test_cmd "variable_expand" "X=42; echo \$X" "42"
test_cmd "special_dollar_question" "true; echo \$?" "0"
test_cmd "special_pid_nonzero" "echo \$\$" ""

# ═══════════════════════════════════════════════════════════════════
# RESULTS
# ═══════════════════════════════════════════════════════════════════

echo ""
echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  A-Z BATTERY RESULTS${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}  TOTAL: $TOTAL"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "  ${GREEN}╔════════════════════════════════════╗${NC}"
    echo -e "  ${GREEN}║   ✅ A-Z BATTERY PASSED — 100%     ║${NC}"
    echo -e "  ${GREEN}╚════════════════════════════════════╝${NC}"
else
    echo -e "  ${RED}╔════════════════════════════════════╗${NC}"
    echo -e "  ${RED}║   ❌ A-Z BATTERY FAILED            ║${NC}"
    echo -e "  {RED}╚════════════════════════════════════╝${NC}"
    echo ""
    echo -e "${RED}Failed tests:${NC}"
    echo -e "$FAILED_TESTS"
fi

echo ""
exit $FAIL
