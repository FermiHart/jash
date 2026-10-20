#!/usr/bin/env bash
# Interactive history must load before readline, survive exit, and rank
# autosuggestions by recency in one reverse pass.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$ROOT/build/jash"

if [ ! -x "$JASH" ]; then
    echo "FAIL: build/jash not found; build the jash target first" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 is required for the interactive PTY test"
    exit 0
fi

T="$(mktemp -d "${TMPDIR:-/tmp}/jash-history-autosuggest.XXXXXX")"
trap 'rm -rf "$T"' EXIT INT TERM
HOME_DIR="$T/home"
mkdir -p "$HOME_DIR"

old="$T/pick-old"
recent="$T/pick-recent"
prefix="touch $T/pick-"
{
    printf '%s\n' 'HISTORY_SENTINEL_DO_NOT_DROP'
    printf 'touch %s\n' "$old"
    printf '%s\n' 'true'
    printf 'touch %s\n' "$old"
    printf '%s\n' 'true'
    printf 'touch %s\n' "$old"
    printf 'touch %s\n' "$recent"
} >"$HOME_DIR/.jash_history"

python3 - "$JASH" "$HOME_DIR" "$prefix" <<'PY'
import os
import pty
import select
import signal
import sys
import time

jash, home, prefix = sys.argv[1:]
base = prefix[len("touch "):]
old = base + "old"
recent = base + "recent"
pid, fd = pty.fork()
if pid == 0:
    os.environ.clear()
    os.environ.update({
        "HOME": home,
        "PATH": "/usr/bin:/bin",
        "TERM": "dumb",
        "JASH_AUTOSUGGEST": "1",
        "JASH_QUIET": "1",
        "JASH_SIGIL": "0",
        "JASH_STATUSBAR": "0",
    })
    os.execv(jash, ["jash"])

def drain(seconds):
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            os.read(fd, 65536)
        except OSError:
            return

try:
    drain(1.0)
    os.write(fd, prefix.encode())
    drain(0.2)
    os.write(fd, b"\x1b[C\n")
    command_deadline = time.time() + 5.0
    while time.time() < command_deadline:
        if os.path.exists(old) or os.path.exists(recent):
            break
        drain(0.1)
    else:
        raise RuntimeError("autosuggested command did not execute")
    drain(0.5)
    os.write(fd, b"\x04")
    deadline = time.time() + 5.0
    while time.time() < deadline:
        waited, _ = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            break
        drain(0.1)
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

if [ ! -e "$recent" ]; then
    echo "FAIL: autosuggest did not select the most recent prefix match" >&2
    exit 1
fi
if [ -e "$old" ]; then
    echo "FAIL: autosuggest selected the older, more frequent match" >&2
    exit 1
fi
if ! grep -qxF 'HISTORY_SENTINEL_DO_NOT_DROP' "$HOME_DIR/.jash_history"; then
    echo "FAIL: interactive exit discarded pre-existing history" >&2
    exit 1
fi
if ! grep -qxF "touch $recent" "$HOME_DIR/.jash_history"; then
    echo "FAIL: interactive command was not persisted" >&2
    exit 1
fi

echo "PASS: interactive history loads, persists, and autosuggests by recency"
