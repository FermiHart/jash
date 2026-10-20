#!/bin/bash
# ============================================================================
# jash SHELL-LANGUAGE conformance harness  —  bash is the oracle
# ============================================================================
#
# This is NOT the same thing as run_conformance.sh (underscore) that lives in
# this directory.  That one measures jash's *builtin tools* (ls/cp/mv/rm/grep)
# against the system tools they shadow.  THIS one measures the *shell language*:
# expansion, quoting, arithmetic, redirection, control flow, exit codes.
#
# Method
# ------
#   For every case in corpus.txt we run the exact same line of shell in
#   /bin/bash and in jash, inside the SAME sandbox directory (reset between
#   the two runs, so $PWD is identical and mutations cannot leak), under the
#   same scrubbed environment (env -i), under `alarm 5`.
#   A case PASSES when stdout and the exit status both match.
#   stderr is compared only with CHECK_STDERR=1 (text of diagnostics is not
#   something jash ever promised to match byte-for-byte).
#
# Corpus format
# -------------
#   One case per line:   <category>::<shell code>
#   The separator is the FIRST '::' on the line; category names never contain
#   a colon, so a '::' inside the shell code is never mistaken for it.
#   '#' in column 1 and blank lines are comments.
#   The literal tokens <NL>, <TAB> and <SP> are replaced by a newline, a tab
#   and a space before execution — that is how multi-line cases (heredocs,
#   multi-line loops, <<-) stay one-case-per-line, and how a *trailing* space
#   survives editors, patches and copy-paste.  Several edge cases are about
#   exactly that trailing space, so never "clean up" a <SP>.
#
# Fixture
# -------
#   Each run gets a fresh identical fixture (see make_fixture below).  Cases
#   are written against it.  Because jash shadows cat/wc/sed/awk with builtins,
#   cases that only need a neutral stdin consumer call /bin/cat and
#   /usr/bin/wc by absolute path — this harness measures the LANGUAGE, not the
#   builtin tool implementations.
#
# Usage
# -----
#   ./run-conformance.sh                  # everything
#   ./run-conformance.sh --only param     # one category
#   ./run-conformance.sh --list           # category counts
#   CHECK_STDERR=1 ./run-conformance.sh   # also require stderr to match
#   JASH_BIN=/path/to/jash ./run-conformance.sh
#
# Exit status: 0 if every case matched, 1 otherwise. It is deliberately NOT
# wired into tests/test_suite.sh yet — see audits/CONFORMANCE-2026-07-27.md.
# ============================================================================

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

CORPUS="${CORPUS:-$HERE/corpus.txt}"
JASH_BIN="${JASH_BIN:-$REPO/build/jash}"
BASH_BIN="${BASH_BIN:-/bin/bash}"
PERL_BIN="${PERL_BIN:-/usr/bin/perl}"
CASE_TIMEOUT="${CASE_TIMEOUT:-5}"
CHECK_STDERR="${CHECK_STDERR:-0}"
SANDBOX_PATH="${SANDBOX_PATH:-/usr/bin:/bin:/usr/sbin:/sbin}"

ONLY=""
LIST_ONLY=0
VERBOSE=0
MAXDIV="${MAXDIV:-0}"          # 0 = print every divergence

while [ $# -gt 0 ]; do
    case "$1" in
        --only)    ONLY="$2"; shift 2 ;;
        --only=*)  ONLY="${1#--only=}"; shift ;;
        --list)    LIST_ONLY=1; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        --max-div) MAXDIV="$2"; shift 2 ;;
        --corpus)  CORPUS="$2"; shift 2 ;;
        -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

for f in "$CORPUS" "$JASH_BIN" "$BASH_BIN" "$PERL_BIN"; do
    if [ ! -e "$f" ]; then echo "missing: $f" >&2; exit 2; fi
done

# ---------------------------------------------------------------- --list ----
if [ "$LIST_ONLY" = 1 ]; then
    grep -v '^#' "$CORPUS" | grep '::' \
        | sed 's/::.*//' | sort | uniq -c | sort -rn
    exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jash-conf.XXXXXX")"
# Resolve to the PHYSICAL path.  On macOS both /tmp and $TMPDIR are symlinks
# into /private, and TMPDIR carries a trailing slash; without this, cases that
# print $PWD/$OLDPWD would diverge for a reason that has nothing to do with
# either shell (bash reports the logical path it was handed, jash realpath()s
# it).  Normalising here keeps such cases honest instead of excluded.
WORK="$(cd "$WORK" && pwd -P)"
SBX="$WORK/sbx"
RESULTS="${CONF_RESULTS:-$WORK/results.tsv}"
trap 'rm -rf "$WORK"' EXIT INT TERM

# ------------------------------------------------------------- fixture -----
# Identical starting filesystem for both shells.  Kept deliberately small and
# LC_ALL=C-sortable so `echo *` has one obvious answer.
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

