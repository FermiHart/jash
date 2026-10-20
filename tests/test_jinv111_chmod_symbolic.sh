#!/usr/bin/env bash
# JINV-111 — `chmod +x` must work.
#
# The builtin shadows the name `chmod`, accepted octal only, and refused
# symbolic modes with a message calling the refusal honest. But a refusal is
# only honest if the caller can still reach a tool that works; shadowing the
# name removed that option, so `chmod +x file` returned rc=2 and changed
# nothing. The bug was found by an agent hitting it in ordinary use.
#
# Every case is differential against the system chmod, which is what bash runs.
# Comparing jash to itself would only prove it is self-consistent.
set -u

JASH="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
[ -x "$JASH" ] || { echo "SKIP: no jash at $JASH"; exit 0; }
SYS_CHMOD=/bin/chmod
[ -x "$SYS_CHMOD" ] || { echo "SKIP: no $SYS_CHMOD"; exit 0; }

export JASH_MEMO=0 JASH_LOOPGUARD=off

TMP=$(mktemp -d "${TMPDIR:-/tmp}/jinv111.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0

# `stat -f FMT` (BSD) || `stat -c FMT` (GNU) does NOT work: GNU stat reads -f as
# "filesystem", treats '%Lp' as a filename, and prints info for the real file on
# STDOUT before exiting 1 — so the fallback runs but its output is appended to
# the garbage instead of replacing it. Pick the dialect once, up front.
if stat -c '%a' . >/dev/null 2>&1; then
    modeof() { stat -c '%a' "$1" 2>/dev/null; }   # GNU coreutils
else
    modeof() { stat -f '%Lp' "$1" 2>/dev/null; }  # BSD / macOS
fi

# Both sides start from the same mode, get the same spec, and are compared on
# the resulting permission bits.
check() {
    local desc="$1" start="$2" spec="$3"
    local a="$TMP/sys.$$" b="$TMP/jash.$$"

    rm -f "$a" "$b"; : > "$a"; : > "$b"
    "$SYS_CHMOD" "$start" "$a"; "$SYS_CHMOD" "$start" "$b"

    "$SYS_CHMOD" "$spec" "$a" 2>/dev/null; local sys_rc=$?
    "$JASH" -c "chmod $spec $b" >/dev/null 2>&1; local jash_rc=$?

    local want got
    want=$(modeof "$a"); got=$(modeof "$b")

    if [ "$want" = "$got" ]; then
        pass=$((pass + 1))
        printf 'ok   %-26s %s %-10s -> %s\n' "$desc" "$start" "$spec" "$got"
    else
        fail=$((fail + 1))
        printf 'FAIL %-26s %s %-10s sys=%s(rc=%s) jash=%s(rc=%s)\n' \
               "$desc" "$start" "$spec" "$want" "$sys_rc" "$got" "$jash_rc"
    fi
}

echo "=== the reported bug ==="
check 'bare +x'            644 '+x'
check 'bare +x on 600'     600 '+x'
check 'bare -x'            755 '-x'

echo "=== who classes ==="
check 'u+x'                644 'u+x'
check 'g+w'                644 'g+w'
check 'o-r'                644 'o-r'
check 'a+r'                600 'a+r'
check 'ug+x'               644 'ug+x'
check 'go-w'               666 'go-w'

echo "=== assignment clears siblings ==="
check 'u=rw'               777 'u=rw'
check 'a='                 777 'a='
check 'g=r'                777 'g=r'
check 'u='                 755 'u='

echo "=== comma clauses ==="
check 'u=rw,go=r'          777 'u=rw,go=r'
check 'u+x,g-w'            660 'u+x,g-w'
check 'a=r,u+w'            000 'a=r,u+w'

echo "=== X: only if dir or already executable ==="
check 'X on plain file'    644 'a+X'
check 'X on exec file'     744 'a+X'
check 'X on 600'           600 'u+X'

echo "=== permcopy ==="
check 'g=u'                750 'g=u'
check 'o=g'                640 'o=g'

echo "=== special bits ==="
check 'u+s'                755 'u+s'
check 'g+s'                755 'g+s'
check '+t'                 755 '+t'
check 'u-s'                4755 'u-s'

echo "=== octal must not regress ==="
check 'octal 755'          644 '755'
check 'octal 600'          755 '600'
check 'octal 4755'         755 '4755'

echo "=== directories (X differs from x here) ==="
d_sys="$TMP/dsys"; d_jash="$TMP/djash"
mkdir -p "$d_sys" "$d_jash"
"$SYS_CHMOD" 644 "$d_sys"; "$SYS_CHMOD" 644 "$d_jash"
"$SYS_CHMOD" a+X "$d_sys" 2>/dev/null
"$JASH" -c "chmod a+X $d_jash" >/dev/null 2>&1
w=$(modeof "$d_sys"); g=$(modeof "$d_jash")
if [ "$w" = "$g" ]; then
    pass=$((pass + 1)); printf 'ok   %-26s -> %s\n' 'dir a+X' "$g"
else
    fail=$((fail + 1)); printf 'FAIL %-26s sys=%s jash=%s\n' 'dir a+X' "$w" "$g"
fi

echo "=== edge specs: accept/refuse decision must match the system ==="
# This section used to hardcode which specs I BELIEVED were malformed, and it
# was the only non-differential section in the file — so it was the only one
# that produced a false red. `u+` and `+x+` are in fact valid no-ops, and jash
# already matched the system on both. Asking the reference instead of asserting
# a belief is the whole method; applying it unevenly is how the method fails.
# Exact exit codes are NOT compared: BSD uses 1, jash uses 2, and neither is
# more correct. What must agree is the verdict (accept vs refuse) and the mode.
for spec in '+q' 'zzz' 'u+' '888' 'u+x,' '+x+' 'a' '=' 'u=g' '+rwx'; do
    a="$TMP/e_sys"; b="$TMP/e_jash"
    rm -f "$a" "$b"; : > "$a"; : > "$b"
    "$SYS_CHMOD" 644 "$a"; "$SYS_CHMOD" 644 "$b"

    "$SYS_CHMOD" "$spec" "$a" >/dev/null 2>&1
    sys_ok=$?
    "$JASH" -c "chmod $spec $b" >/dev/null 2>&1
    jash_ok=$?

    # Normalise to a verdict, not a number.
    [ "$sys_ok" -eq 0 ] && sys_v=accept || sys_v=refuse
    [ "$jash_ok" -eq 0 ] && jash_v=accept || jash_v=refuse
    want=$(modeof "$a"); got=$(modeof "$b")

    if [ "$sys_v" = "$jash_v" ] && [ "$want" = "$got" ]; then
        pass=$((pass + 1))
        printf 'ok   %-8s %-6s mode=%s\n' "$spec" "$sys_v" "$got"
    else
        fail=$((fail + 1))
        printf 'FAIL %-8s sys=%s/%s jash=%s/%s\n' \
               "$spec" "$sys_v" "$want" "$jash_v" "$got"
    fi
done

echo "=== -R resolves per file, not once ==="
# The point: X and +/- read each file's own mode. One file executable, one not.
for side in sys jash; do
    r="$TMP/r_$side"; mkdir -p "$r"
    : > "$r/plain"; : > "$r/exec"
    "$SYS_CHMOD" 644 "$r/plain"; "$SYS_CHMOD" 744 "$r/exec"; "$SYS_CHMOD" 755 "$r"
done
"$SYS_CHMOD" -R a+X "$TMP/r_sys" 2>/dev/null
"$JASH" -c "chmod -R a+X $TMP/r_jash" >/dev/null 2>&1
for f in plain exec; do
    w=$(modeof "$TMP/r_sys/$f"); g=$(modeof "$TMP/r_jash/$f")
    if [ "$w" = "$g" ]; then
        pass=$((pass + 1)); printf 'ok   -R a+X %-18s -> %s\n' "$f" "$g"
    else
        fail=$((fail + 1)); printf 'FAIL -R a+X %-18s sys=%s jash=%s\n' "$f" "$w" "$g"
    fi
done

echo
echo "=== JINV-111: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ] || exit 1
