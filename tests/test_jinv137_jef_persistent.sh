#!/usr/bin/env bash
# JINV-137 — verifier persistente preserva identidade, binding e TOCTOU.
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2
if test "$(uname)" != "Linux" || test ! -e /proc/self/fd; then
    echo "SKIP: sealed persistent verifier requires Linux procfs"
    exit 0
fi
JASH="${JASH_AGENTD:-./build/jash-agentd}"
VERIFY="${JEF_VERIFY:-./build/jef_verify}"
test -x "$JASH" || { echo "SKIP: $JASH ausente"; exit 0; }
test -x "$VERIFY" || { echo "SKIP: $VERIFY ausente"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

PYTHON_BIN=$(command -v python3 || command -v python)
"$PYTHON_BIN" - "$JASH" "$VERIFY" "$TMP" <<'PY'
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

jash, verifier, root_text = sys.argv[1:]
root = Path(root_text)
home = root / "home"
cwd = root / "cwd"
home.mkdir()
cwd.mkdir()
env = {"HOME": str(home), "JASH_SIGIL": "0", "PATH": "/usr/bin:/bin"}

requests = [
    {"id": "inline", "argv": ["echo", "OK"], "cwd": str(cwd),
     "caps": ["effect:read"]},
    {"id": "overflow", "argv": ["head", "-c", "40000", "/dev/zero"],
     "cwd": str(cwd), "caps": ["effect:exec"]},
]
request_lines = [json.dumps(row, separators=(",", ":")) + "\n"
                 for row in requests]
run = subprocess.run(
    [jash], input="".join(request_lines), text=True, capture_output=True,
    check=True, env=env,
)
lines = run.stdout.splitlines(keepends=True)
assert len(lines) == 3, lines
handshake = json.loads(lines[0])
responses = [json.loads(line) for line in lines[1:]]
assert handshake["runtime"] == "jash-agentd", handshake
assert responses[0]["stdout"] == "OK\n", responses[0]
assert responses[1]["stdout_bytes"] == 40000, responses[1]

def sealed(payload: bytes, label: str) -> int:
    fd = os.memfd_create(label, os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    view = memoryview(payload)
    while view:
        written = os.write(fd, view)
        assert written > 0
        view = view[written:]
    os.lseek(fd, 0, os.SEEK_SET)
    fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
                fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
    return fd

def artifacts(index: int):
    response = responses[index]
    receipt = home / ".jash" / "agent_receipts" / response["receipt_ref"]
    fds = [
        sealed(request_lines[index].encode(), f"request-{index}"),
        sealed(lines[index + 1].encode(), f"response-{index}"),
        sealed(receipt.read_bytes(), f"receipt-{index}"),
    ]
    stream_fds = []
    for tag in ("stdout", "stderr"):
        ref = response.get(f"{tag}_ref")
        if ref:
            path = home / ".jash" / "overflow" / ref
            stream_fds.append(sealed(path.read_bytes(), f"{tag}-{index}"))
        else:
            stream_fds.append(-1)
    return response, fds + stream_fds

inline, inline_fds = artifacts(0)
overflow, overflow_fds = artifacts(1)
proc = subprocess.Popen(
    [verifier, "runtime-server", handshake["receipts"]["pubkey"],
     str(os.getpid())],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True,
)
assert proc.stdin and proc.stdout
ready = proc.stdout.readline().strip().split("\t")
assert ready[:3] == ["JEFV", "1", "READY"], ready
verifier_pid = int(ready[3])
assert verifier_pid == proc.pid, ready

def verify(seq: int, fds: list[int], ref: str) -> str:
    fields = ["V", str(seq)] + [str(fd) if fd >= 0 else "-" for fd in fds] + [ref]
    proc.stdin.write("\t".join(fields) + "\n")
    proc.stdin.flush()
    response = proc.stdout.readline().strip()
    assert proc.pid == verifier_pid and proc.poll() is None
    return response

observed = verify(1, inline_fds, inline["receipt_ref"])
assert observed == "R\t1\tVALID", observed
observed = verify(2, overflow_fds, overflow["receipt_ref"])
assert observed == "R\t2\tVALID", observed

# A resposta adulterada, o overflow omitido e um memfd sem seals falham alto.
tampered = dict(inline)
tampered["rc"] = 9
bad_response = sealed((json.dumps(tampered, separators=(",", ":")) + "\n").encode(),
                      "tampered-response")
bad_fds = [inline_fds[0], bad_response, *inline_fds[2:]]
observed = verify(3, bad_fds, inline["receipt_ref"])
assert observed == "R\t3\tINVALID", observed
missing_stream = overflow_fds[:]
missing_stream[3] = -1
observed = verify(4, missing_stream, overflow["receipt_ref"])
assert observed == "R\t4\tINVALID", observed
unsealed = os.memfd_create("unsealed", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
os.write(unsealed, lines[1].encode())
unsealed_fds = [inline_fds[0], unsealed, *inline_fds[2:]]
observed = verify(5, unsealed_fds, inline["receipt_ref"])
assert observed == "R\t5\tERROR", observed

proc.stdin.write("not-a-frame\n")
proc.stdin.flush()
observed = proc.stdout.readline().strip()
assert observed == "R\t-\tERROR", observed
extra = ["V", "6"] + [str(fd) if fd >= 0 else "-" for fd in inline_fds]
proc.stdin.write("\t".join(extra + [inline["receipt_ref"], "extra"]) + "\n")
proc.stdin.flush()
observed = proc.stdout.readline().strip()
assert observed == "R\t-\tERROR", observed

valid_after_nul = "\t".join(
    ["V", "7"] + [str(fd) if fd >= 0 else "-" for fd in inline_fds] +
    [inline["receipt_ref"]]
)
proc.stdin.write("bad\0hidden\n" + valid_after_nul + "\n")
proc.stdin.flush()
observed = proc.stdout.readline().strip()
assert observed == "R\t-\tERROR", observed
observed = proc.stdout.readline().strip()
assert observed == "R\t7\tVALID", observed
observed = verify(8, inline_fds, inline["receipt_ref"])
assert observed == "R\t8\tVALID", observed

fd_count = len(list(Path(f"/proc/{proc.pid}/fd").iterdir()))
for seq in range(9, 29):
    observed = verify(seq, inline_fds, inline["receipt_ref"])
    assert observed == f"R\t{seq}\tVALID", observed
assert len(list(Path(f"/proc/{proc.pid}/fd").iterdir())) == fd_count

proc.stdin.close()
assert proc.wait(timeout=5) == 0, proc.stderr.read()

# O verifier pertence ao processo que o criou; nao pode sobreviver para abrir
# FDs de um PID reciclado depois que o pai morrer.
helper_code = r'''
import os, subprocess, sys
p = subprocess.Popen(
    [sys.argv[1], "runtime-server", sys.argv[2], str(os.getpid())],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
)
assert p.stdout and p.stdout.readline().startswith("JEFV\t1\tREADY\t")
print(p.pid, flush=True)
os._exit(0)
'''
helper = subprocess.run(
    [sys.executable, "-c", helper_code, verifier,
     handshake["receipts"]["pubkey"]],
    text=True, capture_output=True, check=True,
)
orphan = int(helper.stdout.strip())
for _ in range(100):
    if not Path(f"/proc/{orphan}").exists():
        break
    time.sleep(0.01)
else:
    raise AssertionError(f"persistent verifier survived parent death: {orphan}")

for fd in set(inline_fds + overflow_fds + [bad_response, unsealed]):
    if fd >= 0:
        os.close(fd)
PY

echo "PASS: persistent jef verifier is sealed, reusable, and overflow-complete"
