#!/usr/bin/env bash
# Correctness and deterministic work-count gates; wall-clock timing is separate.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-perf-waves.XXXXXX")"
trap 'rm -rf "$T"' EXIT
if [[ "$(uname -s)" == Linux ]]; then
    "${CC:-cc}" -std=gnu11 -O2 -ffunction-sections -fdata-sections -I"$ROOT/c" \
        "$ROOT/tests/test_perf_startup_argv.c" "$ROOT/c/core/argv.c" \
        "$ROOT/c/builtins/registry.c" "$ROOT/c/parser/cmdsub.c" \
        -Wl,--gc-sections -Wl,--wrap=malloc,--wrap=realloc,--wrap=free \
        -o "$T/argv-test"
    "$T/argv-test"
else
    printf '%s\n' 'SKIP: isolated argv allocation test requires GNU linker wrapping'
fi
python3 -B "$ROOT/tests/test_bench_eval_core.py"
python3 -B "$ROOT/tests/test_perf_printf_expansion.py"
bash "$ROOT/tests/test_inert_word_layer.sh"
python3 -B "$ROOT/tests/test_perf_function_lists.py"
python3 -B "$ROOT/tests/test_perf_pipe_redir.py"
python3 -B "$ROOT/tests/test_redir_direct.py"
python3 -B "$ROOT/tests/test_redir_dispatch.py"
python3 -B "$ROOT/tests/test_read_regular.py"
bash "$ROOT/tests/test_read_regular_unit.sh"
python3 -B "$ROOT/tests/test_append_undo.py"
bash "$ROOT/tests/test_append_backup_unit.sh"
python3 -B "$ROOT/tests/test_sha256_streaming.py"
bash "$ROOT/tests/test_sha256_backends.sh"
bash "$ROOT/tests/test_perf_env_fschange.sh"