# -------------------------------------------------------------- runner -----
# Runs $2 under shell $1 in the (freshly reset) sandbox.  Leaves stdout in
# $WORK/$3.out, stderr in $WORK/$3.err and echoes the exit status.
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

# one-line, escaped, truncated rendering of a file for the report
show() {
    "$PERL_BIN" -e '
        local $/; my $s = <STDIN>; $s = "" unless defined $s;
        my $n = length $s;
        $s =~ s/\\/\\\\/g; $s =~ s/\n/\\n/g; $s =~ s/\t/\\t/g;
        $s =~ s/([^\x20-\x7e])/sprintf("\\x%02x", ord($1))/ge;
        if (length($s) > 300) { $s = substr($s,0,300) . "...<${n}B>"; }
        print $s;
    ' < "$1"
}

# ---------------------------------------------------------------- main -----
START=$("$PERL_BIN" -MTime::HiRes=time -e 'printf "%.3f", time')

TOTAL=0; PASS=0; FAIL=0
HANG_JASH=0; HANG_BASH=0; CRASH_JASH=0
: > "$RESULTS"
: > "$WORK/divergences.txt"
: > "$WORK/hangs.txt"
: > "$WORK/crashes.txt"

echo "=========================================================================="
echo " jash shell-language conformance   (oracle: $BASH_BIN)"
echo "=========================================================================="
printf ' jash   : %s\n' "$("$JASH_BIN" --version 2>&1 | head -1)"
printf ' bash   : %s\n' "$("$BASH_BIN" --version 2>&1 | head -1)"
printf ' corpus : %s\n' "$CORPUS"
printf ' timeout: %ss per case   stderr compared: %s\n' "$CASE_TIMEOUT" "$CHECK_STDERR"
echo

LINENO_C=0
MALFORMED=0
while IFS= read -r line; do
    LINENO_C=$((LINENO_C + 1))
    case "$line" in
        ''|'#'*) continue ;;
        *'::'*)  ;;
        *) MALFORMED=$((MALFORMED + 1))
           echo "corpus line $LINENO_C has no '::' separator, skipped" >&2
           continue ;;
    esac
    category="${line%%::*}"
    code="${line#*::}"
    [ -n "$code" ] || continue
    if [ -n "$ONLY" ] && [ "$category" != "$ONLY" ]; then continue; fi

    # <NL>/<TAB>/<SP> tokens -> real newline / tab / space.
    # <SP> exists so that a *trailing* space in a case survives editors,
    # patches and copy-paste: several edge cases are about exactly that.
    code="${code//<NL>/$'\n'}"
    code="${code//<TAB>/$'\t'}"
    code="${code//<SP>/ }"

    TOTAL=$((TOTAL + 1))

    brc=$(run_one "$BASH_BIN" "$code" "b")
    jrc=$(run_one "$JASH_BIN" "$code" "j")

    ok=1
    cmp -s "$WORK/b.out" "$WORK/j.out" || ok=0
    [ "$brc" = "$jrc" ] || ok=0
    if [ "$CHECK_STDERR" = 1 ]; then
        cmp -s "$WORK/b.err" "$WORK/j.err" || ok=0
    fi

    hung=0
    if [ "$jrc" = 142 ]; then hung=1; HANG_JASH=$((HANG_JASH + 1)); fi
    if [ "$brc" = 142 ]; then HANG_BASH=$((HANG_BASH + 1)); fi
    crashed=0
    case "$jrc" in
        134|135|136|137|138|139|memo) crashed=1; CRASH_JASH=$((CRASH_JASH + 1)) ;;
    esac

    if [ "$ok" = 1 ]; then
        PASS=$((PASS + 1))
        printf '%s\tPASS\t%s\t%s\t%s\n' "$category" "$LINENO_C" "$brc" "$jrc" >> "$RESULTS"
        [ "$VERBOSE" = 1 ] && printf '  ok   [%s] %s\n' "$category" "$code"
    else
        FAIL=$((FAIL + 1))
        printf '%s\tFAIL\t%s\t%s\t%s\n' "$category" "$LINENO_C" "$brc" "$jrc" >> "$RESULTS"
        {
            printf -- '--- [%s] line %s\n' "$category" "$LINENO_C"
            printf '    case : %s\n' "$(printf '%s' "$code" | "$PERL_BIN" -pe 's/\n/<NL>/g')"
            printf '    bash : rc=%-3s out=%s\n' "$brc" "$(show "$WORK/b.out")"
            printf '    jash : rc=%-3s out=%s\n' "$jrc" "$(show "$WORK/j.out")"
            if [ "$CHECK_STDERR" = 1 ] || [ "$VERBOSE" = 1 ]; then
                printf '    bash!: %s\n' "$(show "$WORK/b.err")"
                printf '    jash!: %s\n' "$(show "$WORK/j.err")"
            fi
            echo
        } >> "$WORK/divergences.txt"
        [ "$VERBOSE" = 1 ] && printf '  FAIL [%s] %s\n' "$category" "$code"
    fi

    if [ "$hung" = 1 ]; then
        {
            printf -- '!!! HANG (jash did not finish in %ss) [%s] line %s\n' \
                "$CASE_TIMEOUT" "$category" "$LINENO_C"
            printf '    case : %s\n' "$(printf '%s' "$code" | "$PERL_BIN" -pe 's/\n/<NL>/g')"
            printf '    bash : rc=%s out=%s\n' "$brc" "$(show "$WORK/b.out")"
            echo
        } >> "$WORK/hangs.txt"
    fi
    if [ "$crashed" = 1 ]; then
        {
            printf -- '!!! CRASH (jash died on signal, rc=%s) [%s] line %s\n' \
                "$jrc" "$category" "$LINENO_C"
            printf '    case : %s\n' "$(printf '%s' "$code" | "$PERL_BIN" -pe 's/\n/<NL>/g')"
            printf '    bash : rc=%s out=%s\n' "$brc" "$(show "$WORK/b.out")"
            printf '    jash!: %s\n' "$(show "$WORK/j.err")"
            echo
        } >> "$WORK/crashes.txt"
    fi
