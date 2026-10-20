#!/usr/bin/env bash
# JINV-132 — the glob expander had TWO silent-wrong-answer defects, not one.
#
# (a) 256-match ceiling. `matches[256][256]` in c/parser/expand.c. A directory
#     with 300 files expanded to 256 names, exit status 0. Nothing printed.
#
# (b) 255-byte path truncation. Each match was copied into a 256-byte row, so
#     any path longer than 255 bytes was cut mid-string. Measured pre-fix on
#     the fixture below: bash printed a 346-byte path, jash printed 254 bytes
#     of it — a path that names NOTHING. Worse than (a): (a) drops a file, (b)
#     invents one.
#
# Both are now the growable builder from core/argvbuf.h (JINV-131), so the
# ceiling is the real ARG_MAX budget and the path is whatever the filesystem
# actually holds.
#
# Differential against bash, which is the only honest oracle here. We assert
# equality of the WHOLE list, never a magic count — so the gate survives the
# next time a ceiling moves.
#
# A note on what this gate deliberately does NOT accept: "jash printed fewer
# files but did not crash" is a FAIL, not a warning. Silently short output is
# the failure mode this repo exists to kill.
#
#                                                     F E R M I ∞ H A R T

# Prefer the freshly-built binary. `[ -x ]` is NOT a portability check:
# a stale macOS Mach-O in build-speed/ has the exec bit set and passes it,
# then dies with rc 126 on Linux (JINV-119, the stale-binary finding).
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/probe-home"
jash_usable() {
    /usr/bin/env -u JASH_SESSION -u JASH_AGENT HOME="$TMP/probe-home" \
      JASH_SIGIL=0 JASH_RECALL_GUARD=0 JASH_RISK_CHECKPOINT=0 \
      "$1" -c 'exit 0' >/dev/null 2>&1
}
JASH="${JASH:-./build/jash}"
jash_usable "$JASH" || JASH=./build-speed/jash-release
jash_usable "$JASH" || JASH=./build/jash
if ! jash_usable "$JASH"; then echo "SKIP: no runnable jash binary"; exit 0; fi
JASH=$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")
export JASH_MEMO=0 JASH_LOOPGUARD=off

pass=0; fail=0

ck() { # ck <label> <script>   — run in $TMP under both shells, compare exactly
    local label="$1" script="$2" g b
    # 2>&1 into an equality comparison lets ANY advisory line on stderr
    # (nudge / adoption / sigil / loopguard) decide the verdict — this gate
    # measured green or red by luck depending on the ratchet state of the
    # machine running it. Compare stdout, which is what the assertion is
    # about, and keep stderr out of the captured value.
    g=$("$JASH" -c "cd $TMP && $script" 2>/dev/null)
    b=$(bash    -c "cd $TMP && $script" 2>/dev/null)
    if [ "$g" = "$b" ]; then
        pass=$((pass+1)); printf 'ok   %s\n' "$label"
    else
        fail=$((fail+1))
        printf 'FAIL %s\n' "$label"
        printf '       bash: %s words, %s bytes\n' "$(printf '%s' "$b" | wc -w | tr -d ' ')" "$(printf '%s' "$b" | wc -c | tr -d ' ')"
        printf '       jash: %s words, %s bytes\n' "$(printf '%s' "$g" | wc -w | tr -d ' ')" "$(printf '%s' "$g" | wc -c | tr -d ' ')"
    fi
}

# ---------------------------------------------------------------- (a) count
# 300 files: past the old 256 rows, still well inside ARG_MAX.
mkdir -p "$TMP/wide"
i=0; while [ $i -lt 300 ]; do : > "$TMP/wide/f$(printf '%03d' $i).txt"; i=$((i+1)); done

ck "wide/300 single-component"      'echo wide/*.txt'
ck "wide/300 multi-component"       'echo wid*/*.txt'
ck "wide/300 counted"               'set -- wide/*.txt; echo $#'
ck "wide/300 globstar"              'shopt -s globstar 2>/dev/null; echo wide/**/*.txt'

