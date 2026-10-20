#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${JASH_BUILD_DIR:-"$ROOT/build"}
BIN=${JASH_AGENTD_BIN:-"$BUILD/jash-agentd"}
VERIFY="$BUILD/jef_verify"
PYTHON_BIN=${PYTHON_BIN:-python3}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/jash-libjef-job.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

cmake --build "$BUILD" --target jash-agentd jef_verify -j4 >"$TMP/build.log" 2>&1 || {
    printf '%s\n' "job receipt build failed"
    tail -20 "$TMP/build.log"
    exit 1
}

"$PYTHON_BIN" - "$BIN" "$VERIFY" "$PYTHON_BIN" "$TMP" <<'PY'
import base64
import json
import os
import subprocess
import sys

binary, verifier, python, root = sys.argv[1:]
home = os.path.join(root, "home")
os.makedirs(home)
proc = subprocess.Popen(
    [binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, env={**os.environ, "HOME": home},
)
assert proc.stdin and proc.stdout and proc.stderr
handshake = json.loads(proc.stdout.readline())
pubkey = handshake["receipts"]["pubkey"]

def exchange(body, wire=None):
    encoded = wire if wire is not None else json.dumps(body, separators=(",", ":"))
    proc.stdin.write(encoded + "\n")
    proc.stdin.flush()
    raw = proc.stdout.readline()
    assert raw, proc.stderr.read()
    return json.loads(raw), raw, encoded

start_body = {
    "id": "verify-job", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c", "import os;os.write(1,b'OUT\\x00');os.write(2,b'ERR')"],
    "background": True, "caps": ["effect:exec", "job:create"],
}
start, start_raw, start_wire = exchange(start_body)
wait_body = {
    "id": "verify-wait", "protocol_version": 2, "op": "job.wait",
    "job_id": start["job_id"], "wait_ms": 2000, "caps": ["job:read"],
}
terminal, terminal_raw, wait_wire = exchange(wait_body)
assert terminal["terminal"] and terminal["quiescent"]

request_path = os.path.join(root, "request.json")
wait_request_path = os.path.join(root, "wait-request.json")
start_response = os.path.join(root, "start-response.json")
terminal_response = os.path.join(root, "terminal-response.json")
open(request_path, "w").write(start_wire + "\n")
open(wait_request_path, "w").write(wait_wire + "\n")
open(start_response, "w").write(start_raw)
open(terminal_response, "w").write(terminal_raw)
receipt_dir = os.path.join(home, ".jash", "agent_receipts")
start_receipt = os.path.join(receipt_dir, start["receipt_ref"])
terminal_receipt = os.path.join(receipt_dir, terminal["receipt_ref"])

def verify(phase, response, receipt, key=pubkey, request=None, ref=None):
    if request is None:
        request = request_path if phase == "start" else wait_request_path
    argv = [verifier, "job-receipt", key, phase, request, response, receipt]
    if ref is not None:
        argv.append(ref)
    return subprocess.run(
        argv,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    ).returncode

assert verify("start", start_response, start_receipt) == 0, "valid start receipt rejected"
assert verify("terminal", terminal_response, terminal_receipt) == 0, \
       "valid terminal receipt rejected"

# The same response is an idempotent receipt publication, while another
# terminal operation gets a receipt bound to its own complete response.
terminal_replay, terminal_replay_raw, _ = exchange(wait_body)
assert terminal_replay_raw == terminal_raw
terminal_get, terminal_get_raw, get_wire = exchange({
    "id": "verify-get", "protocol_version": 2, "op": "job.get",
    "job_id": start["job_id"], "caps": ["job:read"],
})
get_response = os.path.join(root, "get-response.json")
get_request = os.path.join(root, "get-request.json")
open(get_request, "w").write(get_wire + "\n")
open(get_response, "w").write(terminal_get_raw)
get_receipt = os.path.join(receipt_dir, terminal_get["receipt_ref"])
assert terminal_get["receipt_ref"] != terminal["receipt_ref"]
assert verify("terminal", get_response, get_receipt, request=get_request) == 0

cancel_body = {
    "id": "verify-cancel", "protocol_version": 2, "op": "job.cancel",
    "job_id": start["job_id"], "grace_ms": 0, "caps": ["job:signal"],
}
terminal_cancel, terminal_cancel_raw, cancel_wire = exchange(cancel_body)
cancel_response = os.path.join(root, "cancel-response.json")
cancel_request = os.path.join(root, "cancel-request.json")
open(cancel_request, "w").write(cancel_wire + "\n")
open(cancel_response, "w").write(terminal_cancel_raw)
cancel_receipt = os.path.join(receipt_dir, terminal_cancel["receipt_ref"])
assert terminal_cancel["cancel_applied"] is False
assert terminal_cancel["receipt_ref"] not in {
    terminal["receipt_ref"], terminal_get["receipt_ref"],
}
assert verify("terminal", cancel_response, cancel_receipt,
              request=cancel_request) == 0

def tampered(name, text):
    path = os.path.join(root, name)
    open(path, "w").write(text)
    return path

def tampered_bytes(name, data):
    path = os.path.join(root, name)
    open(path, "wb").write(data)
    return path

# Action receipts bind the exact mutation request, response, start receipt,
# job identity, operation, and operation-specific data.
action_start_body = {
    "id": "verify-action-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c",
             "import os,time; os.write(1,b'READY\\n'); time.sleep(30)"],
    "background": True, "pty": True,
    "caps": ["effect:exec", "job:create", "job:pty:create"],
}
action_start, _, _ = exchange(action_start_body)

