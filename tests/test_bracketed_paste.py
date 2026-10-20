#!/usr/bin/env python3
"""Bracketed paste is literal, deferred, and painted once."""
import os
from pathlib import Path
import pty
import select
import signal
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
JASH = ROOT / "build/jash"


def drain(fd, seconds):
    output = bytearray()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            output.extend(os.read(fd, 65536))
        except OSError:
            break
    return bytes(output)


def main():
    if not JASH.is_file():
        raise SystemExit("build/jash not found; build the jash target first")
    with tempfile.TemporaryDirectory(prefix="jash-bracketed-paste-") as temp:
        target = Path(temp) / "literal.txt"
        pid, fd = pty.fork()
        if pid == 0:
            env = {
                "HOME": temp,
                "PATH": "/usr/bin:/bin",
                "TERM": "dumb",
                "JASH_AUTOSUGGEST": "0",
                "JASH_QUIET": "1",
                "JASH_SIGIL": "0",
                "JASH_STATUSBAR": "0",
            }
            os.execve(JASH, ["jash"], env)
        try:
            drain(fd, 1.0)
            command = f"printf '%s\\n' '(' > '{target}'"
            os.write(fd, b"\x1b[200~" + command.encode() + b"\x1b[201~")
            pasted = drain(fd, 0.5)
            assert not target.exists(), "paste executed before Enter"
            redraws = pasted.count(b"\r\x1b[K") + pasted.count(b"\r\x1b[0K")
            assert redraws == 1, f"paste produced {redraws} redraws"

            os.write(fd, b"\r")
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and not target.exists():
                drain(fd, 0.05)
            assert target.read_text() == "(\n", "paste applied editor auto-pairing"
            os.write(fd, b"\x04")
        finally:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
            os.close(fd)
    print("PASS: bracketed paste is literal, deferred, and painted once")


if __name__ == "__main__":
    main()