# ------------------------------------------------------------- (b) path len
# A 346-byte path. The old code copied it into char[256] and printed the
# first 255 bytes: a path that does not exist on any filesystem.
LONG_D=$(printf 'd%.0s' $(seq 1 120))
LONG_F=$(printf 'f%.0s' $(seq 1 100))
mkdir -p "$TMP/$LONG_D/$LONG_D"
: > "$TMP/$LONG_D/$LONG_D/$LONG_F.txt"

ck "346-byte path single"           "echo $LONG_D/$LONG_D/*.txt"
ck "346-byte path multi-component"  "echo $LONG_D/*/*.txt"
ck "346-byte path is real"          "for p in $LONG_D/$LONG_D/*.txt; do [ -e \"\$p\" ] && echo REAL || echo GHOST; done"

# ----------------------------------------------------- both defects at once
# 300 files, each under a long directory prefix: count AND length together.
mkdir -p "$TMP/$LONG_D/wide"
i=0; while [ $i -lt 300 ]; do : > "$TMP/$LONG_D/wide/g$(printf '%03d' $i).txt"; i=$((i+1)); done
ck "300 long paths"                 "echo $LONG_D/wide/*.txt"
ck "300 long paths all exist"       "n=0; for p in $LONG_D/wide/*.txt; do [ -e \"\$p\" ] || n=\$((n+1)); done; echo missing=\$n"

# Globstar used to append directly into the fixed expansion buffer.
# Above that boundary it returned a plausible prefix with exit 0. Refusal must
# be atomic: literal pattern on stdout plus a visible diagnostic on stderr.
mkdir -p "$TMP/$LONG_D/$LONG_D/$LONG_D/overflow"
i=0; while [ $i -lt 600 ]; do : > "$TMP/$LONG_D/$LONG_D/$LONG_D/overflow/${LONG_F}$(printf '%03d' $i).txt"; i=$((i+1)); done
overflow_pat="$LONG_D/$LONG_D/$LONG_D/overflow/**/*.txt"
"$JASH" -c "cd $TMP; shopt -s globstar 2>/dev/null; echo $overflow_pat; touch overflow-ran" \
    >"$TMP/globstar.out" 2>"$TMP/globstar.err"
overflow_rc=$?
overflow_out=$(cat "$TMP/globstar.out")
if [ "$overflow_rc" -ne 0 ] && [ -z "$overflow_out" ] &&
   [ ! -e "$TMP/overflow-ran" ] && grep -q 'globstar: expanded line exceeds' "$TMP/globstar.err"; then
    pass=$((pass+1)); printf 'ok   %s\n' "globstar overflow refuses atomically"
else
    fail=$((fail+1)); printf 'FAIL %s\n' "globstar overflow refuses atomically"
    printf '       output: %s bytes; stderr: %s\n' "$(printf '%s' "$overflow_out" | wc -c | tr -d ' ')" "$(head -1 "$TMP/globstar.err")"
fi

"$JASH" -c "cd $TMP; x=\$(echo $overflow_pat); touch inner-glob-ran" \
    >"$TMP/inner-glob.out" 2>"$TMP/inner-glob.err"
inner_glob_rc=$?
if [ "$inner_glob_rc" -ne 0 ] && [ ! -s "$TMP/inner-glob.out" ] &&
   [ ! -e "$TMP/inner-glob-ran" ] &&
   grep -q 'inner expansion refused' "$TMP/inner-glob.err"; then
    pass=$((pass+1)); printf 'ok   %s\n' "glob refusal inside cmdsub reaches outer evaluator"
else
    fail=$((fail+1)); printf 'FAIL %s\n' "glob refusal inside cmdsub reaches outer evaluator"
fi

# The traversal and emitted path must share a lossless dynamic join. Five
# legal components exceed the old 511-byte child_rel buffer.
DEEP="$LONG_D/$LONG_D/$LONG_D/$LONG_D/$LONG_D"
mkdir -p "$TMP/$DEEP"
: > "$TMP/$DEEP/deep-real.txt"
ck "globstar path over 511 is real" 'shopt -s globstar 2>/dev/null; for p in **/deep-real.txt; do [ -e "$p" ] && echo REAL:${#p} || echo GHOST; done'
ck "glob pattern over 511 exact"     "echo $DEEP/*.txt"

