#!/usr/bin/env bash
# PATH cache: persist positive resolutions incrementally, keep misses in memory,
# invalidate stale paths after exec failure, and key every entry by PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$ROOT/build/jash"
if [ ! -x "$JASH" ]; then
    echo "FAIL: build/jash not found; build the jash target first" >&2
    exit 1
fi

T="$(mktemp -d "${TMPDIR:-/tmp}/jash-path-cache.XXXXXX")"
trap 'rm -rf "$T"' EXIT INT TERM
HOME_DIR="$T/home"
D1="$T/bin1"
D2="$T/bin2"
mkdir -p "$HOME_DIR/.jash" "$D1" "$D2"

make_cmd() {
    local path="$1" output="$2"
    printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$output" >"$path"
    chmod 0755 "$path"
}

make_cmd "$D1/pc_one" ONE
make_cmd "$D1/pc_two" TWO
make_cmd "$D1/pc_swap" FIRST
make_cmd "$D2/pc_swap" SECOND
make_cmd "$D1/pc_stale" STALE
make_cmd "$D2/pc_stale" FALLBACK
make_cmd "$D1/pc_pipe" PIPE_STALE
make_cmd "$D2/pc_pipe" PIPE_FALLBACK
make_cmd "$D1/pc_sub" SUB_STALE
make_cmd "$D2/pc_sub" SUB_FALLBACK
make_cmd "$D2/pc_negative" NEGATIVE_OK

run() {
    /usr/bin/env -i HOME="$HOME_DIR" PATH="$D1:$D2:/usr/bin:/bin" \
        JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_RECALL_GUARD=0 \
        JASH_RISK_CHECKPOINT=0 "$JASH" -c "$1"
}

cache="$HOME_DIR/.jash/path_cache.dat"

if [ "$(run pc_one)" != ONE ]; then
    echo "FAIL: initial PATH lookup failed" >&2
    exit 1
fi
if [ ! -f "$cache" ]; then
    echo "FAIL: one positive lookup was not flushed at process cleanup" >&2
    exit 1
fi

size_one="$(stat -c %s "$cache")"
sum_one="$(cksum "$cache")"
if [ "$size_one" -gt 1024 ]; then
    echo "FAIL: one cache entry rewrote an oversized cache file ($size_one bytes)" >&2
    exit 1
fi

if [ "$(run pc_one)" != ONE ]; then
    echo "FAIL: persisted positive cache hit failed" >&2
    exit 1
fi
if [ "$(cksum "$cache")" != "$sum_one" ]; then
    echo "FAIL: a clean cache hit rewrote the persistent cache" >&2
    exit 1
fi

if [ "$(run pc_two)" != TWO ]; then
    echo "FAIL: second PATH lookup failed" >&2
    exit 1
fi
size_two="$(stat -c %s "$cache")"
if [ "$size_two" -le "$size_one" ] || [ $((size_two - size_one)) -gt 512 ]; then
    echo "FAIL: second entry was not persisted as one bounded record" >&2
    exit 1
fi

sum_two="$(cksum "$cache")"
run pc_missing_xyz >/dev/null 2>&1 || true
if [ "$(cksum "$cache")" != "$sum_two" ]; then
    echo "FAIL: a negative lookup caused persistent disk I/O" >&2
    exit 1
fi

swap_out="$(run "pc_swap; PATH=$D2:$D1:/usr/bin:/bin; pc_swap")"
if [ "$swap_out" != $'FIRST\nSECOND' ]; then
    echo "FAIL: PATH change reused a stale positive entry: [$swap_out]" >&2
    exit 1
fi

negative_out="$(run "PATH=$D1:/usr/bin:/bin; pc_negative 2>/dev/null; PATH=$D2:$D1:/usr/bin:/bin; pc_negative")"
if [ "$negative_out" != NEGATIVE_OK ]; then
    echo "FAIL: PATH change reused a stale negative entry: [$negative_out]" >&2
    exit 1
fi

if [ "$(run pc_stale)" != STALE ]; then
    echo "FAIL: stale-path setup did not resolve the first executable" >&2
    exit 1
fi
/bin/rm -f "$D1/pc_stale"
run pc_stale >/dev/null 2>&1 || true
stale_out="$(run pc_stale)"
if [ "$stale_out" != FALLBACK ]; then
    echo "FAIL: failed exec did not invalidate the disappeared path: [$stale_out]" >&2
    exit 1
fi

if [ "$(run 'pc_pipe | /bin/cat')" != PIPE_STALE ]; then
    echo "FAIL: pipeline stale-path setup failed" >&2
    exit 1
fi
/bin/rm -f "$D1/pc_pipe"
run 'pc_pipe | /bin/cat' >/dev/null 2>&1 || true
if [ "$(run 'pc_pipe | /bin/cat')" != PIPE_FALLBACK ]; then
    echo "FAIL: pipeline exec failure did not invalidate its PATH entry" >&2
    exit 1
fi

if [ "$(run '(pc_sub)')" != SUB_STALE ]; then
    echo "FAIL: subshell stale-path setup failed" >&2
    exit 1
fi
/bin/rm -f "$D1/pc_sub"
run '(pc_sub)' >/dev/null 2>&1 || true
if [ "$(run '(pc_sub)')" != SUB_FALLBACK ]; then
    echo "FAIL: subshell exec failure did not invalidate its PATH entry" >&2
    exit 1
fi

CON_HOME="$T/concurrent-home"
mkdir -p "$CON_HOME/.jash"
pids=""
for i in 1 2 3 4 5 6 7 8; do
    cmd="$D1/pc_con_$i"
    printf '#!/bin/sh\n/bin/sleep 0.2\nprintf "CON_%s\\n"\n' "$i" >"$cmd"
    chmod 0755 "$cmd"
    /usr/bin/env -i HOME="$CON_HOME" PATH="$D1:/usr/bin:/bin" \
        JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_RECALL_GUARD=0 \
        JASH_RISK_CHECKPOINT=0 "$JASH" -c "pc_con_$i" \
        >/dev/null 2>&1 &
    pids="$pids $!"
done
for pid in $pids; do
    wait "$pid"
done
con_cache="$CON_HOME/.jash/path_cache.dat"
con_count="$(od -An -tu4 -j4 -N4 "$con_cache" | tr -d ' ')"
if [ "$con_count" != 8 ]; then
    echo "FAIL: concurrent jash processes lost PATH-cache records ($con_count/8)" >&2
    exit 1
fi

echo "PASS: PATH cache uses incremental persistence and exec-failure invalidation"
