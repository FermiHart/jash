#!/usr/bin/env bash
# Process-substitution resource ownership: each evaluated command must release
# the /dev/fd descriptors it created after the consumer has finished.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok() { printf 'ok    %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s: %s\n' "$1" "$2"; fail=$((fail + 1)); }

run_jash() {
    env -i HOME="$TMP" PATH="$PATH" TERM=dumb JASH_SIGIL=0 JASH_CHANGES=0 \
        JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off "$JASH" -c "$1"
}

basic=$(run_jash 'diff <(printf hi) <(printf hi) >/dev/null && echo same' 2>/dev/null)
if [ "$basic" = same ]; then
    ok 'process substitution still feeds both consumers'
else
    bad 'basic process substitution' "output=[$basic]"
fi

stress_cmd=''
for _ in $(seq 1 40); do
    stress_cmd="${stress_cmd}cat <(printf x); "
done
stress=$( (ulimit -n 32; run_jash "$stress_cmd") 2>/dev/null )
expected=$(printf 'x%.0s' $(seq 1 40))
if [ "$stress" = "$expected" ]; then
    ok 'sequential process substitutions do not exhaust fd limit'
else
    bad 'process-substitution fd reclamation' \
        "expected-bytes=${#expected} actual-bytes=${#stress}"
fi

reused="$TMP/reused-fd.txt"
reuse_cmd="f() { n=\${1##*/}; eval \"exec \$n>&-\"; eval \"exec \$n>$reused\"; }; f <(true); eval \"echo retained >&\$n\"; eval \"exec \$n>&-\"; cat $reused"
reuse=$(run_jash "$reuse_cmd" 2>/dev/null)
if [ "$reuse" = retained ]; then
    ok 'cleanup does not close a descriptor number reassigned by the script'
else
    bad 'process-substitution fd identity' "output=[$reuse]"
fi

printf '\n=== procsub resources: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
