#!/usr/bin/env bash
# Regressions found while staging the canonical Jash -> Beyond port.
set -u

cd "$(dirname "$0")/.." || exit 2

pass=0
fail=0

ok()  { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

if grep -q 'qsort(names,.*strcmp' c/builtins/builtin_context.c; then
    bad "context ask qsort comparator" "char ** array is still sorted through raw strcmp"
elif grep -q 'ask_qsort_name_cmp' c/builtins/builtin_context.c; then
    ok "context ask qsort comparator is typed"
else
    bad "context ask qsort comparator" "typed comparator is absent"
fi

if grep -q 'read(0, out, 1)' c/main.c; then
    bad "Beyond interactive input batching" "reader regressed to one syscall per decoder byte"
elif grep -q 'static char rbuf\[256\]' c/main.c &&
     grep -q 'read(0, rbuf, sizeof(rbuf))' c/main.c; then
    ok "Beyond interactive input batches the bounded surface read"
else
    bad "Beyond interactive input batching" "bounded 256-byte reader is absent"
fi

tmp_o="${TMPDIR:-/tmp}/jash-theme-io-beyond.$$.$RANDOM.o"
trap 'rm -f "$tmp_o"' EXIT INT TERM
if ${CC:-cc} -DJASH_TARGET_BEYOND -I c -c c/theme/io.c -o "$tmp_o" >/dev/null 2>&1; then
    ok "theme/io.c compiles under JASH_TARGET_BEYOND"
else
    bad "theme/io.c Beyond compile" "$(${CC:-cc} -DJASH_TARGET_BEYOND -I c -c c/theme/io.c -o "$tmp_o" 2>&1 | sed -n '1,3p')"
fi

if grep -q 'final_args' c/parser/builtin_redir.c ||
   grep -q 'jash_writer_t call_w' c/parser/builtin_redir.c ||
   grep -q 'void[[:space:]]*\*[[:space:]]*call_ctx' c/parser/builtin_redir.c; then
    bad "builtin redir dispatch edges" "dispatcher still hides argv/writer origins behind joined locals"
elif grep -q 'INV-523' c/parser/builtin_redir.c &&
     grep -q 'br_obuf_write, &obuf, unquoted' c/parser/builtin_redir.c &&
     grep -q 'br_obuf_write, &obuf, clean' c/parser/builtin_redir.c; then
    ok "builtin redir dispatch edges are explicit"
else
    bad "builtin redir dispatch edges" "explicit clean/unquoted and buffered/unbuffered calls not found"
fi

if [ -x ./build/jash ]; then
    out="$(JASH_SIGIL=0 ./build/jash -c 'printf "%f %g\n" 1.25 2.5' 2>&1)"
    if [ "$out" = "1.250000 2.5" ]; then
        ok "host float formatting remains enabled"
    else
        bad "host float formatting" "got: $out"
    fi

    redir_out="$(mktemp "${TMPDIR:-/tmp}/jash-redir-regression.XXXXXX")" || exit 2
    if JASH_SIGIL=0 ./build/jash -c "printf '%s\n' 'a b c' > '$redir_out'; cat '$redir_out'" 2>/dev/null | grep -qx 'a b c'; then
        ok "quoted printf through builtin redirection keeps one argument"
    else
        bad "quoted printf redirection" "quoted argument was split or lost"
    fi
    rm -f "$redir_out"
else
    bad "runtime probes" "./build/jash is missing; build first"
fi

printf '\n=== Beyond port regressions: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
