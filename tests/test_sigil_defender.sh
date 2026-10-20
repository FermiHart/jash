#!/usr/bin/env bash
# Sigil Defender must remain an idle-only easter egg, fit the terminal, and
# never reserve `bear`, which belongs to the external Bear compiler.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$ROOT/build/jash"

if grep -qE '^\s*\{\s*"bear"' "$ROOT/c/builtins/registry.c"; then
    echo "FAIL: bear is still registered as a jash builtin" >&2
    exit 1
fi
if ! grep -q 'SIGIL DEFENDER' "$ROOT/c/easter/bear_egg.c"; then
    echo "FAIL: idle easter egg is not Sigil Defender" >&2
    exit 1
fi
for marker in 'LEVEL %02d' 'PHASE SHIFT' 'W/S or arrows aim' 'threat_lane'; do
    if ! grep -qF "$marker" "$ROOT/c/easter/bear_egg.c"; then
        echo "FAIL: Sigil Defender is missing progression marker: $marker" >&2
        exit 1
    fi
done
if [ ! -x "$JASH" ]; then
    echo "FAIL: build/jash not found; build the jash target first" >&2
    exit 1
fi
if JASH_SIGIL=0 "$JASH" -c 'type bear' 2>&1 | grep -q 'shell builtin'; then
    echo "FAIL: runtime still resolves bear as a shell builtin" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 is required for the interactive PTY test"
    exit 0
fi

T="$(mktemp -d "${TMPDIR:-/tmp}/jash-sigil-defender.XXXXXX")"
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/home" "$T/bin"
printf '%s\n' '#!/bin/sh' 'printf BEAR_COMPILER_PATH' >"$T/bin/bear"
chmod 755 "$T/bin/bear"
external="$(PATH="$T/bin:/usr/bin:/bin" JASH_SIGIL=0 "$JASH" -c bear)"
if [ "$external" != "BEAR_COMPILER_PATH" ]; then
    echo "FAIL: bear did not resolve to the external compiler path" >&2
    exit 1
fi
"${CC:-cc}" -std=gnu11 -I"$ROOT/c" \
    "$ROOT/tests/test_sigil_defender_logic.c" -o "$T/defender-logic"
if ! "$T/defender-logic"; then
    echo "FAIL: phase-shift changed lanes or repeated continuously" >&2
    exit 1
fi

python3 - "$JASH" "$T/home" "$T/after-game" <<'PY'
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

jash, home, after_game = sys.argv[1:]
rows, cols = 16, 44
pid, fd = pty.fork()
if pid == 0:
    os.environ.clear()
    os.environ.update({
        "HOME": home,
        "PATH": "/usr/bin:/bin",
        "TERM": "xterm-256color",
        "JASH_BEAR_EGG": "1",
        "JASH_BEAR_IDLE_SEC": "1",
        "JASH_PROMPT_SPINNER": "0",
        "JASH_SIGIL": "0",
        "JASH_STATUSBAR": "0",
    })
    os.chdir(home)  # keep the post-game prompt outside the repository
    os.execv(jash, ["jash"])

fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
output = bytearray()

def collect(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ready, _, _ = select.select([fd], [], [], min(0.05, end - time.monotonic()))
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        output.extend(chunk)

def wait_for(marker, seconds):
    end = time.monotonic() + seconds
    while marker not in output and time.monotonic() < end:
        collect(0.1)
    if marker not in output:
        raise RuntimeError("missing terminal marker: " + marker.decode("ascii"))

def wait_for_after(marker, offset, seconds):
    end = time.monotonic() + seconds
    while marker not in output[offset:] and time.monotonic() < end:
        collect(0.1)
    if marker not in output[offset:]:
        raise RuntimeError("missing terminal transition: " + repr(marker))

try:
    wait_for(b"Space starts Sigil Defender", 4.0)
    game_offset = len(output)
    os.write(fd, b" ")
    wait_for_after(b"[ SIGIL DEFENDER ]", game_offset, 2.0)
    wait_for_after(b"LEVEL 01", game_offset, 2.0)
    for color in (b"\x1b[1;36m", b"\x1b[1;33m", b"\x1b[1;31m"):
        wait_for_after(color, game_offset, 2.0)
    os.write(fd, b"w")
    wait_for(b"AIM upper", 2.0)
    os.write(fd, b"s")
    wait_for(b"AIM middle", 2.0)
    os.write(fd, b" ")
    wait_for(b"SIGIL FIRED", 2.0)

    sealed_offset = len(output)
    wait_for_after(b"SEALED 1", sealed_offset, 3.0)
    os.write(fd, b"w ")
    sealed_offset = len(output)
    wait_for_after(b"SEALED 2", sealed_offset, 3.0)
    os.write(fd, b"ss ")
    sealed_offset = len(output)
    wait_for_after(b"SEALED 3", sealed_offset, 3.0)
    os.write(fd, b"w ")
    sealed_offset = len(output)
    wait_for_after(b"SEALED 4", sealed_offset, 3.0)
    wait_for_after(b"LEVEL 02", sealed_offset, 3.0)
    wait_for_after(b"TARGET JUMPED", sealed_offset, 4.0)

    exit_offset = len(output)
    os.write(fd, b"\x1b")
    wait_for_after(b"\x1b[?1049l", exit_offset, 2.0)

    # Short relative fixture path: prompt redraw may consult git on every
    # keystroke, and the runner's private TMPDIR can be very long.
    os.write(fd, b"touch ../after-game\r")
    deadline = time.monotonic() + 5.0
    while not os.path.exists(after_game) and time.monotonic() < deadline:
        collect(0.1)
    if not os.path.exists(after_game):
        raise RuntimeError("prompt did not accept a command after game exit: " +
                           repr(bytes(output[-512:])))

    for match in re.finditer(rb"\x1b\[([0-9]+);([0-9]+)H", output):
        row, col = map(int, match.groups())
        if row > rows or col > cols:
            raise RuntimeError(f"render escaped terminal bounds: row={row} col={col}")

    os.write(fd, b"\x04")
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        waited, _ = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            break
        collect(0.1)
    else:
        raise RuntimeError("interactive jash did not exit")
finally:
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        os.close(fd)
    except OSError:
        pass
PY

echo "PASS: Sigil Defender starts on idle, fires, fits, exits, and leaves bear external"
