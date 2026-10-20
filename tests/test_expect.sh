#!/usr/bin/env bash
# test_expect.sh — `expect` contracts (ADR-0023 E3). Asserts a fact about state
# and fails loudly (rc 1) when the agent's mental model is wrong; rc 2 on misuse.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }

W="$(mktemp -d)" || exit 2
trap 'rm -rf "$W"' EXIT INT TERM
mkdir -p "$W/d"
echo "hello world" > "$W/has.txt"; : > "$W/empty.txt"; printf 'x' > "$W/ne.txt"
cp /bin/echo "$W/prog" 2>/dev/null

rcof(){ "$J" -c "$1" >/dev/null 2>&1; echo $?; }

echo "== expect contracts (ADR-0023 E3) =="
[ "$(rcof "expect file $W/has.txt")"   = 0 ] && ok 'file exists → rc0'        || no 'file exists' "rc!=0"
[ "$(rcof "expect file $W/nope")"      = 1 ] && ok 'file missing → rc1'       || no 'file missing' "rc!=1"
[ "$(rcof "expect dir $W/d")"          = 0 ] && ok 'dir → rc0'               || no 'dir' "rc!=0"
[ "$(rcof "expect dir $W/has.txt")"    = 1 ] && ok 'non-dir → rc1'           || no 'non-dir' "rc!=1"
[ "$(rcof "expect no $W/nope")"        = 0 ] && ok 'absent → rc0'            || no 'absent' "rc!=0"
[ "$(rcof "expect no $W/has.txt")"     = 1 ] && ok 'present (no) → rc1'      || no 'present(no)' "rc!=1"
[ "$(rcof "expect exec $W/prog")"      = 0 ] && ok 'exec → rc0'             || no 'exec' "rc!=0"
[ "$(rcof "expect exec $W/has.txt")"   = 1 ] && ok 'non-exec → rc1'         || no 'non-exec' "rc!=1"
[ "$(rcof "expect empty $W/empty.txt")" = 0 ] && ok 'empty → rc0'            || no 'empty' "rc!=0"
[ "$(rcof "expect empty $W/ne.txt")"    = 1 ] && ok 'non-empty (empty) → rc1' || no 'empty(ne)' "rc!=1"
[ "$(rcof "expect nonempty $W/ne.txt")" = 0 ] && ok 'nonempty → rc0'         || no 'nonempty' "rc!=0"
[ "$(rcof "expect contains $W/has.txt world")" = 0 ] && ok 'contains → rc0' || no 'contains' "rc!=0"
[ "$(rcof "expect contains $W/has.txt zzz")"   = 1 ] && ok 'not-contains → rc1' || no 'not-contains' "rc!=1"
[ "$(rcof "true; expect rc 0")"        = 0 ] && ok 'rc match → rc0'         || no 'rc match' "rc!=0"
[ "$(rcof "false; expect rc 0")"       = 1 ] && ok 'rc mismatch → rc1'      || no 'rc mismatch' "rc!=1"
[ "$(rcof "expect")"                   = 2 ] && ok 'misuse → rc2'           || no 'misuse' "rc!=2"

# newer/older (touch ordering)
old="$W/old.txt"; new="$W/new.txt"; : > "$old"; sleep 1; : > "$new"
[ "$(rcof "expect newer $new $old")" = 0 ] && ok 'newer → rc0'            || no 'newer' "rc!=0"
[ "$(rcof "expect newer $old $new")" = 1 ] && ok 'not-newer → rc1'        || no 'not-newer' "rc!=1"
[ "$(rcof "expect older $old $new")" = 0 ] && ok 'older → rc0'            || no 'older' "rc!=0"

# match (byte equality)
cp "$W/has.txt" "$W/copy.txt"
[ "$(rcof "expect match $W/has.txt $W/copy.txt")" = 0 ] && ok 'match → rc0'    || no 'match' "rc!=0"
[ "$(rcof "expect match $W/has.txt $W/ne.txt")"   = 1 ] && ok 'no-match → rc1'  || no 'no-match' "rc!=1"

# negation
[ "$(rcof "expect not file $W/nope")"    = 0 ] && ok 'not (absent) → rc0'  || no 'not absent' "rc!=0"
[ "$(rcof "expect not file $W/has.txt")" = 1 ] && ok 'not (present) → rc1'  || no 'not present' "rc!=1"

# failure verdict goes to stderr and is readable
errout="$("$J" -c "expect file $W/nope" 2>&1 1>/dev/null | sed $'s/\x1b\\[[0-9;]*m//g')"
printf '%s' "$errout" | grep -q 'FAILED' && ok 'failure verdict on stderr' || no 'failure verdict' "$errout"

# --json structured output (agent-consumable)
jout="$("$J" -c "expect file $W/has.txt --json" 2>/dev/null)"
printf '%s' "$jout" | grep -q '"ok":true' && ok '--json ok=true' || no '--json ok' "$jout"
jout="$("$J" -c "expect file $W/nope --json" 2>/dev/null)"
{ printf '%s' "$jout" | grep -q '"ok":false' && printf '%s' "$jout" | grep -q '"actual":"missing"'; } && ok '--json failure has actual' || no '--json fail' "$jout"
"$J" -c "expect file $W/nope --json" >/dev/null 2>&1; [ $? -eq 1 ] && ok '--json keeps rc1 on fail' || no '--json rc' "rc=$?"

# composes with && (chain stops on first failure)
"$J" -c "expect file $W/has.txt && expect contains $W/has.txt hello" >/dev/null 2>&1
[ $? -eq 0 ] && ok 'composes with &&' || no 'composes &&' "rc!=0"

echo "== expect: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