write_body = {
    "id": "verify-action-write", "protocol_version": 2, "op": "job.write",
    "job_id": action_start["job_id"], "data_b64": "AAEC", "wait_ms": 500,
    "caps": ["job:pty:write"],
}
written, written_raw, written_wire = exchange(write_body)
assert written["accepted_bytes"] == 3 and written["receipt_phase"] == "action"
resize_body = {
    "id": "verify-action-resize", "protocol_version": 2,
    "op": "job.resize", "job_id": action_start["job_id"],
    "rows": 37, "cols": 91, "caps": ["job:pty:resize"],
}
resized, resized_raw, resized_wire = exchange(resize_body)
assert resized["rows"] == 37 and resized["cols"] == 91
assert resized["receipt_phase"] == "action"

write_request = os.path.join(root, "write-request.json")
write_response = os.path.join(root, "write-response.json")
resize_request = os.path.join(root, "resize-request.json")
resize_response = os.path.join(root, "resize-response.json")
open(write_request, "w").write(written_wire + "\n")
open(write_response, "w").write(written_raw)
open(resize_request, "w").write(resized_wire + "\n")
open(resize_response, "w").write(resized_raw)
write_receipt = os.path.join(receipt_dir, written["receipt_ref"])
resize_receipt = os.path.join(receipt_dir, resized["receipt_ref"])
assert verify("action", write_response, write_receipt,
              request=write_request) == 0
assert verify("action", resize_response, resize_receipt,
              request=resize_request) == 0
assert written["receipt_ref"].endswith(".job-action.dsse")
assert resized["receipt_ref"].endswith(".job-action.dsse")

bad_action_write = os.path.join(root, "bad-action-write.json")
open(bad_action_write, "w").write(
    written_raw.replace('"accepted_bytes":3', '"accepted_bytes":2'))
assert verify("action", bad_action_write, write_receipt,
              request=write_request) != 0
bad_action_resize = os.path.join(root, "bad-action-resize.json")
open(bad_action_resize, "w").write(
    resized_raw.replace('"cols":91', '"cols":92'))
assert verify("action", bad_action_resize, resize_receipt,
              request=resize_request) != 0

# Non-PTY stdin writes publish the same independently verifiable action
# receipts, bound to the exact capability, job identity, and accepted count.
pipe_start_body = {
    "id": "verify-pipe-start", "protocol_version": 2, "op": "job.start",
    "argv": [python, "-c",
             "import os,time;d=os.read(0,3);os.write(1,d);time.sleep(30)"],
    "background": True, "stdin_mode": "pipe",
    "caps": ["effect:exec", "job:create"],
}
pipe_start, _, _ = exchange(pipe_start_body)
pipe_write_body = {
    "id": "verify-pipe-write", "protocol_version": 2, "op": "job.write",
    "job_id": pipe_start["job_id"], "data_b64": "AAEC", "wait_ms": 500,
    "caps": ["job:stdin:write"],
}
pipe_written, pipe_written_raw, pipe_written_wire = exchange(pipe_write_body)
assert pipe_written["accepted_bytes"] == 3
assert pipe_written["receipt_phase"] == "action"
pipe_write_request = os.path.join(root, "pipe-write-request.json")
pipe_write_response = os.path.join(root, "pipe-write-response.json")
open(pipe_write_request, "w").write(pipe_written_wire + "\n")
open(pipe_write_response, "w").write(pipe_written_raw)
pipe_write_receipt = os.path.join(receipt_dir, pipe_written["receipt_ref"])
assert verify("action", pipe_write_response, pipe_write_receipt,
              request=pipe_write_request) == 0

