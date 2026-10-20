#!/usr/bin/env bash
# JINV-114 — a command name containing a slash is a PATHNAME, never a PATH search.
#
# POSIX XCU 2.9.1.1 is unambiguous: "If the command name contains at least one
# <slash>, the shell shall execute it as a pathname" — PATH is not consulted.
# jash_path_lookup() tested only for a LEADING '/' or '.', so the commonest
# relative forms in a repo fell through to the PATH walk:
#
#     $ build/app            → build/app: command not found     (it is RIGHT THERE)
#     $ scripts/run.sh       → scripts/run.sh: command not found
#     $ node_modules/.bin/x  → command not found
#
# The lie is the damage: "command not found" for a file the user can see, in a
# shape every Makefile, CI job and README uses. `./build/app` worked, which is
# exactly why it survived — the workaround looks like the idiom.
#
# The pair of assertions that pins the rule: ANY slash ⇒ pathname (no PATH),
# and NO slash ⇒ PATH lookup (never treated as a relative file).
set -u

JASH="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
[ -x "$JASH" ] || { echo "SKIP: no jash at $JASH"; exit 0; }

# Correctness probes must not be answered from cache nor throttled by the loop
# guard — both have produced false greens in this repo before.
export JASH_MEMO=0 JASH_LOOPGUARD=off

pass=0; fail=0

ok()   { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf 'FAIL %s\n     %s\n' "$1" "$2"; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/jinv114.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/build" "$TMP/scripts" "$TMP/node_modules/.bin" "$TMP/deep/a/b"
for f in build/app scripts/run.sh node_modules/.bin/tool deep/a/b/leaf; do
    printf '#!/bin/sh\necho RAN_%s\n' "$(basename "$f")" > "$TMP/$f"
    /bin/chmod 755 "$TMP/$f"
done

# ── ANY slash is a pathname: these must all run ──────────────────────────────
# stdout and stderr are captured apart on purpose: merging them and then
# comparing exactly lets a stray warning ride along inside the "expected"
# text. Here a warning is a failure, which is the point.
runs() {
    local desc="$1" cmd="$2" want="$3" got err rc
    err="$TMP/.stderr.$$"
    got=$(cd "$TMP" && "$JASH" -c "$cmd" 2>"$err"); rc=$?
    if [ "$rc" -eq 0 ] && [ "$got" = "$want" ] && [ ! -s "$err" ]; then ok "$desc"
    else bad "$desc" "cmd=[$cmd] rc=$rc got=[$got] want=[$want] err=[$(cat "$err")]"; fi
    rm -f "$err"
}

echo "=== a command name with a slash is executed as a pathname ==="
runs "relative dir/exe"        'build/app'                'RAN_app'
runs "nested relative"         'scripts/run.sh'           'RAN_run.sh'
runs "dot dir in the middle"   'node_modules/.bin/tool'   'RAN_tool'
runs "deep relative"           'deep/a/b/leaf'            'RAN_leaf'
runs "explicit ./ (regression)" './build/app'             'RAN_app'
runs "absolute (regression)"   "$TMP/build/app"           'RAN_app'
runs "../ from a subdir"       'cd scripts && ../build/app' 'RAN_app'

# The same name reached through a variable and through quoting: expansion must
# not change the classification.
runs "via variable"            'p=build/app; $p'          'RAN_app'
runs "quoted pathname"         '"build/app"'              'RAN_app'

echo
echo "=== a bare name is a PATH lookup, never a relative file ==="
# A file in CWD with no slash must NOT be found: bash refuses it, and a shell
# that "helpfully" runs ./x is a security bug (CWD-in-PATH by the back door).
printf '#!/bin/sh\necho CWD_LEAK\n' > "$TMP/leaker"; /bin/chmod 755 "$TMP/leaker"
got=$(cd "$TMP" && "$JASH" -c 'leaker' 2>&1); rc=$?
bash_got=$(cd "$TMP" && bash -c 'leaker' 2>&1); bash_rc=$?
if [ "$rc" -ne 0 ] && [ "$bash_rc" -ne 0 ]; then
    ok "bare name in CWD is not executed (no implicit . in PATH)"
else
    bad "bare name in CWD is not executed" "jash rc=$rc [$got] / bash rc=$bash_rc [$bash_got]"
fi

ERR="$TMP/.stderr.bare"
got=$(cd "$TMP" && "$JASH" -c 'echo BARE_BUILTIN' 2>"$ERR")
{ [ "$got" = "BARE_BUILTIN" ] && [ ! -s "$ERR" ]; } && ok "bare builtin still resolves" \
    || bad "bare builtin still resolves" "got=[$got] err=[$(cat "$ERR")]"

got=$("$JASH" -c 'true && echo PATH_OK' 2>"$ERR")
{ [ "$got" = "PATH_OK" ] && [ ! -s "$ERR" ]; } && ok "bare PATH command still resolves" \
    || bad "bare PATH command still resolves" "got=[$got] err=[$(cat "$ERR")]"
rm -f "$ERR"

echo
echo "=== a missing pathname fails, and says so ==="
got=$(cd "$TMP" && "$JASH" -c 'nope/missing' 2>&1); rc=$?
if [ "$rc" -eq 127 ]; then ok "missing pathname → rc 127"
else bad "missing pathname → rc 127" "rc=$rc got=[$got]"; fi

# A directory is not a command. Silently succeeding here would be worse than
# any message.
got=$(cd "$TMP" && "$JASH" -c 'build' 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "a directory is not a command" \
    || bad "a directory is not a command" "rc=$rc got=[$got]"

echo
echo "=== JINV-114: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ] || exit 1