# Extglob alternatives and alternatives longer than the old fixed rows are
# complete, sorted, and never emitted as a prefix.
ext_alts=""; i=0
while [ $i -lt 20 ]; do n=$(printf 'e%02d' "$i"); : > "$TMP/$n"; ext_alts="${ext_alts}${ext_alts:+|}$n"; i=$((i+1)); done
ext_out=$("$JASH" -c "cd $TMP; echo @($ext_alts)" 2>/dev/null)
[ "$(printf '%s' "$ext_out" | wc -w | tr -d ' ')" = 20 ] && {
    pass=$((pass+1)); printf 'ok   %s\n' "extglob keeps all 20 alternatives";
} || {
    fail=$((fail+1)); printf 'FAIL %s\n' "extglob keeps all 20 alternatives";
}
ext_long=$(printf 'q%.0s' $(seq 1 140)); : > "$TMP/$ext_long"
ext_long_out=$("$JASH" -c "cd $TMP; echo @($ext_long)" 2>/dev/null)
if [ "$ext_long_out" = "$ext_long" ]; then
    pass=$((pass+1)); printf 'ok   %s\n' "extglob alternative over 128 exact"
else
    fail=$((fail+1)); printf 'FAIL %s\n' "extglob alternative over 128 exact"
fi

# Ordinary glob output fits alone, but not together with a long suffix. The
# suffix must not disappear and neither command may execute.
ordinary_pat_a="$LONG_D/$LONG_D/$LONG_D/overflow/${LONG_F}[0-3]*.txt"
ordinary_pat_b="$LONG_D/$LONG_D/$LONG_D/overflow/${LONG_F}[4-5]*.txt"
"$JASH" -c "cd $TMP; /bin/echo $ordinary_pat_a $ordinary_pat_b" \
    >"$TMP/aggregate-glob.out" 2>"$TMP/aggregate-glob.err"
aggregate_glob_rc=$?
if [ "$aggregate_glob_rc" -ne 0 ] && [ ! -s "$TMP/aggregate-glob.out" ] &&
   grep -Eq 'jash: glob: .*exceeds' "$TMP/aggregate-glob.err"; then
    pass=$((pass+1)); printf 'ok   %s\n' "ordinary glob aggregate refuses atomically"
else
    fail=$((fail+1)); printf 'FAIL %s (rc=%s out=%s err=%s)\n' \
        "ordinary glob aggregate refuses atomically" "$aggregate_glob_rc" \
        "$(wc -c < "$TMP/aggregate-glob.out" | tr -d ' ')" \
        "$(head -1 "$TMP/aggregate-glob.err")"
fi

# Traversal support limits fail visibly instead of returning a successful
# no-match result after silently skipping deeper files.
deep64="depth"; mkdir -p "$TMP/$deep64"; i=0
while [ $i -lt 66 ]; do deep64="$deep64/d"; mkdir -p "$TMP/$deep64"; i=$((i+1)); done
: > "$TMP/$deep64/at-depth.txt"
"$JASH" -c "cd $TMP; echo depth/**/*.txt; touch depth-ran" \
    >"$TMP/depth.out" 2>"$TMP/depth.err"
depth_rc=$?
if [ "$depth_rc" -ne 0 ] && [ ! -e "$TMP/depth-ran" ]; then
    pass=$((pass+1)); printf 'ok   %s\n' "globstar depth ceiling refuses visibly"
else
    fail=$((fail+1)); printf 'FAIL %s\n' "globstar depth ceiling refuses visibly"
fi

# ------------------------------------------------------------------- no-match
# The ceiling work must not disturb the empty case: an unmatched glob stays
# literal (bash default, nullglob off).
ck "no match stays literal"         'echo wide/nothing-here-*.txt'

printf '\n=== JINV-132: %d pass, %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