done < "$CORPUS"

END=$("$PERL_BIN" -MTime::HiRes=time -e 'printf "%.3f", time')
ELAPSED=$("$PERL_BIN" -e 'printf "%.1f", $ARGV[1]-$ARGV[0]' "$START" "$END")

# ------------------------------------------------------------- report ------
echo "--------------------------------------------------------------------------"
echo " PER-CATEGORY SCORE (worst first)"
echo "--------------------------------------------------------------------------"
cut -f1,2 "$RESULTS" | sort | uniq -c \
  | "$PERL_BIN" -e '
      my %p; my %f;
      while (<STDIN>) {
          my ($n,$c,$v) = /^\s*(\d+)\s+(\S+)\s+(\S+)/ or next;
          if ($v eq "PASS") { $p{$c} += $n } else { $f{$c} += $n }
      }
      my @rows;
      for my $c (keys %{{ map { $_ => 1 } (keys %p, keys %f) }}) {
          my $pa = $p{$c} // 0; my $fa = $f{$c} // 0; my $t = $pa + $fa;
          push @rows, [$c, $pa, $t, $t ? 100*$pa/$t : 0];
      }
      for my $r (sort { $a->[3] <=> $b->[3] or $a->[0] cmp $b->[0] } @rows) {
          printf "  %-12s %4d/%-4d  %6.1f%%  %s\n", $r->[0], $r->[1], $r->[2], $r->[3],
                 ("#" x int($r->[3]/4));
      }
  '
echo

if [ -s "$WORK/crashes.txt" ]; then
    echo "=========================================================================="
    echo " !!!!!  CRASHES  ($CRASH_JASH)  —  jash terminated by a signal"
    echo "=========================================================================="
    cat "$WORK/crashes.txt"
fi
if [ -s "$WORK/hangs.txt" ]; then
    echo "=========================================================================="
    echo " !!!!!  HANGS  ($HANG_JASH)  —  jash exceeded ${CASE_TIMEOUT}s"
    echo "=========================================================================="
    cat "$WORK/hangs.txt"
fi

if [ -s "$WORK/divergences.txt" ]; then
    echo "--------------------------------------------------------------------------"
    echo " DIVERGENCES ($FAIL)"
    echo "--------------------------------------------------------------------------"
    if [ "$MAXDIV" -gt 0 ]; then
        head -n $((MAXDIV * 6)) "$WORK/divergences.txt"
        echo "  (truncated to $MAXDIV; run with --max-div 0 for all)"
    else
        cat "$WORK/divergences.txt"
    fi
fi

PCT=$("$PERL_BIN" -e 'printf "%.1f", $ARGV[0] ? 100*$ARGV[1]/$ARGV[0] : 0' "$TOTAL" "$PASS")
echo "=========================================================================="
printf ' TOTAL   %s/%s  (%s%%)\n' "$PASS" "$TOTAL" "$PCT"
printf ' hangs   jash=%s  bash=%s\n' "$HANG_JASH" "$HANG_BASH"
printf ' crashes jash=%s\n' "$CRASH_JASH"
[ "$MALFORMED" -gt 0 ] && printf ' MALFORMED corpus lines skipped: %s\n' "$MALFORMED"
printf ' elapsed %ss\n' "$ELAPSED"
echo "=========================================================================="

if [ -n "${CONF_RESULTS:-}" ]; then
    echo " results tsv: $RESULTS"
    cp "$WORK/divergences.txt" "$(dirname "$RESULTS")/divergences.txt" 2>/dev/null
    cp "$WORK/hangs.txt"       "$(dirname "$RESULTS")/hangs.txt"       2>/dev/null
    cp "$WORK/crashes.txt"     "$(dirname "$RESULTS")/crashes.txt"     2>/dev/null
fi

[ "$FAIL" = 0 ]
