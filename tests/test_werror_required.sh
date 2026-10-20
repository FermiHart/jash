#!/usr/bin/env bash
# A normal configure and every bundled-module build must reject C warnings.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash-werror-required.XXXXXX")"
trap 'rm -rf "$T"' EXIT

if cmake -S "$ROOT" -B "$T/off" -DJASH_WERROR=OFF >"$T/off.log" 2>&1; then
    printf 'FAIL: configure accepted JASH_WERROR=OFF\n' >&2
    exit 1
fi
if ! grep -q 'requires -Werror' "$T/off.log"; then
    printf 'FAIL: configure failed for a reason other than mandatory -Werror\n' >&2
    exit 1
fi
cmake -S "$ROOT" -B "$T/on" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >"$T/on.log" 2>&1
python3 -B - "$T/on/compile_commands.json" <<'PY'
import json
import shlex
import sys
commands = json.load(open(sys.argv[1], encoding="utf-8"))
c_commands = [row for row in commands if row["file"].endswith(".c")]
assert c_commands, "CMake exported no C translation units"
assert all("-Werror" in shlex.split(row["command"]) for row in c_commands), \
    "a project C translation unit omitted -Werror"
PY

mkdir -p "$T/home" "$T/warn"
printf 'int warning_fixture(void) { int unused = 1; return 0; }\n' >"$T/warn/warn_module.c"
printf 'name = "warn"\n' >"$T/warn/manifest.toml"
if HOME="$T/home" bash "$ROOT/modules/build_module.sh" "$T/warn" \
        >"$T/module.log" 2>&1; then
    printf 'FAIL: module build accepted a C warning\n' >&2
    exit 1
fi
if ! grep -q 'error: unused variable' "$T/module.log" ||
        test -e "$T/warn/warn.modjash"; then
    printf 'FAIL: module did not refuse the warning before packaging\n' >&2
    exit 1
fi
printf 'Werror required: CMake and modules reject warnings\n'
