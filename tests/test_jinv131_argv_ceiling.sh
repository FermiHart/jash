#!/usr/bin/env bash
# JINV-131 — argv ceiling: a glob wider than 128 files was silently truncated.
#
# Pre-fix evidence (this exact harness, 222 matched files):
#     jash printed 21 filenames, bash printed 222, exit status 0 in both.
# Not a crash, not an error — a silent wrong answer, which is the one failure
# mode this repo treats as unacceptable.
#
# Root cause was TWO fixed-size argv builders (parser/exec.c `build_argv` and
# parser/pipeline.c `build_argv_local`), each capped at 128 tokens. Both now
# share core/argvbuf.h and grow to the real sysconf(_SC_ARG_MAX) budget.
#
# Differential against bash — the only honest oracle. We assert equality, not
# a magic number, so this gate keeps working when the ceiling moves again.
#
# NOTE: the glob expander still has its own 256-match ceiling (JINV-132), so
# every count here stays under 256 deliberately. Raise these numbers only
# together with that fix, or this gate will start reporting someone else's bug.

# Prefer the freshly-built binary. `[ -x ]` is NOT a portability check:
# a stale macOS Mach-O in build-speed/ has the exec bit set and passes it,
# then dies with rc 126 on Linux (JINV-119, the stale-binary finding).
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
can_run() { env -i HOME="$TMP" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [ -n "${JASH:-}" ]; then
    can_run "$JASH" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    JASH=
    for candidate in "$PROJECT_ROOT/build/jash" "$PROJECT_ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then JASH="$candidate"; break; fi
    done
    if [ -z "$JASH" ]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
# Resolve once before entering the fixture. The runner may supply an absolute
# JASH path, which must not be prefixed with the previous working directory.
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
export JASH_MEMO=0 JASH_LOOPGUARD=off

pass=0; fail=0

ck() { # ck <label> <script>
    local label="$1" script="$2" g b grc brc
    # 2>&1 into an equality comparison lets ANY advisory line on stderr
    # (nudge / adoption / sigil / loopguard) decide the verdict — this gate
    # measured green or red by luck depending on the ratchet state of the
    # machine running it. Compare stdout, which is what the assertion is
    # about, and keep stderr out of the captured value.
    g=$(cd "$TMP" && "$JASH" -c "$script" 2>"$TMP/jash.stderr"); grc=$?
    b=$(cd "$TMP" && bash -c "$script" 2>"$TMP/bash.stderr"); brc=$?
    if [ "$grc" -eq 0 ] && [ "$brc" -eq 0 ] && [ "$g" = "$b" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL %s\n  jash(rc=%s): %.90s\n  bash(rc=%s): %.90s\n' \
            "$label" "$grc" "$g" "$brc" "$b"
        cat "$TMP/jash.stderr" "$TMP/bash.stderr" >&2
    fi
}

# 222 files: comfortably past the old 128 cap, safely under the glob's 256.
for i in $(seq 1 222); do printf 'line\n' > "$TMP/f$i.txt"; done

ck "echo wide glob (spawn)"      '/bin/echo *.txt | wc -w'
ck "cat wide glob (pipe/fork)"   'cat *.txt | wc -l'
ck "wc -l wide glob (spawn)"     'wc -l *.txt | tail -1'
ck "argv survives quoting"       '/bin/echo "a b" *.txt | wc -w'

# Long filenames: exercises the byte budget, not just the token count.
mkdir -p "$TMP/long"
for i in $(seq 1 200); do
    printf 'x\n' > "$TMP/long/$(printf 'n%03d' "$i")_padding_to_make_this_name_meaningfully_long.txt"
done
ck "long names (byte budget)"    '/bin/echo long/*.txt | wc -c'

# The old failure was silent, so also assert the shell did not start *erroring*
# instead: a loud regression is better than a quiet one, but neither is a pass.
error_output=$(cd "$TMP" && "$JASH" -c '/bin/echo *.txt >/dev/null' 2>&1); rc=$?
if [ "$rc" -ne 0 ] || printf '%s\n' "$error_output" | grep -q 'argv build failed'; then
    fail=$((fail+1)); printf 'FAIL wide glob rc=%s: %.90s\n' "$rc" "$error_output"
else
    pass=$((pass+1))
fi

printf '=== JINV-131: %d pass, %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
