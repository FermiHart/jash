#!/usr/bin/env bash
# Protocol v2 is opt-in; legacy requests remain one-response v1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${JASH_AGENTD_BIN:-$ROOT/build/jash-agentd}"
PYTHON_BIN="$(command -v python3 || command -v python)"
test -x "$BIN"

TMP="${TMPDIR:-/tmp}/jash-agent-protocol-v2.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

"$PYTHON_BIN" - "$BIN" "$PYTHON_BIN" "$TMP" <<'PY'
import base64
import json
import os
import re
import subprocess
import sys
import time

binary, python, root = sys.argv[1:]


def daemon(name):
    home = os.path.join(root, name)
    os.makedirs(home, exist_ok=True)
    proc = subprocess.Popen(
        [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env={**os.environ, "HOME": home},
        text=True,
    )
    assert proc.stdin and proc.stdout and proc.stderr
    handshake = json.loads(proc.stdout.readline())
    return proc, handshake


def finish(proc):
    proc.stdin.close()
    rows = [json.loads(line) for line in proc.stdout if line.strip()]
    stderr = proc.stderr.read()
    rc = proc.wait(timeout=20)
    assert rc == 0, (rc, stderr, rows[-3:])
    return rows


hash_re = re.compile(r"^[0-9a-f]{64}$")

# The handshake distinguishes default-v1 features from tested v2 features.
proc, handshake = daemon("handshake")
assert handshake["request_version_default"] == 1, handshake
v2 = handshake["protocol_features"]["2"]
assert v2 == {
    "streaming": True,
    "binary_stdin": True,
    "byte_exact_base64": True,
    "byte_argv": True,
    "byte_paths": True,
    "pipe_backpressure": True,
    "terminal_output_hashes": True,
    "content_addressed_output_refs": True,
}, v2
assert {"request_version", "protocol_version", "stdin_b64", "stdin_mode"} <= set(
    handshake["fields"]
), handshake
assert "stdin_mode" not in handshake["response_fields"], handshake
assert finish(proc) == []

# No version remains byte-for-byte shaped as v1: one terminal response, no
# event framing, and text output stays in the legacy stdout field.
proc, _ = daemon("legacy")
proc.stdin.write(json.dumps({
    "id": "legacy", "argv": ["echo", "V1_OK"],
    "caps": ["effect:read"],
}) + "\n")
rows = finish(proc)
assert len(rows) == 1 and rows[0]["id"] == "legacy", rows
assert "event" not in rows[0] and rows[0]["stdout"] == "V1_OK\n", rows

# Binary stdin, stdout, and stderr survive JSONL exactly. Chunk sequence is
# global per request, making interleaving deterministic and reconstructible.
stdin_bytes = b"\x00\xffA\n"
stderr_bytes = b"\x80ERR\x00"
code = (
    "import os; data=os.read(0,9999); os.write(1,data); "
    f"os.write(2,{stderr_bytes!r})"
)
proc, _ = daemon("binary")
proc.stdin.write(json.dumps({
    "id": "binary", "request_version": 2,
    "argv": [python, "-c", code],
    "stdin_b64": base64.b64encode(stdin_bytes).decode(),
    "caps": ["effect:exec"],
}) + "\n")
rows = finish(proc)
assert rows[0]["event"] == "start" and rows[-1]["event"] == "result", rows
chunks = [row for row in rows if row.get("event") == "chunk"]
assert [row["seq"] for row in chunks] == list(range(len(chunks))), chunks
streams = {"stdout": bytearray(), "stderr": bytearray()}
for row in chunks:
    streams[row["stream"]] += base64.b64decode(row["data_b64"], validate=True)
assert bytes(streams["stdout"]) == stdin_bytes, streams
assert bytes(streams["stderr"]) == stderr_bytes, streams
result = rows[-1]
assert result["stdout_bytes"] == len(stdin_bytes), result
assert result["stderr_bytes"] == len(stderr_bytes), result
assert hash_re.fullmatch(result["stdout_blake3"]), result
assert hash_re.fullmatch(result["stderr_blake3"]), result
assert result["stdout_ref"] == (
    f"blake3-{result['stdout_blake3']}-{result['stdout_bytes']}.bin"
), result
assert result["stderr_ref"] == (
    f"blake3-{result['stderr_blake3']}-{result['stderr_bytes']}.bin"
), result
with open(os.path.join(root, "binary", ".jash", "overflow",
                       result["stdout_ref"]), "rb") as stream:
    assert stream.read() == stdin_bytes
with open(os.path.join(root, "binary", ".jash", "overflow",
                       result["stderr_ref"]), "rb") as stream:
    assert stream.read() == stderr_bytes
assert result["receipt_ref"] and result["receipt_blake3"], result

# Identical complete streams share one immutable object, both within a request
# and across distinct requests. Final names contain no process/request nonce.
same = b"DEDUP\x00BYTES"
same_code = f"import os; data={same!r}; os.write(1,data); os.write(2,data)"
proc, _ = daemon("dedup")
for request_id in ("dedup-a", "dedup-b"):
    proc.stdin.write(json.dumps({
        "id": request_id, "request_version": 2,
        "argv": [python, "-c", same_code], "caps": ["effect:exec"],
    }) + "\n")
rows = finish(proc)
dedup_results = [row for row in rows if row.get("event") == "result"]
assert len(dedup_results) == 2, dedup_results
refs = {row["stdout_ref"] for row in dedup_results}
refs.update(row["stderr_ref"] for row in dedup_results)
assert len(refs) == 1, (refs, dedup_results)
dedup_ref = refs.pop()
assert str(os.getpid()) not in dedup_ref and "dedup" not in dedup_ref, dedup_ref
overflow = os.path.join(root, "dedup", ".jash", "overflow")
assert os.listdir(overflow) == [dedup_ref], os.listdir(overflow)
with open(os.path.join(overflow, dedup_ref), "rb") as stream:
    assert stream.read() == same


def cas_result(home_name, request_id, payload):
    proc, _ = daemon(home_name)
    proc.stdin.write(json.dumps({
        "id": request_id, "request_version": 2,
        "argv": [python, "-c", f"import os; os.write(1,{payload!r})"],
        "caps": ["effect:exec"],
    }) + "\n")
    rows = finish(proc)
    return rows, [row for row in rows if row.get("event") == "result"]


# EEXIST is a dedup hit only after regular-file, size, and hash verification.
# A corrupted object poisons no result/receipt: the request fails closed.
payload = b"TAMPER-CAS"
_, valid = cas_result("tamper", "tamper-good", payload)
assert len(valid) == 1, valid
tamper_path = os.path.join(root, "tamper", ".jash", "overflow",
                           valid[0]["stdout_ref"])
with open(tamper_path, "wb") as stream:
    stream.write(b"X" * len(payload))
rows, invalid = cas_result("tamper", "tamper-recheck", payload)
assert not invalid, rows
assert rows[-1].get("error") == "receipt commit failed", rows[-1]
assert rows[-1].get("outcome") == "indeterminate", rows[-1]

# O_NOFOLLOW is enforced even when a symlink target has the expected bytes.
_, valid = cas_result("symlink", "symlink-good", payload)
assert len(valid) == 1, valid
symlink_path = os.path.join(root, "symlink", ".jash", "overflow",
                            valid[0]["stdout_ref"])
target = os.path.join(root, "symlink-target")
with open(target, "wb") as stream:
    stream.write(payload)
os.unlink(symlink_path)
os.symlink(target, symlink_path)
rows, invalid = cas_result("symlink", "symlink-recheck", payload)
assert not invalid, rows
assert rows[-1].get("error") == "receipt commit failed", rows[-1]
assert rows[-1].get("outcome") == "indeterminate", rows[-1]

# Decoder and negotiation fail closed. The protocol_version spelling is an
# equivalent opt-in and a valid request after rejections still executes.
proc, _ = daemon("fail-closed")
requests = [
    {"id": "bad-bits", "request_version": 2, "argv": ["cat"],
     "stdin_b64": "AB==", "caps": ["effect:read"]},
    {"id": "exclusive", "request_version": 2, "argv": ["cat"],
     "stdin": "x", "stdin_b64": "eA==", "caps": ["effect:read"]},
    {"id": "needs-v2", "argv": ["cat"], "stdin_b64": "eA==",
     "caps": ["effect:read"]},
    {"id": "alias", "protocol_version": 2, "argv": ["cat"],
     "stdin_b64": "eA==", "caps": ["effect:read"]},
]
for request in requests:
    proc.stdin.write(json.dumps(request) + "\n")
rows = finish(proc)
errors = {row["id"]: row["error"] for row in rows if "error" in row}
assert set(errors) == {"bad-bits", "exclusive", "needs-v2"}, (errors, rows)
alias = [row for row in rows if row.get("id") == "alias"]
assert alias[0]["event"] == "start" and alias[-1]["event"] == "result", alias
alias_out = b"".join(base64.b64decode(row["data_b64"])
                     for row in alias if row.get("stream") == "stdout")
assert alias_out == b"x", alias

# Repeating the exact v2 request replays the signed terminal commitment but
# never repeats its effect. The replay start explicitly identifies this path.
marker = os.path.join(root, "exactly-once-marker")
request = {
    "id": "same-v2", "request_version": 2,
    "argv": ["sh", "-c", f"printf x >> {marker}; printf RECOVER"],
    "caps": ["effect:exec"],
}
wire = json.dumps(request, separators=(",", ":")) + "\n"
proc, exact_handshake = daemon("exactly-once")
proc.stdin.write(wire)
proc.stdin.flush()
first = []
terminal_wire = ""
while True:
    raw = proc.stdout.readline()
    row = json.loads(raw)
    first.append(row)
    if row.get("event") == "result":
        terminal_wire = raw
        break
proc.stdin.write(wire)
rows = finish(proc)
assert rows[0]["event"] == "start" and rows[0]["replayed"] is True, rows
assert rows[-1] == first[-1], (first, rows)
with open(marker, "rb") as stream:
    assert stream.read() == b"x"
request_path = os.path.join(root, "v2-request.json")
response_path = os.path.join(root, "v2-result.json")
with open(request_path, "w", encoding="utf-8") as stream:
    stream.write(wire)
with open(response_path, "w", encoding="utf-8") as stream:
    stream.write(terminal_wire)
receipt_path = os.path.join(
    root, "exactly-once", ".jash", "agent_receipts",
    first[-1]["receipt_ref"],
)
snapshot_output = os.path.join(
    os.path.dirname(os.path.dirname(receipt_path)), "overflow",
    rows[-1]["stdout_ref"],
)
with open(snapshot_output, "rb") as stream:
    assert stream.read() == b"RECOVER"
empty_stderr = os.path.join(
    os.path.dirname(os.path.dirname(receipt_path)), "overflow",
    rows[-1]["stderr_ref"],
)
assert os.path.isfile(empty_stderr) and os.path.getsize(empty_stderr) == 0
verifier = os.path.join(os.path.dirname(binary), "jef_verify")
verified = subprocess.run(
    [verifier, "runtime-receipt", exact_handshake["receipts"]["pubkey"],
     request_path, response_path, receipt_path],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
)
assert verified.returncode == 0, (verified.stdout, verified.stderr)

def verify_with_suffix(source, name):
    path = os.path.join(root, name)
    with open(source, "rb") as stream:
        data = stream.read()
    with open(path, "wb") as stream:
        stream.write(data + b'\x00{"ignored":true}\n')
    paths = {
        "request": request_path,
        "response": response_path,
        "receipt": receipt_path,
    }
    paths[name] = path
    result = subprocess.run(
        [verifier, "runtime-receipt", exact_handshake["receipts"]["pubkey"],
         paths["request"], paths["response"], paths["receipt"]],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode != 0, (name, result.stdout, result.stderr)

verify_with_suffix(request_path, "request")
verify_with_suffix(response_path, "response")
verify_with_suffix(receipt_path, "receipt")

# A client that stops draining JSONL must stop a large producer. Closing stdin
# removes the daemon-loop explanation: without pipe backpressure this process
# would have completed and exited before the observation window.
home = os.path.join(root, "backpressure")
os.makedirs(home, exist_ok=True)
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, env={**os.environ, "HOME": home},
)
assert proc.stdin and proc.stdout and proc.stderr
json.loads(proc.stdout.readline())
large = {
    "id": "backpressure", "request_version": 2,
    "argv": [python, "-c", "import os; os.write(1,b'x'*(8*1024*1024))"],
    "caps": ["effect:exec"],
}
proc.stdin.write((json.dumps(large) + "\n").encode())
proc.stdin.close()
time.sleep(0.5)
assert proc.poll() is None, "producer completed while protocol output was blocked"
wire_rows = proc.stdout.read()
stderr = proc.stderr.read()
assert proc.wait(timeout=30) == 0, stderr
rows = [json.loads(line) for line in wire_rows.splitlines() if line]
assert rows[0]["event"] == "start" and rows[-1]["event"] == "result", rows[-3:]
assert rows[-1]["stdout_bytes"] == 8 * 1024 * 1024, rows[-1]
assert len([row for row in rows if row.get("event") == "chunk"]) > 100, len(rows)
PY

echo "agent protocol v2: PASS"
