#!/bin/bash
# ============================================================================
# bench-matrix.sh — run tests/conformance/corpus.txt under ONE candidate shell
#                   and emit a per-case verdict TSV.
# ============================================================================
#
# This is a strict sibling of tests/conformance/run-conformance.sh: same corpus,
# same fixture, same scrubbed environment, same PASS criterion (stdout byte for
# byte AND exit status), same 5 s alarm.  The ONLY difference is that the shell
# under test is a parameter instead of being hard-wired to jash, and that the
# output is machine-readable so several runs can be joined into a matrix.
#
# It does not read, write or modify the corpus or the original runner.
#
#   ./bench-matrix.sh --shell /bin/dash --label dash --out /tmp/dash.tsv
#
# TSV columns:  lineno  category  status  oracle_rc  cand_rc
#   status: PASS | FAIL | HANG | CRASH | OHANG
#     HANG  = candidate hit the 5 s alarm (rc 142) — its own category, not a FAIL
#     CRASH = candidate died on a signal (rc 134..139)
#     OHANG = the ORACLE hit the alarm; case is uncomparable, excluded
# ============================================================================

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../.." && pwd)}"
CORPUS="${CORPUS:-$REPO/tests/conformance/corpus.txt}"
BASH_BIN="${BASH_BIN:-/bin/bash}"
PERL_BIN="${PERL_BIN:-/usr/bin/perl}"
CASE_TIMEOUT="${CASE_TIMEOUT:-5}"
SANDBOX_PATH="${SANDBOX_PATH:-/usr/bin:/bin:/usr/sbin:/sbin}"

SHELL_BIN=""
LABEL=""
OUT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --shell) SHELL_BIN="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        --out)   OUT="$2"; shift 2 ;;
        --corpus) CORPUS="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -n "$SHELL_BIN" ] || { echo "--shell required" >&2; exit 2; }
[ -n "$LABEL" ]     || LABEL="$(basename "$SHELL_BIN")"
[ -n "$OUT" ]       || { echo "--out required" >&2; exit 2; }

for f in "$CORPUS" "$SHELL_BIN" "$BASH_BIN" "$PERL_BIN"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 2; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jash-matrix.XXXXXX")"
WORK="$(cd "$WORK" && pwd -P)"
SBX="$WORK/sbx"
trap 'rm -rf "$WORK"' EXIT INT TERM

# ------------------------------------------------------------- fixture -----
make_fixture() {
    local d="$1"
    mkdir -p "$d/home" "$d/sub"
    : > "$d/a.txt"
    : > "$d/ab.txt"
    : > "$d/b.txt"
    : > "$d/c.log"
    : > "$d/.hidden"
    : > "$d/empty.txt"
    : > "$d/two words.txt"
    : > "$d/sub/inner.txt"
    printf 'alpha\nbeta\ngamma\ndelta\n' > "$d/data.txt"
    printf '3\n1\n2\n'                   > "$d/nums.txt"
}

reset_sandbox() {
    rm -rf "$SBX"
    mkdir -p "$SBX"
    make_fixture "$SBX"
}

run_one() {
    local shell="$1" code="$2" tag="$3" rc
    reset_sandbox
    (
        cd "$SBX" || exit 127
        env -i \
            PATH="$SANDBOX_PATH" \
            HOME="$SBX/home" \
            PWD="$SBX" \
            LC_ALL=C LANG=C TERM=dumb \
            JASH_SIGIL=0 \
            "$PERL_BIN" -e 'alarm shift @ARGV; exec @ARGV' \
                "$CASE_TIMEOUT" "$shell" -c "$code"
    ) > "$WORK/$tag.out" 2> "$WORK/$tag.err" < /dev/null
    rc=$?
    echo "$rc"
}

: > "$OUT"

LINENO_C=0
while IFS= read -r line; do
    LINENO_C=$((LINENO_C + 1))
    case "$line" in
        ''|'#'*) continue ;;
        *'::'*)  ;;
        *) continue ;;
    esac
    category="${line%%::*}"
    code="${line#*::}"
    [ -n "$code" ] || continue

    code="${code//<NL>/$'\n'}"
    code="${code//<TAB>/$'\t'}"
    code="${code//<SP>/ }"

    brc=$(run_one "$BASH_BIN" "$code" "b")
    crc=$(run_one "$SHELL_BIN" "$code" "c")

    status=FAIL
    if [ "$brc" = 142 ]; then
        status=OHANG
    elif [ "$crc" = 142 ]; then
        status=HANG
    else
        case "$crc" in
            134|135|136|137|138|139) status=CRASH ;;
            *)
                if cmp -s "$WORK/b.out" "$WORK/c.out" && [ "$brc" = "$crc" ]; then
                    status=PASS
                else
                    status=FAIL
                fi
                ;;
        esac
    fi

    printf '%s\t%s\t%s\t%s\t%s\n' "$LINENO_C" "$category" "$status" "$brc" "$crc" >> "$OUT"
done < "$CORPUS"

echo "done: $LABEL -> $OUT" >&2