bad_pipe_count = tampered(
    "bad-pipe-count.json",
    pipe_written_raw.replace('"accepted_bytes":3', '"accepted_bytes":2'),
)
assert verify("action", bad_pipe_count, pipe_write_receipt,
              request=pipe_write_request) != 0
bad_pipe_capability = tampered(
    "bad-pipe-capability.json",
    pipe_written_wire.replace("job:stdin:write", "job:pty:write") + "\n",
)
assert verify("action", pipe_write_response, pipe_write_receipt,
              request=bad_pipe_capability) != 0
bad_pipe_job = tampered(
    "bad-pipe-job.json",
    pipe_written_wire.replace(pipe_start["job_id"], "j-" + "0" * 32) + "\n",
)
assert verify("action", pipe_write_response, pipe_write_receipt,
              request=bad_pipe_job) != 0

exchange({
    "id": "verify-pipe-cancel", "protocol_version": 2, "op": "job.cancel",
    "job_id": pipe_start["job_id"], "grace_ms": 0,
    "caps": ["job:signal"],
})

exchange({
    "id": "verify-action-cancel", "protocol_version": 2, "op": "job.cancel",
    "job_id": action_start["job_id"], "grace_ms": 0,
    "caps": ["job:signal"],
})

# Start receipts bind the complete response before receipt metadata. Nothing
# may be inserted into or appended after the canonical receipt suffix.
start_extra_field = tampered(
    "bad-start-extra-field.json",
    start_raw[:-2] + ',"unauthenticated":true}\n',
)
assert verify("start", start_extra_field, start_receipt) != 0
start_trailing_object = tampered(
    "bad-start-trailing-object.json",
    start_raw + '{"unauthenticated":true}\n',
)
assert verify("start", start_trailing_object, start_receipt) != 0
start_nul_trailing_object = tampered_bytes(
    "bad-start-nul-trailing-object.json",
    start_raw.encode() + b'\x00{"unauthenticated":true}\n',
)
assert verify("start", start_nul_trailing_object, start_receipt) != 0

start_mutations = {
    "id": "forged-start-id",
    "protocol_version": 1,
    "op": "job.get",
    "job_id": "j-" + "0" * 32,
    "accepted": False,
    "state": "done",
    "terminal": True,
    "background": False,
    "pty": True,
}
for field, forged in start_mutations.items():
    changed = dict(start)
    changed[field] = forged
    path = tampered(
        f"bad-start-{field}.json",
        json.dumps(changed, separators=(",", ":")) + "\n",
    )
    assert verify("start", path, start_receipt) != 0, field

# Identity, phase, request, response, receipt refs, start link, and stream
# bytes are independently bound. Every mutation must fail closed.
assert verify("terminal", terminal_response, terminal_receipt, "0" * 64) != 0
assert verify("terminal", start_response, start_receipt) != 0
bad_request = tampered("bad-request.json", wait_wire.replace("verify-wait", "verify-wait-x") + "\n")
assert verify("terminal", terminal_response, terminal_receipt, request=bad_request) != 0
bad_nul_request = tampered_bytes(
    "bad-nul-request.json", wait_wire.encode() + b'\x00{"ignored":true}\n')
assert verify("terminal", terminal_response, terminal_receipt,
              request=bad_nul_request) != 0
bad_job = tampered("bad-job.json", terminal_raw.replace(start["job_id"], "j-" + "0" * 32))
assert verify("terminal", bad_job, terminal_receipt) != 0
bad_ref = tampered("bad-ref.json", terminal_raw.replace(terminal["receipt_ref"], "foreign.dsse"))
assert verify("terminal", bad_ref, terminal_receipt) != 0
bad_receipt_hash = tampered(
    "bad-receipt-hash.json",
    terminal_raw.replace(terminal["receipt_blake3"], "4" * 64),
)
assert verify("terminal", bad_receipt_hash, terminal_receipt) != 0
bad_receipt_phase = tampered(
    "bad-receipt-phase.json",
    terminal_raw.replace('"receipt_phase":"terminal"', '"receipt_phase":"start"'),
)
assert verify("terminal", bad_receipt_phase, terminal_receipt) != 0
forged_pubkey = terminal["receipt_pubkey"].upper()
assert forged_pubkey != terminal["receipt_pubkey"]
bad_receipt_pubkey = tampered(
    "bad-receipt-pubkey.json",
    terminal_raw.replace(terminal["receipt_pubkey"], forged_pubkey),
)
assert verify("terminal", bad_receipt_pubkey, terminal_receipt) != 0
bad_start = tampered("bad-start.json", terminal_raw.replace(start["receipt_ref"], "foreign-start.dsse"))
assert verify("terminal", bad_start, terminal_receipt) != 0
bad_hash = tampered("bad-stream-hash.json", terminal_raw.replace(terminal["stdout_blake3"], "0" * 64))
assert verify("terminal", bad_hash, terminal_receipt) != 0
bad_rc = tampered("bad-rc.json", terminal_raw.replace('"rc":0', '"rc":1', 1))
assert verify("terminal", bad_rc, terminal_receipt) != 0
duplicate_state = tampered(
    "duplicate-state.json", terminal_raw.replace('"state":"done"', '"state":"done","state":"running"', 1),
)
assert verify("terminal", duplicate_state, terminal_receipt) != 0

