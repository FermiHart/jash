#!/bin/bash
# ADR-0036 W1 — conformance harness: jash builtins vs the system tools they
# shadow. Compares stdout+rc for the same invocation, run in twin sandboxes
# (mutations in one must never leak into the other's fixture).
#
# A case is one line of shell ARGS (no tool name) in tests/conformance/cases/<tool>.txt.
# The runner prefixes it with the real tool path for the system run and with
# the bare tool name for the jash run (jash builtins shadow lookup).
#
# Documented, deliberate divergences (jash extras, trash-not-delete, etc.) go
# in tests/conformance/cases/<tool>.exceptions, one case-line per line,
# `case :: reason`. An undocumented mismatch fails the suite.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
JASH_BIN="${JASH_BIN:-$REPO/build/jash}"

# macOS ships bash 3.2 (no `declare -A`) — map tool -> system path portably.
tool_path() {
    case "$1" in
        grep)  echo /usr/bin/grep ;;
        cp)    echo /bin/cp ;;
        mv)    echo /bin/mv ;;
        ls)    echo /bin/ls ;;
        touch) echo /usr/bin/touch ;;
        rm)    echo /bin/rm ;;
        *)     echo "" ;;
    esac
}

PASS=0
FAIL=0
SKIP=0

setup_fixture() {
    local dir="$1"
    mkdir -p "$dir"
    printf 'TODO fix\nstrcpy(a,b);\nfoo bar baz\nhello world foo\n' > "$dir/sample.txt"
    printf 'strcpy(a,b);\nmy_strcpy_thing\nvoid f() {}\n' > "$dir/code.c"
    printf 'a b\n' > "$dir/spacefile.txt"
    printf 'move me\n' > "$dir/movable.txt"
    printf 'gone\n' > "$dir/removable.txt"
    mkdir -p "$dir/subdir"
    printf 'nested\n' > "$dir/subdir/inner.txt"
    mkdir -p "$dir/destdir"
}

is_exception() {
    local tool="$1" case_line="$2"
    local exfile="$HERE/cases/$tool.exceptions"
    [ -f "$exfile" ] || return 1
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        local key="${line%% :: *}"
        [ "$key" = "$case_line" ] && return 0
    done < "$exfile"
    return 1
}

run_case() {
    local tool="$1" case_line="$2" tool_bin="$3"
    local sys_dir jash_dir sys_out jash_out sys_rc jash_rc sys_err jash_err
    sys_dir="$(mktemp -d)"; jash_dir="$(mktemp -d)"
    setup_fixture "$sys_dir"
    setup_fixture "$jash_dir"

    # Contract: stdout + rc must match exactly; stderr TEXT may differ (error
    # message wording), so it is captured for debugging only, never compared.
    sys_out="$(cd "$sys_dir" && eval "$tool_bin $case_line" 2>"$sys_dir/.stderr")"
    sys_rc=$?
    sys_err="$(cat "$sys_dir/.stderr" 2>/dev/null)"
    jash_out="$(cd "$jash_dir" && eval "$JASH_BIN -c \"$tool $case_line\"" 2>"$jash_dir/.stderr")"
    jash_rc=$?
    jash_err="$(cat "$jash_dir/.stderr" 2>/dev/null)"

    rm -rf "$sys_dir" "$jash_dir"

    if [ "$sys_out" = "$jash_out" ] && [ "$sys_rc" = "$jash_rc" ]; then
        PASS=$((PASS + 1))
        return 0
    fi

    if is_exception "$tool" "$case_line"; then
        SKIP=$((SKIP + 1))
        return 0
    fi

    FAIL=$((FAIL + 1))
    echo "FAIL [$tool] $case_line"
    echo "  system(rc=$sys_rc) stdout: $sys_out"
    echo "  system stderr: $sys_err"
    echo "  jash  (rc=$jash_rc) stdout: $jash_out"
    echo "  jash   stderr: $jash_err"
    return 1
}

for corpus in "$HERE"/cases/*.txt; do
    [ -e "$corpus" ] || continue
    tool="$(basename "$corpus" .txt)"
    tool_bin="$(tool_path "$tool")"
    if [ -z "$tool_bin" ]; then
        echo "skip: no system tool path registered for '$tool'"
        continue
    fi
    if [ ! -x "$tool_bin" ]; then
        echo "skip: $tool_bin not found on this system"
        continue
    fi
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in \#*) continue ;; esac
        run_case "$tool" "$line" "$tool_bin"
    done < "$corpus"
done

echo "---"
echo "conformance: $PASS pass, $FAIL fail, $SKIP documented exception(s)"
[ "$FAIL" -eq 0 ]