# Every non-receipt field belongs to the signed terminal response, including
# request framing and operation-specific fields such as timed_out.
operational_mutations = {
    "id": "forged-terminal-id",
    "protocol_version": 1,
    "op": "job.get",
    "job_id": "j-" + "0" * 32,
    "state": "running",
    "terminal": False,
    "background": False,
    "pty": True,
    "cancel_requested": True,
    "kill_sent": True,
    "rc": 1,
    "signal": 9,
    "quiescent": False,
    "quiescence_forced": True,
    "output_limit": True,
    "output_limit_bytes": terminal["output_limit_bytes"] + 1,
    "stdout_ref": "forged-stdout.bin",
    "stdout_blake3": "1" * 64,
    "stdout_bytes": terminal["stdout_bytes"] + 1,
    "stderr_ref": "forged-stderr.bin",
    "stderr_blake3": "2" * 64,
    "stderr_bytes": terminal["stderr_bytes"] + 1,
    "start_receipt_ref": "forged-start.dsse",
    "start_receipt_blake3": "3" * 64,
    "start_request_blake3": "4" * 64,
    "timed_out": True,
}
for field, forged in operational_mutations.items():
    changed = dict(terminal)
    changed[field] = forged
    path = tampered(
        f"bad-terminal-{field}.json",
        json.dumps(changed, separators=(",", ":")) + "\n",
    )
    assert verify("terminal", path, terminal_receipt) != 0, field

changed_cancel = dict(terminal_cancel)
changed_cancel["cancel_applied"] = True
bad_cancel_applied = tampered(
    "bad-terminal-cancel_applied.json",
    json.dumps(changed_cancel, separators=(",", ":")) + "\n",
)
assert verify("terminal", bad_cancel_applied, cancel_receipt,
              request=cancel_request) != 0

cas = os.path.join(home, ".jash", "overflow", terminal["stdout_ref"])
original = open(cas, "rb").read()
open(cas, "wb").write(b"X" + original[1:])
assert verify("terminal", terminal_response, terminal_receipt) != 0
open(cas, "wb").write(original)

receipt_text = open(terminal_receipt).read()
bad_media_receipt = tampered(
    "bad-media.dsse", receipt_text.replace("agent-job-receipt", "agent-jab-receipt", 1),
)
assert verify("terminal", terminal_response, bad_media_receipt,
              ref=terminal["receipt_ref"]) != 0
bad_signature_receipt = tampered(
    "bad-signature.dsse", receipt_text.replace('"sig":"', '"sig":"A', 1),
)
assert verify("terminal", terminal_response, bad_signature_receipt,
              ref=terminal["receipt_ref"]) != 0
bad_nul_receipt = tampered_bytes(
    "bad-nul.dsse", receipt_text.encode() + b'\x00{"ignored":true}\n')
assert verify("terminal", terminal_response, bad_nul_receipt,
              ref=terminal["receipt_ref"]) != 0

envelope = json.loads(receipt_text)
payload = base64.b64decode(envelope["payload"]).decode()
for name, mutated in (
    ("schema", payload.replace("jash.agent-job-receipt.v1", "jash.agent-job-receipt.v0", 1)),
    ("phase", payload.replace('"phase":"terminal"', '"phase":"start"', 1)),
):
    changed = dict(envelope)
    changed["payload"] = base64.b64encode(mutated.encode()).decode()
    path = tampered(f"bad-{name}.dsse", json.dumps(changed, separators=(",", ":")) + "\n")
    assert verify("terminal", terminal_response, path,
                  ref=terminal["receipt_ref"]) != 0

start_original = open(start_receipt, "rb").read()
open(start_receipt, "wb").write(start_original[:-2] + b"X\n")
assert verify("terminal", terminal_response, terminal_receipt) != 0
open(start_receipt, "wb").write(start_original)

proc.stdin.close()
extra = proc.stdout.read()
stderr = proc.stderr.read()
assert proc.wait(timeout=20) == 0, stderr
assert not extra.strip(), extra
PY

printf '%s\n' "libjef job receipt: PASS"
